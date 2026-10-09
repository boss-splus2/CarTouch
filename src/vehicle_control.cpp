/**
 * vehicle_control.cpp - Vehicle control implementation
 *
 * Commands are resolved via ActiveProfileManager (built-in DBC or a
 * custom Learned/Manual profile) instead of hardcoded CAN IDs. See
 * CarTouch_SPEC.md.
 *
 * There is exactly ONE dispatch path: every command, including the
 * dedicated one-shot verification send, is resolved through
 * ActiveProfileManager and then funneled through
 * _sendResolvedMessage(), which applies the Listen-Only guard, the base
 * rate limit and (via _execute) the duty-cycle limit. There is no
 * hardcoded-CAN-ID / legacy override path that could bypass the
 * UNVERIFIED/VERIFIED safety gate.
 */

#include "vehicle_control.h"
#include "custom_vehicle.h"

// RAII lock for VehicleControl::_mutex (recursive). Used for every access to
// the shared state - not only the send path - because commands can come from
// the loop task AND the async web task (learn-mode verification), and
// _lastErrorMessage is an Arduino String that must never be written and read
// at the same time.
namespace {
struct CtCtrlLock {
    SemaphoreHandle_t m;
    explicit CtCtrlLock(SemaphoreHandle_t h) : m(h) {
        if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    }
    ~CtCtrlLock() {
        if (m) xSemaphoreGiveRecursive(m);
    }
};
}  // namespace

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Constructor
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

VehicleControl::VehicleControl(CANService& canService, ActiveProfileManager& profileManager)
    : _can(canService), _profileManager(profileManager) {
    _mutex              = xSemaphoreCreateRecursiveMutex();
    _lastError          = 0;
    _lastErrorMessage   = "";
    _lastCommandTime     = 0;

}

void VehicleControl::begin() {
    Serial.println("[CTRL] Vehicle Control ready (profile-driven)");
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Message dispatch
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

CanBusId VehicleControl::selectedBus() const {
    return ctSanitizeBusIndex(getConfig()->vehicleCanBus) == 1 ? CAN_BUS_2 : CAN_BUS_1;
}

static bool configListenOnlyFor(CanBusId bus) {
    const AppConfig* cfg = getConfig();
    // CAN1 = TWAI (listenOnlyMode); CAN2 = MCP2515 (stored in can1ListenOnly,
    // an internal name kept for NVS/API compatibility).
    return bus == CAN_BUS_2 ? cfg->can1ListenOnly : cfg->listenOnlyMode;
}

bool VehicleControl::isListenOnlyForSelectedBus() {
    const CanBusId bus = selectedBus();
    return configListenOnlyFor(bus) || _can.isListenOnlyActive(bus);
}

bool VehicleControl::_sendResolvedMessage(const CanMessage& msg) {
    const CanBusId bus = selectedBus();
    const char* busName = bus == CAN_BUS_2 ? "CAN2" : "CAN1";

    // Single firmware call site for the shared, unit-tested TX admission,
    // rate-limit and send chain; do not duplicate these checks here.
    const CtVehicleTxResult result = ctVehicleTxSend(
        _can, bus, configListenOnlyFor(bus), msg, millis(), _lastCommandTime,
        MIN_COMMAND_INTERVAL_MS);

    switch (result) {
        case CT_VTX_SENT:
            Serial.printf("[CTRL] Command sent on %s: ID=0x%03lX, data=", busName,
                          (unsigned long)msg.id);
            for (int i = 0; i < msg.length; i++) Serial.printf("%02X ", msg.data[i]);
            Serial.println();
            _lastError = 0;
            _lastErrorMessage = "";
            return true;
        case CT_VTX_LISTEN_ONLY:
            Serial.printf("[CTRL] %s is in Listen-Only mode - command not sent\n", busName);
            _lastError = 1;
            _lastErrorMessage = "Listen-Only mode is active";
            break;
        case CT_VTX_NOT_READY:
            Serial.printf("[CTRL] %s is not ready - command not sent\n", busName);
            _lastError = 2;
            _lastErrorMessage = "Selected CAN channel is not ready";
            break;
        case CT_VTX_RATE_LIMITED:
            Serial.println("[CTRL] Command rejected - rate limit");
            _lastError = 3;
            _lastErrorMessage = "Commands sent too rapidly (rate limit)";
            break;
        case CT_VTX_INVALID_FRAME:
            Serial.printf("[CTRL] Invalid frame for %s - command not sent\n", busName);
            _lastError = 2;
            _lastErrorMessage = "Invalid CAN frame (ID or length)";
            break;
        case CT_VTX_SEND_FAILED:
        default:
            _lastError = 2;
            _lastErrorMessage = "CAN Bus send failed";
            break;
    }
    return false;
}

CtDutyState* VehicleControl::_dutyStateFor(ActuatorClass cls) {
    switch (cls) {
        case ACTUATOR_WINDOW:  return &_windowDuty;
        case ACTUATOR_SUNROOF: return &_sunroofDuty;
        case ACTUATOR_MIRROR:  return &_mirrorDuty;
        default:                return nullptr;
    }
}

VehicleControl::ActuatorClass VehicleControl::_classifyLabel(const char* label) {
    if (!label) return ACTUATOR_NONE;
    // Prefixes match the CMD_LABEL_* constants in custom_vehicle.h.
    // A free-text custom label that doesn't use these prefixes will not
    // be classified here, and will only get the base rate limit above.
    if (strncmp(label, "window_",  7) == 0) return ACTUATOR_WINDOW;
    if (strncmp(label, "all_windows_", 12) == 0) return ACTUATOR_WINDOW;
    if (strncmp(label, "sunroof_", 8) == 0) return ACTUATOR_SUNROOF;
    if (strncmp(label, "mirror_",  7) == 0) return ACTUATOR_MIRROR;
    return ACTUATOR_NONE;
}

bool VehicleControl::_checkDutyCycle(ActuatorClass cls, String& outErrorReason) {
    CtDutyState* state = _dutyStateFor(cls);
    if (!state) return true;  // ACTUATOR_NONE: no limit applies

    const CtDutyResult result = ctDutyCheck(state, millis());
    if (result == CT_DUTY_OK) return true;
    outErrorReason = result == CT_DUTY_COOLDOWN
        ? "This component needs a brief rest after recent repeated use"
        : "Too many activations in a short period - please wait a few seconds";
    return false;
}

void VehicleControl::_recordDutyCycle(ActuatorClass cls) {
    CtDutyState* state = _dutyStateFor(cls);
    if (state) ctDutyRecord(state, millis());
}

bool VehicleControl::_execute(const char* label, String& outErrorReason, bool forVerification, int8_t explicitActuatorClass) {
    CtCtrlLock lock(_mutex);
    // Runs before ActiveProfileManager resolution, since the goal is
    // protecting the physical motor regardless of which command is
    // requested.
    ActuatorClass cls = explicitActuatorClass >= 0 ? (ActuatorClass)explicitActuatorClass : _classifyLabel(label);
    if (forVerification && explicitActuatorClass < 0) {
        // Legacy active-profile verification path is intentionally conservative.
        // Profile-specific verification below supplies explicit metadata.
        cls = ACTUATOR_NONE;
    }
    if (cls != ACTUATOR_NONE) {
        if (!_checkDutyCycle(cls, outErrorReason)) {
            _lastError = 4;
            _lastErrorMessage = outErrorReason;
            return false;
        }
    }

    // Primary path: resolve through ActiveProfileManager. Normal
    // dispatch always keeps the CMD_VERIFIED gate (resolveCommand());
    // only the dedicated one-shot verification flow bypasses it
    // (resolveCommandForVerification()), while every check below
    // (Listen-Only, rate limit, duty-cycle) still applies either way.
    CanMessage msg;
    bool resolved = forVerification
        ? _profileManager.resolveCommandForVerification(label, msg, outErrorReason)
        : _profileManager.resolveCommand(label, msg, outErrorReason);
    if (!resolved) {
        return false;
    }

    bool sent = _sendResolvedMessage(msg);
    if (sent && cls != ACTUATOR_NONE) _recordDutyCycle(cls);
    return sent;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Public execute API
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

bool VehicleControl::executeCommand(const char* commandLabel) {
    String reason;
    return executeCommand(commandLabel, reason);
}

bool VehicleControl::executeCommand(const char* commandLabel, String& outErrorReason) {
    CtCtrlLock lock(_mutex);
    bool result = _execute(commandLabel, outErrorReason);
    if (!result && outErrorReason.length() > 0) {
        _lastErrorMessage = outErrorReason;
        Serial.printf("[CTRL] Command '%s' failed: %s\n",
                      commandLabel, outErrorReason.c_str());
    }
    return result;
}

bool VehicleControl::executeCommandForVerification(const char* commandLabel, String& outErrorReason) {
    CtCtrlLock lock(_mutex);
    bool result = _execute(commandLabel, outErrorReason, /*forVerification=*/true);
    if (!result && outErrorReason.length() > 0) {
        _lastErrorMessage = outErrorReason;
        Serial.printf("[CTRL] Verification send '%s' failed: %s\n",
                      commandLabel, outErrorReason.c_str());
    }
    return result;
}bool VehicleControl::executeCommandForVerification(uint8_t profileIndex, const char* commandLabel, String& outErrorReason) {
    CtCtrlLock lock(_mutex);
    CommandActuatorClass meta = COMMAND_ACTUATOR_UNKNOWN;
    if (!_profileManager.getCommandActuatorClass(profileIndex, commandLabel, meta) ||
        meta == COMMAND_ACTUATOR_UNKNOWN) {
        outErrorReason = "Command actuator type is unknown; re-create this command before verification";
        return false;
    }
    CanMessage msg;
    if (!_profileManager.resolveCommandForVerification(profileIndex, commandLabel, msg, outErrorReason)) return false;
    ActuatorClass cls = ACTUATOR_NONE;
    switch (meta) {
        case COMMAND_ACTUATOR_WINDOW: cls = ACTUATOR_WINDOW; break;
        case COMMAND_ACTUATOR_SUNROOF: cls = ACTUATOR_SUNROOF; break;
        case COMMAND_ACTUATOR_MIRROR: cls = ACTUATOR_MIRROR; break;
        default: cls = ACTUATOR_NONE; break;
    }
    if (cls != ACTUATOR_NONE && !_checkDutyCycle(cls, outErrorReason)) return false;
    bool sent = _sendResolvedMessage(msg);
    if (sent && cls != ACTUATOR_NONE) _recordDutyCycle(cls);
    return sent;
}


// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Convenience wrappers
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool VehicleControl::lockAllDoors() {
    return executeCommand(CMD_LABEL_LOCK_ALL);
}

bool VehicleControl::unlockAllDoors() {
    return executeCommand(CMD_LABEL_UNLOCK_ALL);
}

bool VehicleControl::unlockDriverDoor() {
    return executeCommand(CMD_LABEL_UNLOCK_DRIVER);
}

bool VehicleControl::windowUp(uint8_t window) {
    if (window > 3) return false;
    static const char* labels[4] = {
        CMD_LABEL_WINDOW_FL_UP, CMD_LABEL_WINDOW_FR_UP,
        CMD_LABEL_WINDOW_RL_UP, CMD_LABEL_WINDOW_RR_UP
    };
    return executeCommand(labels[window]);
}

bool VehicleControl::windowDown(uint8_t window) {
    if (window > 3) return false;
    static const char* labels[4] = {
        CMD_LABEL_WINDOW_FL_DOWN, CMD_LABEL_WINDOW_FR_DOWN,
        CMD_LABEL_WINDOW_RL_DOWN, CMD_LABEL_WINDOW_RR_DOWN
    };
    return executeCommand(labels[window]);
}

bool VehicleControl::allWindowsUp() {
    return executeCommand(CMD_LABEL_ALL_WINDOWS_UP);
}

bool VehicleControl::allWindowsDown() {
    return executeCommand(CMD_LABEL_ALL_WINDOWS_DOWN);
}

bool VehicleControl::sunroofOpen() {
    return executeCommand(CMD_LABEL_SUNROOF_OPEN);
}

bool VehicleControl::sunroofClose() {
    return executeCommand(CMD_LABEL_SUNROOF_CLOSE);
}

bool VehicleControl::sunroofTilt() {
    return executeCommand(CMD_LABEL_SUNROOF_TILT);
}

bool VehicleControl::trunkOpen() {
    return executeCommand(CMD_LABEL_TRUNK_OPEN);
}

bool VehicleControl::trunkLock() {
    return executeCommand(CMD_LABEL_TRUNK_LOCK);
}

bool VehicleControl::foldMirrors() {
    return executeCommand(CMD_LABEL_MIRROR_FOLD);
}

bool VehicleControl::unfoldMirrors() {
    return executeCommand(CMD_LABEL_MIRROR_UNFOLD);
}

bool VehicleControl::alarmArm() {
    return executeCommand(CMD_LABEL_ALARM_ARM);
}

bool VehicleControl::alarmDisarm() {
    return executeCommand(CMD_LABEL_ALARM_DISARM);
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Stop all
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

bool VehicleControl::stopAll() {
    // Profile-driven only. "stop_all" must be defined by the active
    // profile (built-in DBC or a verified custom command) exactly like
    // any other label - so this goes through resolveCommand()'s
    // CMD_VERIFIED gate, the Listen-Only guard and the base rate limit.
    // If no such command exists, nothing is sent; we never emit an
    // all-zero frame on a guessed CAN ID.
    CtCtrlLock lock(_mutex);
    String reason;
    bool ok = executeCommand("stop_all", reason);
    if (!ok) {
        _lastErrorMessage = reason.length() > 0
            ? reason
            : String("No 'stop_all' command is defined by the active profile");
    }
    return ok;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Error accessors
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

uint8_t VehicleControl::getLastError() {
    CtCtrlLock lock(_mutex);
    return _lastError;
}

String VehicleControl::getLastErrorMessage() {
    CtCtrlLock lock(_mutex);
    return _lastErrorMessage;  // copied while locked
}
