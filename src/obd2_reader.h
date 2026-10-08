/**
 * obd2_reader.h - OBD-II data acquisition
 *
 * The main runtime path is the non-blocking poll state machine exposed by
 * update() and getLatestData(). Direct PID helpers remain available for
 * explicit callers but may block while waiting for an ECU response.
 */

#ifndef OBD2_READER_H
#define OBD2_READER_H

#include <Arduino.h>
#include "config.h"
#include "can_service.h"
#include "ct_obd_parser.h"

// State machine states for the non-blocking multi-PID poll cycle

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Polling types
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

enum ObdPollState : uint8_t {
    OBD_POLL_IDLE = 0,    // Between rounds, waiting for the next one to start
    OBD_POLL_SENDING,     // Sending the request for the current PID
    OBD_POLL_WAITING,     // Waiting for the current PID's response (no delay/loop)
    OBD_POLL_DONE         // A full round (all PIDs) has completed
};

enum ObdDiagnosticState : uint8_t {
    OBD_DIAG_IDLE = 0,
    OBD_DIAG_READ_WAITING,
    OBD_DIAG_READ_CONSECUTIVE,
    OBD_DIAG_CLEAR_WAITING,
    OBD_DIAG_COMPLETE,
    OBD_DIAG_FAILED
};

enum ObdDiagnosticOperation : uint8_t {
    OBD_DIAG_OP_NONE = 0,
    OBD_DIAG_OP_READ_DTCS,
    OBD_DIAG_OP_CLEAR_DTCS
};

struct ObdResponse {
    uint8_t  pid;          // Requested PID
    uint8_t  data[6];      // Response payload (up to 6 bytes)
    uint8_t  length;       // Actual payload length
    bool     success;      // true if the response is valid
    uint32_t timestamp;    // Reception time (millis)
};

class OBD2Reader {

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Public API
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

public:
    OBD2Reader(CANService& canService);

    void begin();
    bool setCanBus(CanBusId bus);
    CanBusId getCanBus() const { return _bus; }
    bool canTransmit();

    /**
     * [BLOCKING] Sends a request and waits for the response to a single
     * PID - blocks for up to ~200ms. Do not call from the main loop;
     * suitable for manual debugging or calls outside the main loop. Use
     * update()/getLatestData() for periodic background polling.
     */
    bool requestPID(uint8_t pid, ObdResponse& response);

    /** [BLOCKING] See requestPID note above. */
    uint16_t readEngineRPM();
    uint8_t  readVehicleSpeed();
    int8_t   readCoolantTemp();
    uint8_t  readThrottlePosition();
    uint8_t  readFuelLevel();
    uint16_t readEngineRuntime();
    float    readControlModuleVoltage();

    /**
     * [BLOCKING] Reads the base PIDs sequentially. Do not call from the
     * main loop; use update() and getLatestData() for periodic polling.
     */
    void readAllPIDs(VehicleData& data);

    /**
     * [NON-BLOCKING] Must be called every loop() iteration (like
     * tftUI.update()). Advances the periodic poll state machine by one
     * step, respecting the minimum request interval: sends a request or
     * checks the receive queue without blocking. Never calls delay() or
     * waits in a loop.
     */
    void update();

    /**
     * Returns the most recently completed poll round. If no round has
     * completed yet, returns default VehicleData values (zero/UNKNOWN).
     * @return true if at least one full round has completed
     */
    bool getLatestData(VehicleData& outData);

    /** Current poll state machine status (for UI/debug status display). */
    ObdPollState getPollState();

    /** Checks whether the ECU supports a given PID (via PID 0x00). */
    bool isPidSupported(uint8_t pid);

    /**
     * Reads Mode 03 DTCs using ISO-TP Single Frame or bounded
     * First/Consecutive Frame reassembly. Blocking; never call from loop().
     */
    uint8_t readDTCs(uint16_t dtcList[], uint8_t maxCount = MAX_DTC_COUNT);

    /** Clears stored DTCs. */
    bool clearDTCs();

    bool startDtcRead();
    bool startDtcClear();
    bool isDiagnosticBusy() const;
    ObdDiagnosticState getDiagnosticState() const { return _diagnosticState; }
    ObdDiagnosticOperation getDiagnosticOperation() const { return _diagnosticOperation; }
    uint8_t getDiagnosticError() const { return _diagnosticError; }
    uint8_t getDiagnosticResponseCode() const { return _diagnosticResponseCode; }
    uint8_t getDtcCount() const { return _dtcCount; }
    uint16_t getDtc(uint8_t index) const {
        return index < _dtcCount ? _dtcList[index] : 0;
    }

    uint8_t getLastError();
    bool hasReceivedData() const { return _hasCompletedRound; }

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Polling internals
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

private:
    CANService& _can;
    CanBusId _bus;
    bool _rxSubscribed;
    uint8_t     _lastError;
    uint32_t    _lastRequestTime;
    uint32_t    _requestInterval;    // Minimum spacing between requests (ms)

    bool _sendOBDRequest(uint8_t pid, uint8_t expectedDataLength);
    bool _parseOBDResponse(const uint8_t* rawData, uint8_t length,
                            uint8_t pid, ObdResponse& response);

    // -- Non-blocking poll state machine (update()) --------------------------
    static const uint8_t _POLL_PID_COUNT = 7;
    ObdPollState _pollState;
    uint8_t      _pollIndex;                     // Current PID index within _POLL_PID_COUNT
    uint32_t     _pollWaitStartMs;               // When the current PID's wait started
    VehicleData  _pendingData;                   // Data being assembled this round
    VehicleData  _latestData;                    // Most recently completed round
    bool         _hasCompletedRound;
    uint32_t     _lastAnswerMs[7];               // Last time the ECU answered each polled PID
    bool         _everAnswered[7];
    uint32_t     _pollIntervalMs;                // Spacing between rounds (not between PIDs)
    uint32_t     _lastRoundStartMs;

    ObdDiagnosticState _diagnosticState;
    ObdDiagnosticOperation _diagnosticOperation;
    uint32_t _diagnosticStartMs;
    uint8_t _diagnosticError;
    uint8_t _diagnosticResponseCode;
    uint8_t _dtcCount;
    uint16_t _dtcList[MAX_DTC_COUNT];
    uint8_t _dtcPayload[1 + (2 * MAX_DTC_COUNT)];
    CtIsoTpReassembly _dtcReassembly;

    void _pollStartNextPid();                                                         // Sends the current PID's request (non-blocking)
    void _pollCheckResponse();                                                        // Non-blocking receive-queue check
    void _applyPidToData(uint8_t pid, const ObdResponse& resp, VehicleData& data);
    bool _startDiagnosticRequest(uint8_t service, ObdDiagnosticOperation operation);
    void _updateDiagnostic();
    void _finishDtcRead(const uint8_t* payload, uint16_t length);
};

#endif    // OBD2_READER_H
