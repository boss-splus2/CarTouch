/**
 * learn_engine.cpp - Command-learning engine implementation
 *
 * Reminder: this file must never call _can.sendMessage(). Receive is
 * through Learn's dedicated CANService consumer queue.
 */

#include "learn_engine.h"
#include "error_log.h"
#include "ct_tx_guard.h"

namespace {
class LearnStateGuard {
public:
    explicit LearnStateGuard(SemaphoreHandle_t mutex)
        : _mutex(mutex), _locked(mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {}

    ~LearnStateGuard() {
        if (_locked) xSemaphoreGive(_mutex);
    }

    bool locked() const { return _locked; }

private:
    SemaphoreHandle_t _mutex;
    bool _locked;
};
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Constructor
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

LearnEngine::LearnEngine(CANService& canService)
    : _can(canService),
      _stateMutex(xSemaphoreCreateMutex()),
      _bus(CAN_BUS_1),
      _state(_stateMutex ? LEARN_IDLE : LEARN_ERROR),
      _currentLabel{},
      _currentDisplayName{},
      _phaseStartTime(0),
      _phaseDurationMs(0),
      _baselineCount(0),
      _candidateCount(0),
      _forcedListenOnly(false),
      _previousListenOnlyMode(true),
      _rxSubscribed(false),
      _targetProfileId(255),
      _sessionId(0) {}

bool LearnEngine::setCanBus(CanBusId bus) {
    LearnStateGuard lock(_stateMutex);
    if (!lock.locked()) return false;
    if (bus != CAN_BUS_1 && bus != CAN_BUS_2) return false;
    if (_forcedListenOnly ||
        (_state != LEARN_IDLE && _state != LEARN_ERROR)) return false;
    if (_rxSubscribed) {
        _can.unsubscribeRx(_bus, CAN_RX_LEARN);
        _rxSubscribed = false;
    }
    _bus = bus;
    return true;
}

CanBusId LearnEngine::getCanBus() {
    LearnStateGuard lock(_stateMutex);
    return lock.locked() ? _bus : CAN_BUS_1;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Begin learning
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void LearnEngine::beginLearning(const char* label, const char* displayName, uint8_t targetProfileId) {
    LearnStateGuard lock(_stateMutex);
    if (!lock.locked()) return;
    if (_state != LEARN_IDLE && _state != LEARN_ERROR) {
        getErrorLog()->log(LOG_CAT_LEARN, LOG_WARN, "Learning session already active; refusing to replace it");
        return;
    }
    if (!label || !label[0]) {
        getErrorLog()->log(LOG_CAT_LEARN, LOG_ERROR, "Learning requires a non-empty command label");
        _state = LEARN_ERROR;
        return;
    }

    strncpy(_currentLabel, label, sizeof(_currentLabel) - 1);
    _currentLabel[sizeof(_currentLabel) - 1] = '\0';

    strncpy(_currentDisplayName, displayName ? displayName : label, sizeof(_currentDisplayName) - 1);
    _currentDisplayName[sizeof(_currentDisplayName) - 1] = '\0';

    _baselineCount   = 0;
    _candidateCount    = 0;
    _targetProfileId = targetProfileId;
    ++_sessionId;
    if (_sessionId == 0) ++_sessionId;
    for (int i = 0; i < BASELINE_MAX_IDS; i++) {
        _baseline[i].valid = false;
    }

    // Hardware-level enforcement (see CarTouch_SPEC.md): make sure the TWAI
    // driver is actually running in listen-only before any capture
    // window can open, not just relying on this class never calling
    // sendMessage(). Remember whatever mode was active so cancel() can
    // restore it once the learning session ends.
    // If a previous session already forced Listen-Only and was never cancelled,
    // keep the ORIGINAL mode. Overwriting it here made the next cancel() think
    // Listen-Only was the user's own choice and never restore Normal mode.
    AppConfig* cfg = getConfig();
    bool* configuredListenOnly = _bus == CAN_BUS_1
        ? &cfg->listenOnlyMode : &cfg->can1ListenOnly;
    if (!_forcedListenOnly) {
        _previousListenOnlyMode = *configuredListenOnly;
    }
    _baselineOverflowed = false;

    if (!_can.isActive(_bus)) {
        getErrorLog()->log(LOG_CAT_LEARN, LOG_ERROR, "Selected CAN interface unavailable; aborting learning session");
        _state = LEARN_ERROR;
        return;
    }
    if (!_can.isListenOnlyActive(_bus)) {
        Serial.println("[LEARN] Forcing selected CAN interface into hardware Listen-Only for this learning session...");
        if (!_can.reconfigureMode(_bus, true)) {
            // Fail-safe: the driver is left uninitialized by a failed
            // reconfigure (see CANManager::reconfigureMode), which
            // itself blocks sendMessage() - but we still refuse to
            // proceed into a capture window rather than trust that.
            getErrorLog()->log(LOG_CAT_LEARN, LOG_ERROR, "Failed to force hardware Listen-Only - aborting learning session");
            _state = LEARN_ERROR;
            return;
        }
        *configuredListenOnly = true;
        _forcedListenOnly       = true;
        // Keep the user's own choice for any saveConfig() during this session.
        configSetLearnListenOverride(_bus == CAN_BUS_1 ? 0 : 1, true, _previousListenOnlyMode);
    }

    if (!_can.subscribeRx(_bus, CAN_RX_LEARN)) {
        getErrorLog()->log(LOG_CAT_LEARN, LOG_ERROR, "Could not subscribe Learn Mode to the selected CAN interface");
        _state = LEARN_ERROR;
        return;
    }
    _rxSubscribed = true;

    _state = LEARN_IDLE;

    Serial.printf("[LEARN] Starting learning for label: %s (%s)\n", label, _currentDisplayName);
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Baseline capture
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void LearnEngine::startBaselineCapture() {
    LearnStateGuard lock(_stateMutex);
    if (!lock.locked()) return;
    // Defense in depth (see CarTouch_SPEC.md): beginLearning() already
    // forces hardware Listen-Only, but re-check the driver's actual
    // state here too, in case beginLearning() failed, was skipped, or
    // something else on the device flipped the mode in between.
    if (!_can.isListenOnlyActive(_bus)) {
        getErrorLog()->log(LOG_CAT_LEARN, LOG_ERROR, "startBaselineCapture() refused - CAN driver not in hardware Listen-Only");
        _state = LEARN_ERROR;
        return;
    }

    // Ensure the queue is empty before starting, so stale data doesn't
    // mix with this learning session's data.
    _can.flushRx(_bus, CAN_RX_LEARN);

    _baselineCount = 0;
    _baselineOverflowed = false;
    for (int i = 0; i < BASELINE_MAX_IDS; i++) {
        _baseline[i].valid = false;
    }

    _phaseStartTime    = millis();
    _phaseDurationMs      = LEARN_BASELINE_MS;
    _state                   = LEARN_BASELINE_CAPTURE;

    Serial.println("[LEARN] Baseline capture started - please don't press any button yet");
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Baseline lookup / insert
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

BaselineEntry* LearnEngine::_findOrAddBaseline(uint32_t canId, bool isExtended) {
    for (int i = 0; i < _baselineCount; i++) {
        if (ctSameCanFrameId(_baseline[i].canId, _baseline[i].isExtended,
                             canId, isExtended)) {
            return &_baseline[i];
        }
    }

    if (_baselineCount >= BASELINE_MAX_IDS) {
        // Table capacity reached - this CAN ID is not tracked. It would later
        // look like a brand-new message during the action phase, so remember
        // the overflow and warn (see update()).
        _baselineOverflowed = true;
        return nullptr;
    }

    BaselineEntry* entry = &_baseline[_baselineCount];
    entry->canId       = canId;
    entry->isExtended  = isExtended;
    entry->seenCount      = 0;
    entry->valid             = true;
    _baselineCount++;
    return entry;
}

// Lower value = more likely to be the frame produced by the button press:
// brand-new IDs first, then rarely-seen-in-baseline IDs, then IDs that were
// transmitted only a few times during the action window (a button press sends
// a handful of frames; rolling counters send one per cycle).
static int candidatePriority(const LearnCandidate& c) {
    int b = c.seenCountInBaseline > 999 ? 999 : (int)c.seenCountInBaseline;
    int a = c.seenCountInAction  > 99  ? 99  : (int)c.seenCountInAction;
    return (c.isNewMessage ? 0 : 100000) + b * 100 + a;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Message processing during ACTION_CAPTURE
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void LearnEngine::_processActionMessage(const CanMessage& msg) {
    // Find this CAN ID in the baseline
    BaselineEntry* baseEntry = nullptr;
    for (int i = 0; i < _baselineCount; i++) {
        if (ctSameCanFrameId(_baseline[i].canId, _baseline[i].isExtended,
                             msg.id, msg.isExtended)) {
            baseEntry = &_baseline[i];
            break;
        }
    }

    bool isNew     = (baseEntry == nullptr);
    bool isChanged = false;

    if (!isNew) {
        // Compare data bytes against the last value seen in the baseline
        if (baseEntry->length != msg.length) {
            isChanged = true;
        } else {
            for (int i = 0; i < msg.length; i++) {
                if (baseEntry->lastData[i] != msg.data[i]) {
                    isChanged = true;
                    break;
                }
            }
        }
    }

    if (!isNew && !isChanged) {
        // This message is identical to what was already in the
        // baseline - likely unrelated to the pressed button, ignored.
        return;
    }

    // Find or add this message in the candidate list
    LearnCandidate* candidate = nullptr;
    for (int i = 0; i < _candidateCount; i++) {
        if (ctSameCanFrameId(_candidates[i].canId, _candidates[i].isExtended,
                             msg.id, msg.isExtended)) {
            candidate = &_candidates[i];
            break;
        }
    }

    if (!candidate) {
        if (_candidateCount >= CANDIDATE_MAX) {
            // Table full. Periodic frames with rolling counters change on every
            // cycle and used to fill all slots within milliseconds, so the real
            // button frame arriving later was silently dropped. Instead, replace
            // the least likely candidate if this one ranks better.
            LearnCandidate probe;
            probe.isNewMessage = isNew;
            probe.seenCountInBaseline = baseEntry ? baseEntry->seenCount : 0;
            probe.seenCountInAction = 1;
            int worstIdx = 0;
            for (int i = 1; i < _candidateCount; i++) {
                if (candidatePriority(_candidates[i]) > candidatePriority(_candidates[worstIdx])) worstIdx = i;
            }
            if (candidatePriority(probe) >= candidatePriority(_candidates[worstIdx])) return;
            candidate = &_candidates[worstIdx];
            *candidate = LearnCandidate();
            candidate->canId                  = msg.id;
            candidate->isExtended               = msg.isExtended;
            candidate->isNewMessage                = isNew;
            candidate->seenCountInBaseline             = probe.seenCountInBaseline;
            candidate->seenCountInAction                   = 0;
        } else {
        candidate = &_candidates[_candidateCount];
        _candidateCount++;
        candidate->canId                = msg.id;
        candidate->isExtended               = msg.isExtended;
        candidate->isNewMessage                = isNew;
        candidate->seenCountInBaseline             = baseEntry ? baseEntry->seenCount : 0;
        candidate->seenCountInAction                   = 0;
        }
    }

    // Always keep the latest data value (closest to the moment the button was pressed)
    candidate->length = msg.length;
    memcpy(candidate->data, msg.data, msg.length);
    candidate->seenCountInAction++;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ update() - must be called from the main loop
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void LearnEngine::update() {
    LearnStateGuard lock(_stateMutex);
    if (!lock.locked()) return;
    if (_state == LEARN_BASELINE_CAPTURE) {
        CanMessage msg;
        // Drain everything currently available this tick (non-blocking)
        CanRxFrame rxFrame;
        while (_can.receiveRx(_bus, CAN_RX_LEARN, rxFrame)) {
            msg = rxFrame.message;
            if (msg.isRemote) continue;  // RTR frames carry no data and cannot be learned as commands
            BaselineEntry* entry = _findOrAddBaseline(msg.id, msg.isExtended);
            if (entry) {
                entry->length = msg.length;
                memcpy(entry->lastData, msg.data, msg.length);
                entry->seenCount++;
            }
        }

        if (millis() - _phaseStartTime >= _phaseDurationMs) {
            _state = LEARN_WAITING_ACTION;
            if (_baselineOverflowed) {
                getErrorLog()->log(LOG_CAT_LEARN, LOG_WARN,
                    "Baseline table full (%d IDs) - unrelated frames may appear as candidates", BASELINE_MAX_IDS);
            }
            Serial.printf("[LEARN] Baseline capture complete - %d distinct message(s) seen. Now press the physical button\n",
                          _baselineCount);
        }
        return;
    }

    if (_state == LEARN_ACTION_CAPTURE) {
        CanMessage msg;
        CanRxFrame rxFrame;
        while (_can.receiveRx(_bus, CAN_RX_LEARN, rxFrame)) {
            msg = rxFrame.message;
            if (msg.isRemote) continue;
            _processActionMessage(msg);
        }

        if (millis() - _phaseStartTime >= _phaseDurationMs) {
            _rankCandidates();
            _state = LEARN_CANDIDATES_READY;
            Serial.printf("[LEARN] Action capture complete - %d candidate(s) found\n", _candidateCount);
        }
        return;
    }

    // Nothing to do in the other states (IDLE, WAITING_ACTION,
    // CANDIDATES_READY, ERROR) - waiting for the next user action.
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Confirm ready for action
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

void LearnEngine::confirmReadyForAction() {
    LearnStateGuard lock(_stateMutex);
    if (!lock.locked()) return;
    if (_state != LEARN_WAITING_ACTION) {
        Serial.println("[LEARN] confirmReadyForAction called in an invalid state");
        return;
    }

    // Defense in depth (see CarTouch_SPEC.md) - same re-check as
    // startBaselineCapture(). This is the other capture window the
    // SPEC calls out as needing the hardware guarantee.
    if (!_can.isListenOnlyActive(_bus)) {
        getErrorLog()->log(LOG_CAT_LEARN, LOG_ERROR, "confirmReadyForAction() refused - CAN driver not in hardware Listen-Only");
        _state = LEARN_ERROR;
        return;
    }

    // Flush the queue again so only messages *after* this moment
    // (truly concurrent with the button press) are captured.
    _can.flushRx(_bus, CAN_RX_LEARN);

    _candidateCount   = 0;
    _phaseStartTime      = millis();
    _phaseDurationMs        = LEARN_ACTION_CAPTURE_MS;
    _state                     = LEARN_ACTION_CAPTURE;

    Serial.println("[LEARN] Capturing action - press the vehicle's physical button now");
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Candidate ranking
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void LearnEngine::_rankCandidates() {
    // Simple insertion sort - element count is tiny (at most
    // CANDIDATE_MAX=10), so efficiency doesn't matter.
    // Priority:
    //   1. Brand-new messages (isNewMessage=true) rank before changed messages
    //   2. Among changed messages, a lower seenCountInBaseline (i.e. not
    //      a frequently-repeating periodic message) ranks higher - more
    //      likely to be a direct result of the button press rather than
    //      noise from a periodic signal like RPM
    for (int i = 1; i < _candidateCount; i++) {
        LearnCandidate key = _candidates[i];
        int            j   = i - 1;

        int keyPriority = candidatePriority(key);
        while (j >= 0 && candidatePriority(_candidates[j]) > keyPriority) {
            _candidates[j + 1] = _candidates[j];
            j--;
        }
        _candidates[j + 1] = key;
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Cancel
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void LearnEngine::cancel() {
    LearnStateGuard lock(_stateMutex);
    if (!lock.locked()) return;
    _cancelLocked(0);
}

bool LearnEngine::cancelIfSession(uint32_t expectedSessionId) {
    LearnStateGuard lock(_stateMutex);
    if (!lock.locked()) return false;
    return _cancelLocked(expectedSessionId);
}

bool LearnEngine::_cancelLocked(uint32_t expectedSessionId) {
    if (expectedSessionId != 0 && _sessionId != expectedSessionId) return false;

    if (_rxSubscribed) {
        _can.unsubscribeRx(_bus, CAN_RX_LEARN);
        _rxSubscribed = false;
    }
    _state             = LEARN_IDLE;
    _baselineCount        = 0;
    _candidateCount          = 0;
    _targetProfileId = 255;

    // Restore whatever CAN driver mode was active before beginLearning()
    // forced hardware Listen-Only (see CarTouch_SPEC.md). Called on every
    // exit path (save success, save failure, explicit cancel), not
    // just here.
    if (_forcedListenOnly) {
        if (!_previousListenOnlyMode) {
            if (_can.reconfigureMode(_bus, false)) {
                if (_bus == CAN_BUS_1) getConfig()->listenOnlyMode = false;
                else getConfig()->can1ListenOnly = false;
                Serial.println("[LEARN] Restored CAN driver to Normal mode after learning session");
            } else {
                // Fail-safe: if the switch back fails, deliberately stay
                // in Listen-Only rather than risk an unknown driver
                // state. The selected interface remains in Listen-Only.
                getErrorLog()->log(LOG_CAT_LEARN, LOG_WARN, "Failed to restore Normal mode after learning - staying in Listen-Only (fail-safe)");
            }
        }
        configSetLearnListenOverride(_bus == CAN_BUS_1 ? 0 : 1, false, false);
        _forcedListenOnly = false;
    }

    Serial.println("[LEARN] Learning process cancelled");
    return true;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Accessors
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

LearnModeState LearnEngine::getState() {
    LearnStateGuard lock(_stateMutex);
    return lock.locked() ? _state : LEARN_ERROR;
}

uint8_t LearnEngine::getCandidateCount() {
    LearnStateGuard lock(_stateMutex);
    return lock.locked() ? _candidateCount : 0;
}

bool LearnEngine::getCandidate(uint8_t index, LearnCandidate& outCandidate) {
    LearnStateGuard lock(_stateMutex);
    if (!lock.locked()) return false;
    if (index >= _candidateCount) return false;
    outCandidate = _candidates[index];
    return true;
}

uint8_t LearnEngine::getTargetProfileId() {
    LearnStateGuard lock(_stateMutex);
    return lock.locked() ? _targetProfileId : 255;
}

uint32_t LearnEngine::getSessionId() {
    LearnStateGuard lock(_stateMutex);
    return lock.locked() ? _sessionId : 0;
}

bool LearnEngine::isActiveCaptureState() {
    LearnStateGuard lock(_stateMutex);
    return lock.locked() &&
        (_state == LEARN_BASELINE_CAPTURE || _state == LEARN_ACTION_CAPTURE);
}

uint8_t LearnEngine::_progressPercentLocked() {
    if (_state != LEARN_BASELINE_CAPTURE && _state != LEARN_ACTION_CAPTURE) {
        return 0;
    }

    uint32_t elapsed = millis() - _phaseStartTime;
    if (elapsed >= _phaseDurationMs) return 100;

    return (uint8_t)((elapsed * 100) / _phaseDurationMs);
}

uint8_t LearnEngine::getProgressPercent() {
    LearnStateGuard lock(_stateMutex);
    return lock.locked() ? _progressPercentLocked() : 0;
}

bool LearnEngine::getSnapshot(LearnEngineSnapshot& snapshot) {
    LearnStateGuard lock(_stateMutex);
    if (!lock.locked()) {
        snapshot.state = LEARN_ERROR;
        snapshot.progressPercent = 0;
        snapshot.label[0] = '\0';
        snapshot.displayName[0] = '\0';
        snapshot.targetProfileId = 255;
        snapshot.sessionId = 0;
        snapshot.candidateCount = 0;
        return false;
    }

    snapshot.state = _state;
    snapshot.progressPercent = _progressPercentLocked();
    memcpy(snapshot.label, _currentLabel, sizeof(snapshot.label));
    memcpy(snapshot.displayName, _currentDisplayName, sizeof(snapshot.displayName));
    snapshot.targetProfileId = _targetProfileId;
    snapshot.sessionId = _sessionId;
    snapshot.candidateCount = _candidateCount;
    for (uint8_t i = 0; i < _candidateCount; ++i) {
        snapshot.candidates[i] = _candidates[i];
    }
    return true;
}
