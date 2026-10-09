/**
 * obd2_reader.cpp - OBD-II reader implementation
 *
 * ISO 15765-4 (CAN 11-bit). Functional request ID 0x7DF, response IDs 0x7E8-0x7EF.
 */

#include "obd2_reader.h"
#include "ct_obd_parser.h"
#include "ct_time.h"
#include "ct_obd_validity.h"
#include "ct_obd_formulas.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ OBD-II constants
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

#define OBD_REQUEST_ID 0x7DF    // Broadcast request

#define OBD_MODE_CURRENT   0x01  // Show current data
#define OBD_MODE_FREEZE    0x02  // Freeze frame data
#define OBD_MODE_DTC       0x03  // Read DTCs
#define OBD_MODE_CLEAR_DTC 0x04  // Clear DTCs

#define PID_SUPPORTED_1 0x00  // Supported PIDs 0x01-0x20
#define PID_SUPPORTED_2 0x20  // Supported PIDs 0x21-0x40
#define PID_SUPPORTED_3 0x40  // Supported PIDs 0x41-0x60

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Constructor
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

OBD2Reader::OBD2Reader(CANService& canService)
    : _can(canService), _bus(CAN_BUS_1), _rxSubscribed(false) {
    _lastError        = 0;
    _lastRequestTime   = 0;
    _requestInterval    = 50;  // Minimum spacing between requests (ms)

    _pollState          = OBD_POLL_IDLE;
    _pollIndex          = 0;
    _pollWaitStartMs     = 0;
    _hasCompletedRound   = false;
    for (uint8_t i = 0; i < _POLL_PID_COUNT; ++i) { _lastAnswerMs[i] = 0; _everAnswered[i] = false; }
    _pollIntervalMs       = 200;  // Spacing between completed rounds
    _lastRoundStartMs      = 0;
    _diagnosticState = OBD_DIAG_IDLE;
    _diagnosticOperation = OBD_DIAG_OP_NONE;
    _diagnosticStartMs = 0;
    _diagnosticError = 0;
    _diagnosticResponseCode = 0;
    _dtcCount = 0;
    memset(_dtcList, 0, sizeof(_dtcList));
    memset(_dtcPayload, 0, sizeof(_dtcPayload));
    memset(&_dtcReassembly, 0, sizeof(_dtcReassembly));
}

bool OBD2Reader::setCanBus(CanBusId bus) {
    if (bus != CAN_BUS_1 && bus != CAN_BUS_2) return false;
    if (_rxSubscribed) _can.unsubscribeRx(_bus, CAN_RX_OBD);
    _bus = bus;
    _pollState = OBD_POLL_IDLE;
    _pollIndex = 0;
    _hasCompletedRound = false;
    for (uint8_t i = 0; i < _POLL_PID_COUNT; ++i) _everAnswered[i] = false;
    if (_rxSubscribed) _can.subscribeRx(_bus, CAN_RX_OBD);
    return true;
}

bool OBD2Reader::canTransmit() {
    return _can.isActive(_bus) && !_can.isListenOnlyActive(_bus);
}

void OBD2Reader::begin() {
    _rxSubscribed = _can.subscribeRx(_bus, CAN_RX_OBD);
    _can.flushRx(_bus, CAN_RX_OBD);
    Serial.println("[OBD2] Reader ready");
}

static bool receiveObdFrame(CANService& can, CanBusId bus,
                            CanMessage& message, uint32_t timeoutMs) {
    const uint32_t start = millis();
    do {
        CanRxFrame frame;
        if (can.receiveRx(bus, CAN_RX_OBD, frame)) {
            message = frame.message;
            return true;
        }
        if (ctElapsedAtLeast(millis(), start, timeoutMs)) break;
        delay(1);
    } while (true);
    return false;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Single-PID request [BLOCKING]
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool OBD2Reader::requestPID(uint8_t pid, ObdResponse& response) {
    uint32_t now = millis();
    if (now - _lastRequestTime < _requestInterval) {
        delay(_requestInterval - (now - _lastRequestTime));
    }

    // Request frame: [Mode, PID, 0x00 x 5]
    CanMessage request;
    request.id          = OBD_REQUEST_ID;
    request.isExtended  = false;
    request.isRemote    = false;
    request.length      = 8;
    request.data[0]     = 0x02;  // Valid byte count
    request.data[1]     = OBD_MODE_CURRENT;
    request.data[2]     = pid;
    request.data[3]     = 0x00;
    request.data[4]     = 0x00;
    request.data[5]     = 0x00;
    request.data[6]     = 0x00;
    request.data[7]     = 0x00;

    if (!_can.sendMessage(_bus, request)) {
        _lastError = 1;
        response.success = false;
        return false;
    }

    _lastRequestTime = millis();

    CanMessage reply;
    uint32_t startMs  = millis();
    bool     received = false;

    // Wrap-safe timeout: unsigned subtraction stays correct when millis()
    // rolls over, unlike the previous "millis() < millis() + timeout" form
    // (which would stall until the next rollover near the wrap point).
    while ((uint32_t)(millis() - startMs) < 200) {
        if (receiveObdFrame(_can, _bus, reply, 50)) {
            if (ctIsObdReplyFrame(reply.id, reply.isExtended, reply.isRemote)) {
                if (reply.length >= 3 &&
                    reply.data[1] == (OBD_MODE_CURRENT + 0x40) &&
                    reply.data[2] == pid) {
                    received = true;
                    break;
                }
            }
        }
    }

    if (!received) {
        _lastError = 2;
        response.success = false;
        return false;
    }

    CtObdSingleFrame parsed;
    if (!ctParseObdSingleFrame(reply.data, reply.length,
                               (uint8_t)(OBD_MODE_CURRENT + 0x40), pid, parsed) ||
        parsed.payloadLength < 3) {
        _lastError = 3;
        response.success = false;
        return false;
    }

    const uint8_t dataLength = (uint8_t)(parsed.payloadLength - 2);
    if (dataLength > sizeof(response.data)) {
        _lastError = 3;
        response.success = false;
        return false;
    }

    response.pid = pid;
    response.length = dataLength;
    response.success = true;
    response.timestamp = millis();
    memcpy(response.data, &reply.data[parsed.dataOffset], dataLength);

    _lastError = 0;
    return true;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Single-value readers [BLOCKING]
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

uint16_t OBD2Reader::readEngineRPM() {
    ObdResponse response;
    if (!requestPID(OBD_PID_ENGINE_RPM, response)) return 0;
    if (response.length >= 2) {
        return ctObdEngineRpm(response.data[0], response.data[1]);
    }
    return 0;
}

uint8_t OBD2Reader::readVehicleSpeed() {
    ObdResponse response;
    if (!requestPID(OBD_PID_VEHICLE_SPEED, response)) return 0;
    if (response.length >= 1) {
        return ctObdVehicleSpeed(response.data[0]);
    }
    return 0;
}

int8_t OBD2Reader::readCoolantTemp() {
    ObdResponse response;
    if (!requestPID(OBD_PID_COOLANT_TEMP, response)) return -40;
    if (response.length >= 1) {
        return ctObdCoolantTemp(response.data[0]);
    }
    return -40;
}

uint8_t OBD2Reader::readThrottlePosition() {
    ObdResponse response;
    if (!requestPID(OBD_PID_THROTTLE_POS, response)) return 0;
    if (response.length >= 1) {
        return ctObdPercent(response.data[0]);
    }
    return 0;
}

uint8_t OBD2Reader::readFuelLevel() {
    ObdResponse response;
    if (!requestPID(OBD_PID_FUEL_LEVEL, response)) return 0;
    if (response.length >= 1) {
        return ctObdPercent(response.data[0]);
    }
    return 0;
}

uint16_t OBD2Reader::readEngineRuntime() {
    ObdResponse response;
    if (!requestPID(OBD_PID_RUNTIME, response)) return 0;
    if (response.length >= 2) {
        return ctObdEngineRuntime(response.data[0], response.data[1]);
    }
    return 0;
}

float OBD2Reader::readControlModuleVoltage() {
    ObdResponse response;
    if (!requestPID(OBD_PID_BATTERY_VOLT, response)) return 0.0f;
    if (response.length >= 2) {
        return ctObdControlModuleVoltage(response.data[0], response.data[1]);
    }
    return 0.0f;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Read all PIDs [BLOCKING]
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

// No longer called from the main loop() - use update() + getLatestData().

void OBD2Reader::readAllPIDs(VehicleData& data) {
    data.engineRPM = readEngineRPM();
    delay(10);
    data.vehicleSpeed = readVehicleSpeed();
    delay(10);
    data.coolantTemp = readCoolantTemp();
    delay(10);
    data.throttlePos = readThrottlePosition();
    delay(10);
    data.fuelLevel = readFuelLevel();
    delay(10);
    data.engineRuntime = readEngineRuntime();
    delay(10);
    data.batteryVoltage = readControlModuleVoltage();
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Non-blocking poll state machine
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// Round order: RPM -> Speed -> Coolant -> Throttle -> Fuel -> Runtime -> done
// -> pause _pollIntervalMs -> next round. No delay() and no wait loop
// anywhere here; each update() call does at most one send or one
// non-blocking receive check, then returns immediately.

void OBD2Reader::_applyPidToData(uint8_t pid, const ObdResponse& resp, VehicleData& data) {
    if (!resp.success) {
        if (pid == OBD_PID_BATTERY_VOLT) data.batteryVoltage = 0.0f;
        return;
    }

    switch (pid) {
        case OBD_PID_ENGINE_RPM:
            if (resp.length >= 2)
                data.engineRPM = ctObdEngineRpm(resp.data[0], resp.data[1]);
            break;
        case OBD_PID_VEHICLE_SPEED:
            if (resp.length >= 1) data.vehicleSpeed = ctObdVehicleSpeed(resp.data[0]);
            break;
        case OBD_PID_COOLANT_TEMP:
            if (resp.length >= 1) data.coolantTemp = ctObdCoolantTemp(resp.data[0]);
            break;
        case OBD_PID_THROTTLE_POS:
            if (resp.length >= 1) data.throttlePos = ctObdPercent(resp.data[0]);
            break;
        case OBD_PID_FUEL_LEVEL:
            if (resp.length >= 1) data.fuelLevel = ctObdPercent(resp.data[0]);
            break;
        case OBD_PID_RUNTIME:
            if (resp.length >= 2)
                data.engineRuntime = ctObdEngineRuntime(resp.data[0], resp.data[1]);
            break;
        case OBD_PID_BATTERY_VOLT:
            if (resp.length >= 2)
                data.batteryVoltage = ctObdControlModuleVoltage(resp.data[0], resp.data[1]);
            break;
    }
}

// Poll order - must stay in sync with _applyPidToData
static const uint8_t OBD_POLL_PID_TABLE[7] = {
    OBD_PID_ENGINE_RPM, OBD_PID_VEHICLE_SPEED, OBD_PID_COOLANT_TEMP,
    OBD_PID_THROTTLE_POS, OBD_PID_FUEL_LEVEL, OBD_PID_RUNTIME,
    OBD_PID_BATTERY_VOLT
};

void OBD2Reader::_pollStartNextPid() {
    uint8_t pid = OBD_POLL_PID_TABLE[_pollIndex];

    CanMessage request;
    request.id          = OBD_REQUEST_ID;
    request.isExtended  = false;
    request.isRemote    = false;
    request.length      = 8;
    request.data[0]     = 0x02;
    request.data[1]     = OBD_MODE_CURRENT;
    request.data[2]     = pid;
    request.data[3]     = 0x00;
    request.data[4]     = 0x00;
    request.data[5]     = 0x00;
    request.data[6]     = 0x00;
    request.data[7]     = 0x00;

    if (_can.sendMessage(_bus, request)) {
        _pollWaitStartMs = millis();
        _pollState = OBD_POLL_WAITING;
    } else {
        // Send failed - skip this PID rather than stalling the round
        if (pid == OBD_PID_BATTERY_VOLT) _pendingData.batteryVoltage = 0.0f;
        _pollIndex++;
        _pollState = (_pollIndex >= _POLL_PID_COUNT) ? OBD_POLL_DONE : OBD_POLL_SENDING;
    }
}

void OBD2Reader::_pollCheckResponse() {
    uint8_t pid = OBD_POLL_PID_TABLE[_pollIndex];

    // Drain a bounded number of queued frames per call. The main loop runs
    // roughly every 5-10 ms, while a car bus delivers hundreds to thousands of
    // frames per second: reading ONE frame per call left the reply stuck
    // behind up to 64 stale frames (longer than the 200 ms timeout), so on a
    // busy bus almost every PID timed out.
    for (uint8_t n = 0; n < 32; ++n) {
        CanRxFrame rxFrame;
        if (!_can.receiveRx(_bus, CAN_RX_OBD, rxFrame)) break;
        const CanMessage& reply = rxFrame.message;

        if (!ctIsObdReplyFrame(reply.id, reply.isExtended, reply.isRemote)) continue;

        CtObdSingleFrame parsed;
        if (!ctParseObdSingleFrame(reply.data, reply.length,
                                   (uint8_t)(OBD_MODE_CURRENT + 0x40), pid, parsed) ||
            parsed.payloadLength < 3) {
            continue;  // e.g. negative response or another PID's reply
        }

        const uint8_t dataLength = (uint8_t)(parsed.payloadLength - 2);
        if (dataLength > 6) continue;

        ObdResponse resp;
        resp.pid = pid;
        resp.length = dataLength;
        resp.success = true;
        resp.timestamp = millis();
        memcpy(resp.data, &reply.data[parsed.dataOffset], dataLength);
        _applyPidToData(pid, resp, _pendingData);
        _lastAnswerMs[_pollIndex] = millis();
        _everAnswered[_pollIndex] = true;

        _pollIndex++;
        _pollState = (_pollIndex >= _POLL_PID_COUNT) ? OBD_POLL_DONE : OBD_POLL_SENDING;
        return;
    }

    // The timeout is evaluated on EVERY call. Previously it ran only when the
    // queue was empty, so continuous bus traffic could keep the state machine
    // in WAITING forever after a lost or rejected request.
    if ((uint32_t)(millis() - _pollWaitStartMs) > 200) {
        ObdResponse timeoutResp;
        timeoutResp.success = false;
        _applyPidToData(pid, timeoutResp, _pendingData);  // Previous value kept

        _pollIndex++;
        _pollState = (_pollIndex >= _POLL_PID_COUNT) ? OBD_POLL_DONE : OBD_POLL_SENDING;
    }
}

void OBD2Reader::update() {
    if (isDiagnosticBusy()) {
        _updateDiagnostic();
        return;
    }

    uint32_t now = millis();

    switch (_pollState) {
        case OBD_POLL_IDLE:
            if (now - _lastRoundStartMs >= _pollIntervalMs) {
                _pendingData = _hasCompletedRound ? _latestData : VehicleData();
                _pollIndex = 0;
                _lastRoundStartMs = now;
                _pollState = OBD_POLL_SENDING;
            }
            break;

        case OBD_POLL_SENDING:
            if (now - _lastRequestTime >= _requestInterval) {
                _pollStartNextPid();
                _lastRequestTime = now;
            }
            break;

        case OBD_POLL_WAITING:
            _pollCheckResponse();
            break;

        case OBD_POLL_DONE:
            _latestData.engineRPM      = _pendingData.engineRPM;
            _latestData.vehicleSpeed    = _pendingData.vehicleSpeed;
            _latestData.coolantTemp     = _pendingData.coolantTemp;
            _latestData.throttlePos     = _pendingData.throttlePos;
            _latestData.fuelLevel        = _pendingData.fuelLevel;
            _latestData.engineRuntime     = _pendingData.engineRuntime;
            _latestData.batteryVoltage     = _pendingData.batteryVoltage;
            {
                // Same order as OBD_POLL_PID_TABLE.
                static const uint8_t bits[7] = { CT_VD_RPM, CT_VD_SPEED, CT_VD_COOLANT,
                                                 CT_VD_THROTTLE, CT_VD_FUEL, CT_VD_RUNTIME,
                                                 CT_VD_BATTERY };
                uint8_t mask = 0;
                for (uint8_t i = 0; i < _POLL_PID_COUNT; ++i) {
                    if (ctObdValueFresh(_everAnswered[i], _lastAnswerMs[i], now, CT_OBD_STALE_MS)) {
                        mask |= bits[i];
                    }
                }
                _latestData.validMask = mask;
            }
            _hasCompletedRound = true;
            _pollState = OBD_POLL_IDLE;
            break;
    }
}

bool OBD2Reader::getLatestData(VehicleData& outData) {
    if (!_hasCompletedRound) return false;
    outData.engineRPM      = _latestData.engineRPM;
    outData.vehicleSpeed    = _latestData.vehicleSpeed;
    outData.coolantTemp     = _latestData.coolantTemp;
    outData.throttlePos     = _latestData.throttlePos;
    outData.fuelLevel        = _latestData.fuelLevel;
    outData.engineRuntime     = _latestData.engineRuntime;
    outData.batteryVoltage     = _latestData.batteryVoltage;
    outData.validMask          = _latestData.validMask;
    return true;
}

ObdPollState OBD2Reader::getPollState() {
    return _pollState;
}

bool OBD2Reader::isDiagnosticBusy() const {
    return _diagnosticState == OBD_DIAG_READ_WAITING ||
           _diagnosticState == OBD_DIAG_READ_CONSECUTIVE ||
           _diagnosticState == OBD_DIAG_CLEAR_WAITING;
}

bool OBD2Reader::_startDiagnosticRequest(uint8_t service,
                                         ObdDiagnosticOperation operation) {
    if (isDiagnosticBusy()) return false;

    _diagnosticOperation = operation;
    _diagnosticError = 0;
    _diagnosticResponseCode = 0;
    _dtcCount = 0;
    _lastError = 0;
    _pollState = OBD_POLL_IDLE;

    if (!canTransmit()) {
        _diagnosticError = _lastError = 1;
        _diagnosticState = OBD_DIAG_FAILED;
        return false;
    }

    _can.flushRx(_bus, CAN_RX_OBD);
    CanMessage request = {};
    request.id = OBD_REQUEST_ID;
    request.length = 8;
    request.data[0] = 0x01;
    request.data[1] = service;
    if (!_can.sendMessage(_bus, request)) {
        _diagnosticError = _lastError = 1;
        _diagnosticState = OBD_DIAG_FAILED;
        return false;
    }

    _diagnosticStartMs = millis();
    _diagnosticState = operation == OBD_DIAG_OP_READ_DTCS
        ? OBD_DIAG_READ_WAITING : OBD_DIAG_CLEAR_WAITING;
    return true;
}

bool OBD2Reader::startDtcRead() {
    return _startDiagnosticRequest(OBD_MODE_DTC, OBD_DIAG_OP_READ_DTCS);
}

bool OBD2Reader::startDtcClear() {
    return _startDiagnosticRequest(OBD_MODE_CLEAR_DTC, OBD_DIAG_OP_CLEAR_DTCS);
}

void OBD2Reader::_finishDtcRead(const uint8_t* payload, uint16_t length) {
    if (!ctParseObdDtcPayload(payload, length, _dtcList,
                              MAX_DTC_COUNT, _dtcCount)) {
        _diagnosticError = _lastError = 3;
        _diagnosticState = OBD_DIAG_FAILED;
        return;
    }
    _diagnosticError = _lastError = 0;
    _diagnosticState = OBD_DIAG_COMPLETE;
}

void OBD2Reader::_updateDiagnostic() {
    if (!isDiagnosticBusy()) return;
    if (!canTransmit()) {
        _diagnosticError = _lastError = 1;
        _diagnosticState = OBD_DIAG_FAILED;
        return;
    }
    if (ctElapsedAtLeast(millis(), _diagnosticStartMs, 1000)) {
        _diagnosticError = _lastError = 2;
        _diagnosticState = OBD_DIAG_FAILED;
        return;
    }

    for (uint8_t count = 0; count < 32; ++count) {
        CanRxFrame rxFrame = {};
        if (!_can.receiveRx(_bus, CAN_RX_OBD, rxFrame)) break;
        const CanMessage& reply = rxFrame.message;
        if (!ctIsObdReplyFrame(reply.id, reply.isExtended, reply.isRemote) ||
            reply.length == 0) continue;

        if (_diagnosticState == OBD_DIAG_CLEAR_WAITING) {
            if (ctParseObdPositiveServiceAck(reply.data, reply.length, 0x44)) {
                _diagnosticError = _lastError = 0;
                _diagnosticState = OBD_DIAG_COMPLETE;
                return;
            }
            uint8_t responseCode = 0;
            if (ctParseObdNegativeResponse(reply.data, reply.length,
                                           OBD_MODE_CLEAR_DTC, responseCode)) {
                _diagnosticResponseCode = responseCode;
                _diagnosticError = _lastError = 3;
                _diagnosticState = OBD_DIAG_FAILED;
                return;
            }
            continue;
        }

        if (_diagnosticState == OBD_DIAG_READ_CONSECUTIVE) {
            if (reply.id != _dtcReassembly.canId ||
                reply.isExtended != _dtcReassembly.isExtended) continue;
            if (!ctIsoTpAppend(reply.data, reply.length, reply.id,
                               reply.isExtended, _dtcPayload, _dtcReassembly)) {
                _diagnosticError = _lastError = 3;
                _diagnosticState = OBD_DIAG_FAILED;
                return;
            }
            if (ctIsoTpComplete(_dtcReassembly)) {
                _finishDtcRead(_dtcPayload, _dtcReassembly.totalLength);
                return;
            }
            continue;
        }

        const uint8_t frameType = (uint8_t)(reply.data[0] & 0xF0u);
        if (frameType == 0x00u) {
            CtObdSingleFrame parsed;
            if (ctParseObdSingleFrame(reply.data, reply.length, 0x43, 0xFF, parsed)) {
                _finishDtcRead(reply.data + 1, parsed.payloadLength);
                return;
            }
            uint8_t responseCode = 0;
            if (ctParseObdNegativeResponse(reply.data, reply.length,
                                           OBD_MODE_DTC, responseCode)) {
                _diagnosticResponseCode = responseCode;
                _diagnosticError = _lastError = 3;
                _diagnosticState = OBD_DIAG_FAILED;
                return;
            }
            continue;
        }

        if (frameType != 0x10u) continue;
        memset(_dtcPayload, 0, sizeof(_dtcPayload));
        if (!ctIsoTpBegin(reply.data, reply.length, reply.id, reply.isExtended,
                          _dtcPayload, sizeof(_dtcPayload), _dtcReassembly) ||
            _dtcPayload[0] != 0x43u) {
            _diagnosticError = _lastError = 3;
            _diagnosticState = OBD_DIAG_FAILED;
            return;
        }

        CanMessage flowControl = {};
        flowControl.id = reply.id - 8u;
        flowControl.length = 8;
        if (!ctBuildIsoTpFlowControl(flowControl.data, sizeof(flowControl.data),
                                     0, 0, 0) ||
            !_can.sendMessage(_bus, flowControl)) {
            _diagnosticError = _lastError = 1;
            _diagnosticState = OBD_DIAG_FAILED;
            return;
        }
        _diagnosticState = OBD_DIAG_READ_CONSECUTIVE;
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ PID support check
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

bool OBD2Reader::isPidSupported(uint8_t pid) {
    ObdResponse response;
    if (!requestPID(PID_SUPPORTED_1, response)) return false;

    // 4-byte bitmask response: bits 31-0 map to PIDs 0x01-0x20
    if (response.length >= 4 && pid >= 0x01 && pid <= 0x20) {
        uint32_t supported = 0;
        for (int i = 0; i < 4; i++) {
            supported = (supported << 8) | response.data[i];
        }
        return (supported >> (32 - pid)) & 1;
    }

    return false;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ DTC read / clear
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

uint8_t OBD2Reader::readDTCs(uint16_t dtcList[], uint8_t maxCount) {
    if (!dtcList || maxCount == 0) {
        _lastError = 3;
        return 0;
    }
    if (maxCount > MAX_DTC_COUNT) maxCount = MAX_DTC_COUNT;

    CanMessage request;
    request.id          = OBD_REQUEST_ID;
    request.isExtended  = false;
    request.isRemote    = false;
    request.length      = 8;
    request.data[0]     = 0x01;
    request.data[1]     = OBD_MODE_DTC;
    request.data[2]     = 0x00;
    request.data[3]     = 0x00;
    request.data[4]     = 0x00;
    request.data[5]     = 0x00;
    request.data[6]     = 0x00;
    request.data[7]     = 0x00;

    if (!_can.sendMessage(_bus, request)) {
        // Listen-Only mode or bus error: the request never reached the bus.
        _lastError = 1;
        return 0;
    }

    uint8_t        payload[1 + (2 * MAX_DTC_COUNT)] = {};
    uint16_t       payloadLength                    = 0;
    bool           complete                         = false;
    const uint32_t startMs                          = millis();

    while (!ctElapsedAtLeast(millis(), startMs, 1000)) {
        CanMessage reply = {};
        if (!receiveObdFrame(_can, _bus, reply, 25)) continue;
        if (!ctIsObdReplyFrame(reply.id, reply.isExtended, reply.isRemote)) continue;

        const uint8_t frameType = (uint8_t)(reply.data[0] & 0xF0u);
        if (frameType == 0x00u) {
            CtObdSingleFrame parsed;
            if (!ctParseObdSingleFrame(reply.data, reply.length, 0x43, 0xFF, parsed) ||
                !ctDtcPayloadHasValidPairLength(parsed.payloadLength)) {
                continue;
            }
            payloadLength = parsed.payloadLength;
            memcpy(payload, reply.data + parsed.dataOffset, payloadLength - 1u);
            payload[0] = 0x43;
            complete = true;
            break;
        }

        if (frameType != 0x10u) continue;

        CtIsoTpReassembly state;
        if (!ctIsoTpBegin(reply.data, reply.length, reply.id, reply.isExtended,
                          payload, sizeof(payload), state) ||
            payload[0] != 0x43u) {
            _lastError = 3;
            return 0;
        }

        // ISO-TP Flow Control is sent only back to the ECU that started this
        // response, through the already selected CAN interface.
        CanMessage flowControl = {};
        flowControl.id = reply.id - 8u;
        flowControl.length = 8;
        if (!ctBuildIsoTpFlowControl(flowControl.data, sizeof(flowControl.data),
                                     0, 0, 0)) {
            _lastError = 3;
            return 0;
        }
        if (!_can.sendMessage(_bus, flowControl)) {
            _lastError = 1;
            return 0;
        }

        bool malformed = false;
        while (!ctIsoTpComplete(state) &&
               !ctElapsedAtLeast(millis(), startMs, 1000)) {
            CanMessage consecutive = {};
            if (!receiveObdFrame(_can, _bus, consecutive, 25)) continue;
            if (consecutive.id != state.canId ||
                consecutive.isExtended != state.isExtended ||
                consecutive.isRemote) continue;
            if (!ctIsoTpAppend(consecutive.data, consecutive.length,
                               consecutive.id, consecutive.isExtended,
                               payload, state)) {
                malformed = true;
                break;
            }
        }
        if (malformed) {
            _lastError = 3;
            return 0;
        }
        if (!ctIsoTpComplete(state)) break;
        payloadLength = state.totalLength;
        complete = true;
        break;
    }

    uint8_t dtcCount = 0;
    if (!complete || !ctParseObdDtcPayload(payload, payloadLength, dtcList,
                                          maxCount, dtcCount)) {
        _lastError = complete ? 3 : 2;
        return 0;
    }

    _lastError = 0;
    return dtcCount;
}

bool OBD2Reader::clearDTCs() {
    CanMessage request;
    request.id          = OBD_REQUEST_ID;
    request.isExtended  = false;
    request.isRemote    = false;
    request.length      = 8;
    request.data[0]     = 0x01;
    request.data[1]     = OBD_MODE_CLEAR_DTC;
    request.data[2]     = 0x00;
    request.data[3]     = 0x00;
    request.data[4]     = 0x00;
    request.data[5]     = 0x00;
    request.data[6]     = 0x00;
    request.data[7]     = 0x00;

    if (!_can.sendMessage(_bus, request)) {
        _lastError = 1;
        return false;
    }

    const uint32_t startMs = millis();
    bool receivedNegativeResponse = false;
    while (!ctElapsedAtLeast(millis(), startMs, 500)) {
        CanMessage reply = {};
        if (!receiveObdFrame(_can, _bus, reply, 25) ||
            !ctIsObdReplyFrame(reply.id, reply.isExtended, reply.isRemote)) {
            continue;
        }

        if (ctParseObdPositiveServiceAck(reply.data, reply.length, 0x44)) {
            _lastError = 0;
            return true;
        }

        uint8_t responseCode = 0;
        if (ctParseObdNegativeResponse(reply.data, reply.length, OBD_MODE_CLEAR_DTC,
                                       responseCode)) {
            receivedNegativeResponse = true;
        }
    }

    _lastError = receivedNegativeResponse ? 3 : 2;
    return false;
}

uint8_t OBD2Reader::getLastError() {
    return _lastError;
}
