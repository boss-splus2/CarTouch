#ifndef CT_LOGIN_LOCK_H
#define CT_LOGIN_LOCK_H

#include <stdint.h>

// Failed-login lockout, shared by every BLE path that checks the password
// (command login and OTA start). One shared counter means an attacker cannot
// get 5 guesses on each path per minute: all wrong guesses add up.

#define CT_LOGIN_MAX_FAILS  5u
#define CT_LOGIN_LOCK_MS    60000u

struct CtLoginLock {
    uint8_t  fails = 0;
    uint32_t lockUntil = 0;    // 0 = not locked
};

// true while locked out. An expired lock is cleared here.
static inline bool ctLoginLocked(CtLoginLock& l, uint32_t nowMs) {
    if (l.lockUntil == 0) return false;
    if ((int32_t)(nowMs - l.lockUntil) < 0) return true;
    l.lockUntil = 0;
    l.fails = 0;
    return false;
}

static inline void ctLoginFailed(CtLoginLock& l, uint32_t nowMs) {
    if (++l.fails >= CT_LOGIN_MAX_FAILS) {
        l.fails = 0;
        l.lockUntil = nowMs + CT_LOGIN_LOCK_MS;
        // cppcheck-suppress knownConditionTrueFalse -- millis() wrap can produce 0.
        if (l.lockUntil == 0) l.lockUntil = 1;    // 0 means "not locked"
    }
}

static inline void ctLoginSucceeded(CtLoginLock& l) {
    l.fails = 0;
    l.lockUntil = 0;
}

#endif
