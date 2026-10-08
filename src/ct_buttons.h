#pragma once
// Pure 5-way key logic (no hardware): ADC ladder classification, debounce,
// short/long press. Native-testable.
#include <stdint.h>
#include <stddef.h>
#include "ct_storage_policy.h"   // ctSdCsPinAllowed (generic reserved-pin check)

enum CtKey : uint8_t { CT_KEY_NONE = 0, CT_KEY_UP, CT_KEY_DOWN, CT_KEY_LEFT, CT_KEY_RIGHT, CT_KEY_OK,
                       CT_KEY_INVALID = 0xFE };  // INVALID: ADC value matches no key and is not idle
enum CtKeyEventType : uint8_t { CT_EV_NONE = 0, CT_EV_PRESS, CT_EV_SHORT, CT_EV_LONG };

#define CT_ADC_MAX 4095
#define CT_ADC_IDLE_MIN 3900   // at/above this the ladder is idle (nothing pressed)
#define CT_KEY_DEBOUNCE_MS 30
#define CT_KEY_LONG_MS 800

// ladder[0..4] = raw ADC value of UP,DOWN,LEFT,RIGHT,OK. A value matches when
// within +/- tol. Values above 4095 are never valid.
static inline CtKey ctAdcClassify(uint16_t raw, const uint16_t ladder[5], uint16_t tol) {
    if (raw > CT_ADC_MAX) return CT_KEY_INVALID;
    if (raw >= CT_ADC_IDLE_MIN) return CT_KEY_NONE;
    for (uint8_t i = 0; i < 5; i++) {
        const int d = (int)raw - (int)ladder[i];
        if (d >= -(int)tol && d <= (int)tol) return (CtKey)(i + 1);
    }
    return CT_KEY_INVALID;
}

// Ladder sanity: five distinct values, each below the idle threshold, and
// neighbours (when sorted) further apart than 2*tol so windows can't overlap.
static inline bool ctLadderValid(const uint16_t ladder[5], uint16_t tol) {
    for (uint8_t i = 0; i < 5; i++) {
        if (ladder[i] >= CT_ADC_IDLE_MIN) return false;
        for (uint8_t j = (uint8_t)(i + 1); j < 5; j++) {
            const int d = (int)ladder[i] - (int)ladder[j];
            if ((d < 0 ? -d : d) <= 2 * (int)tol) return false;
        }
    }
    return true;
}

// ADC1 channels only (GPIO1-10 on ESP32-S3); ADC2 conflicts with Wi-Fi.
static inline bool ctAdcPinAllowed(int pin, const int* inUse, size_t n) {
    return pin >= 1 && pin <= 10 && ctSdCsPinAllowed(pin, inUse, n);
}

struct CtKeyState {
    CtKey stable = CT_KEY_NONE;     // debounced key currently held
    CtKey candidate = CT_KEY_NONE;
    uint32_t candidateSince = 0;
    uint32_t pressedSince = 0;
    bool longSent = false;
};

struct CtKeyEvent { CtKey key; CtKeyEventType type; };

// Feed the raw key every poll. INVALID readings are treated as "no change".
// Returns at most one event per call: PRESS on a debounced press, LONG once
// when held >= CT_KEY_LONG_MS, SHORT on release if no LONG was sent.
static inline CtKeyEvent ctKeyStep(CtKeyState& s, CtKey raw, uint32_t now) {
    CtKeyEvent ev = { CT_KEY_NONE, CT_EV_NONE };
    if (raw == CT_KEY_INVALID) return ev;
    if (raw != s.candidate) { s.candidate = raw; s.candidateSince = now; }
    if (raw != s.stable && (uint32_t)(now - s.candidateSince) >= CT_KEY_DEBOUNCE_MS) {
        const CtKey prev = s.stable;
        s.stable = raw;
        if (prev != CT_KEY_NONE && !s.longSent) { ev.key = prev; ev.type = CT_EV_SHORT; }
        s.longSent = false;
        if (raw != CT_KEY_NONE) {
            s.pressedSince = now;
            if (ev.type == CT_EV_NONE) { ev.key = raw; ev.type = CT_EV_PRESS; }
        }
        return ev;
    }
    if (s.stable != CT_KEY_NONE && !s.longSent &&
        (uint32_t)(now - s.pressedSince) >= CT_KEY_LONG_MS) {
        s.longSent = true;
        ev.key = s.stable; ev.type = CT_EV_LONG;
    }
    return ev;
}
