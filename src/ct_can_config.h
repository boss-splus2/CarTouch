#ifndef CT_CAN_CONFIG_H
#define CT_CAN_CONFIG_H

#include <stdint.h>

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Pin and bitrate checks
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

static inline bool ctCanPinsConflictFree(uint8_t can0Tx, uint8_t can0Rx,
                                         uint8_t can1Cs, uint8_t can1Int) {
    return can0Tx != can0Rx && can1Cs != can1Int &&
           can0Tx != can1Cs && can0Tx != can1Int &&
           can0Rx != can1Cs && can0Rx != can1Int;
}

static inline bool ctMcp2515BitrateValid(uint32_t bitrate) {
    switch (bitrate) {
        case 100000:
        case 125000:
        case 250000:
        case 500000:
        case 1000000:
            return true;
        default:
            return false;
    }
}

// Bus-route selector: 0 = CAN1 (TWAI), 1 = CAN2 (MCP2515). Anything else is
// treated as the safe default (CAN1) instead of invalidating the config.
static inline uint8_t ctSanitizeBusIndex(uint8_t value) {
    return value <= 1u ? value : 0u;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Bit timing
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

static inline bool ctPartitionFitsFlash(uint32_t flashBytes, uint32_t partitionEndBytes) {
    return flashBytes != 0u && partitionEndBytes != 0u && flashBytes >= partitionEndBytes;
}


// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Link state
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// ---- Link state: "connected" only with real traffic -----------------------
// A started driver proves nothing about the wiring. A channel counts as
// connected (CT_LINK_TRAFFIC) only if a frame was really received within
// `timeoutMs`; started but silent is CT_LINK_NO_TRAFFIC (could be a parked
// car or a missing connection - the firmware cannot tell which).
enum CtCanLinkState : uint8_t {
    CT_LINK_DOWN = 0,    // driver not running
    CT_LINK_BUS_OFF,     // controller is in Bus-Off
    CT_LINK_NO_TRAFFIC,  // running, but no frame seen recently
    CT_LINK_TRAFFIC      // running and frames received recently
};

// lastRxMs == 0 means "never received". Wrap-safe (unsigned subtraction).
static inline CtCanLinkState ctCanLinkState(bool driverActive, bool busOff,
                                            uint32_t lastRxMs, uint32_t nowMs,
                                            uint32_t timeoutMs) {
    if (busOff) return CT_LINK_BUS_OFF;
    if (!driverActive) return CT_LINK_DOWN;
    if (lastRxMs == 0) return CT_LINK_NO_TRAFFIC;
    return (uint32_t)(nowMs - lastRxMs) <= timeoutMs ? CT_LINK_TRAFFIC
                                                      : CT_LINK_NO_TRAFFIC;
}

// Bus-Off recovery scheduling: first attempt immediately, then no more than
// one attempt per `retryMs`. Wrap-safe.
static inline bool ctRecoveryDue(bool alreadyTried, uint32_t lastAttemptMs,
                                 uint32_t nowMs, uint32_t retryMs) {
    return !alreadyTried || (uint32_t)(nowMs - lastAttemptMs) >= retryMs;
}

#endif