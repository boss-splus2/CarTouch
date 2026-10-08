#ifndef CT_OBD_VALIDITY_H
#define CT_OBD_VALIDITY_H

#include <stdint.h>

// Per-value validity of the polled OBD-II data. A value counts as valid only
// if the ECU really answered for it, and only for a limited time afterwards.
// Without this, a PID that never answers (unsupported, ignition off, bus
// problem) kept showing its initial 0 or its last number as if it were live.

#define CT_VD_RPM       0x01
#define CT_VD_SPEED     0x02
#define CT_VD_COOLANT   0x04
#define CT_VD_THROTTLE  0x08
#define CT_VD_FUEL      0x10
#define CT_VD_RUNTIME   0x20
#define CT_VD_BATTERY   0x40

// A full poll round is about 1.5 s in the worst case (7 PIDs, 200 ms timeout
// each, plus spacing), so 5 s means "missed several rounds in a row".
#define CT_OBD_STALE_MS 5000u

// Wrap-safe: unsigned subtraction stays correct when millis() rolls over.
static inline bool ctObdValueFresh(bool everAnswered, uint32_t lastAnswerMs,
                                   uint32_t nowMs, uint32_t staleMs) {
    if (!everAnswered) return false;
    return (uint32_t)(nowMs - lastAnswerMs) <= staleMs;
}

static inline bool ctVdValid(uint8_t mask, uint8_t bit) {
    return (mask & bit) != 0;
}

#endif
