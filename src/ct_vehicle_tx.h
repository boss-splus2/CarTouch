/**
 * ct_vehicle_tx.h - Pure vehicle-control TX decision chain
 *
 * The exact decision order used by VehicleControl before a frame reaches the
 * CAN service: Listen-Only / readiness / frame validity, then the base rate
 * limit, then the actual send. Kept free of hardware includes so the whole
 * chain (and the per-actuator duty-cycle limit) is unit-tested natively with
 * a mock CAN interface. VehicleControl calls these functions directly; there
 * is no second copy of the logic.
 */
#ifndef CT_VEHICLE_TX_H
#define CT_VEHICLE_TX_H

#include <stdint.h>
#include "can_interface.h"
#include "ct_tx_guard.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Limits
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

#define CT_COMMAND_MIN_INTERVAL_MS 150
#define CT_DUTY_WINDOW_MS          10000
#define CT_DUTY_MAX_ACTIVATIONS    6
#define CT_DUTY_COOLDOWN_MS        5000

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Base rate limit and send chain
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

enum CtVehicleTxResult : uint8_t {
    CT_VTX_SENT = 0,
    CT_VTX_LISTEN_ONLY,     // config or driver is in Listen-Only
    CT_VTX_NOT_READY,       // selected bus not started
    CT_VTX_INVALID_FRAME,   // DLC > 8 or ID wider than the frame format
    CT_VTX_RATE_LIMITED,    // two commands closer than the minimum interval
    CT_VTX_SEND_FAILED      // the bus driver refused the frame
};

// Wrap-safe: unsigned subtraction stays correct across a millis() rollover.
static inline bool ctCommandRateOk(uint32_t now, uint32_t lastCommandTime,
                                   uint32_t minIntervalMs) {
    return (uint32_t)(now - lastCommandTime) >= minIntervalMs;
}

/**
 * Admission + send for one vehicle-control frame on an explicit bus.
 * `Service` needs isActive(bus), isListenOnlyActive(bus) and
 * sendMessage(bus, msg). Nothing reaches sendMessage unless every check
 * passed. `lastCommandTime` is updated only once the rate check has passed
 * (also when the driver then fails), exactly as before the refactor.
 */
template <class Service, class Bus>
static inline CtVehicleTxResult ctVehicleTxSend(Service& can, Bus bus,
                                                bool configListenOnly,
                                                const CanMessage& msg,
                                                uint32_t now,
                                                uint32_t& lastCommandTime,
                                                uint32_t minIntervalMs) {
    CtTxGuardResult admission = ctVehicleTxGuard(
        configListenOnly, can.isActive(bus), can.isListenOnlyActive(bus), msg.length);
    if (admission == CT_TX_OK && !ctTxIdValid(msg.id, msg.isExtended)) {
        admission = CT_TX_ERR_LENGTH;  // "invalid frame" (ID or length)
    }
    if (admission == CT_TX_ERR_LISTEN_ONLY)     return CT_VTX_LISTEN_ONLY;
    if (admission == CT_TX_ERR_NOT_INITIALIZED) return CT_VTX_NOT_READY;
    if (admission != CT_TX_OK)                  return CT_VTX_INVALID_FRAME;

    if (!ctCommandRateOk(now, lastCommandTime, minIntervalMs)) return CT_VTX_RATE_LIMITED;
    lastCommandTime = now;

    return can.sendMessage(bus, msg) ? CT_VTX_SENT : CT_VTX_SEND_FAILED;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Mechanical duty-cycle limit
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

struct CtDutyState {
    uint32_t activationTimestamps[CT_DUTY_MAX_ACTIVATIONS];  // circular buffer
    uint8_t  count;                                          // entries recorded so far
    uint8_t  nextSlot;                                       // next write index
    uint32_t cooldownUntil;                                  // 0 = no active cooldown
};

enum CtDutyResult : uint8_t {
    CT_DUTY_OK = 0,
    CT_DUTY_COOLDOWN,   // still resting after the cap was hit
    CT_DUTY_LIMIT       // the cap was just reached; cooldown started now
};

static inline CtDutyResult ctDutyCheck(CtDutyState* state, uint32_t now) {
    if (state->cooldownUntil != 0) {
        // Wrap-safe: the cooldown is only a few seconds, so the signed
        // difference stays correct across a millis() rollover.
        if ((int32_t)(now - state->cooldownUntil) < 0) return CT_DUTY_COOLDOWN;
        state->cooldownUntil = 0;
        state->count         = 0;
        state->nextSlot      = 0;
    }

    uint8_t recentCount = 0;
    for (uint8_t i = 0; i < state->count; i++) {
        if ((uint32_t)(now - state->activationTimestamps[i]) <= CT_DUTY_WINDOW_MS) {
            recentCount++;
        }
    }

    if (recentCount >= CT_DUTY_MAX_ACTIVATIONS) {
        const uint32_t until = now + CT_DUTY_COOLDOWN_MS;
        // 0 is the "no cooldown" sentinel - avoid colliding with it.
        state->cooldownUntil = (until == 0) ? 1 : until;
        return CT_DUTY_LIMIT;
    }
    return CT_DUTY_OK;
}

static inline void ctDutyRecord(CtDutyState* state, uint32_t now) {
    state->activationTimestamps[state->nextSlot] = now;
    state->nextSlot = (uint8_t)((state->nextSlot + 1) % CT_DUTY_MAX_ACTIVATIONS);
    if (state->count < CT_DUTY_MAX_ACTIVATIONS) state->count++;
}

#endif    // CT_VEHICLE_TX_H
