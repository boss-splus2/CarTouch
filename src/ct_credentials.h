#pragma once
// Single source of truth for the device login (user name, web password,
// Wi-Fi access-point key). No hardware dependencies: native-testable.
//
// Build modes (-DCT_PRODUCT_MODE=0|1 in platformio.ini):
//   0 = personal (DEFAULT). Fixed login, identical on every device:
//       CarTouch / 12345678. The Wi-Fi AP key follows the web password
//       (exactly the behaviour before this header existed). No random
//       password is ever generated.
//   1 = commercial (disabled for now). A unique login is generated once per
//       device, the Wi-Fi AP key is a SEPARATE stored value, and the owner
//       must change the web password before BLE commands / BLE OTA work.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "ct_password.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Build-time settings
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

#ifndef CT_PRODUCT_MODE
#define CT_PRODUCT_MODE 0
#endif

// Default login. Override with -D in platformio.ini, no need to edit this file:
//   '-DWEB_DEFAULT_USER="Owner"'   '-DWEB_DEFAULT_PASS="abcd1234"'
#ifndef WEB_DEFAULT_USER
#define WEB_DEFAULT_USER "CarTouch"
#endif
#ifndef WEB_DEFAULT_PASS
#define WEB_DEFAULT_PASS "12345678"
#endif

// Factory Wi-Fi AP key. Only used as the AP key when it is separate from the
// web password (CT_AP_KEY_SEPARATE=1) or as the fallback for a damaged value.
// In mode 0 it equals the web default, so both are the same fixed password.
#ifndef CT_AP_DEFAULT_PASS
#define CT_AP_DEFAULT_PASS WEB_DEFAULT_PASS
#endif

// 1 = the AP key is its own stored value (not the web password).
#ifndef CT_AP_KEY_SEPARATE
#define CT_AP_KEY_SEPARATE CT_PRODUCT_MODE
#endif

// 1 = while the factory password is unchanged, BLE commands and BLE OTA are
//     refused, TFT/Web show a "change the password" warning and the default
//     cannot be chosen again as the new password.
#ifndef CT_REQUIRE_PASSWORD_CHANGE
#define CT_REQUIRE_PASSWORD_CHANGE CT_PRODUCT_MODE
#endif

// Compile-time guard: user/password buffers are char[16] and WPA2 needs at
// least 8 characters.
static_assert(sizeof(WEB_DEFAULT_PASS) >= 9 && sizeof(WEB_DEFAULT_PASS) <= 16,
              "WEB_DEFAULT_PASS must be 8 to 15 characters");
static_assert(sizeof(CT_AP_DEFAULT_PASS) >= 9 && sizeof(CT_AP_DEFAULT_PASS) <= 16,
              "CT_AP_DEFAULT_PASS must be 8 to 15 characters");
static_assert(sizeof(WEB_DEFAULT_USER) >= 2 && sizeof(WEB_DEFAULT_USER) <= 16,
              "WEB_DEFAULT_USER must be 1 to 15 characters");
static_assert(CT_PRODUCT_MODE == 0 || CT_PRODUCT_MODE == 1,
              "CT_PRODUCT_MODE must be 0 or 1");

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Credential set
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// Bounded copy that always terminates (no dependency on strlcpy).
static inline void ctCopyStr(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) return;
    size_t i = 0;
    if (src) for (; i + 1 < dstSize && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

struct CtCredentials {
    char user[16];
    char webPass[16];
    char apKey[16];
    bool mustChange;  // owner has to pick their own web password
};

// Fills the login of a device that has no stored settings (first boot, factory
// reset, damaged data). Called from ONE place (config.cpp), never anywhere
// else, and only then.
//   productMode 0: the fixed defaults; rnd is never called.
//   productMode 1: unique 12-character web password and AP key from rnd;
//                  mustChange = true. Returns false (and fills nothing
//                  random) when rnd is null.
static inline bool ctProvisionInitialCredentials(CtCredentials* out, int productMode,
                                                 uint32_t (*rnd)(void)) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    ctCopyStr(out->user, sizeof(out->user), WEB_DEFAULT_USER);
    if (productMode == 0) {
        ctCopyStr(out->webPass, sizeof(out->webPass), WEB_DEFAULT_PASS);
        ctCopyStr(out->apKey, sizeof(out->apKey), CT_AP_DEFAULT_PASS);
        out->mustChange = false;
        return true;
    }
    if (!rnd) {
        // Never fall back to the public default in commercial mode.
        return false;
    }
    ctGeneratePassword(out->webPass, sizeof(out->webPass), rnd);
    ctGeneratePassword(out->apKey, sizeof(out->apKey), rnd);
    out->mustChange = true;
    return true;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Wi-Fi AP key and password policy
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// Key handed to WiFi.softAP(). Always 8..15 characters, never empty (an empty
// key would start an OPEN network).
//   separate = false: follows the web password (personal mode, old behaviour).
//   separate = true : the stored AP key.
// A too-short source falls back to the factory AP key.
static inline const char* ctEffectiveApKey(bool separate, const char* webPass,
                                           const char* storedApKey) {
    const char* p = separate ? storedApKey : webPass;
    return (p && strlen(p) >= 8) ? p : CT_AP_DEFAULT_PASS;
}

// True while the owner still has to change the password.
//   requireChange: build setting CT_REQUIRE_PASSWORD_CHANGE
//   mustChangeFlag: stored flag (set by provisioning in commercial mode)
// With requireChange = false the answer is always false (default login is
// allowed everywhere).
static inline bool ctPasswordChangePending(bool requireChange, bool mustChangeFlag,
                                           const char* webPass) {
    if (!requireChange) return false;
    if (mustChangeFlag) return true;
    return webPass && strcmp(webPass, WEB_DEFAULT_PASS) == 0;
}
