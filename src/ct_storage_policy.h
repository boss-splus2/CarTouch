#pragma once
// Pure storage-location logic (no hardware): native-testable.
#include <stdint.h>
#include <stddef.h>

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Storage types
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

enum CtStorageChoice : uint8_t { CT_STORE_AUTO = 0, CT_STORE_INTERNAL = 1, CT_STORE_SD = 2 };
enum CtStorageLoc    : uint8_t { CT_LOC_NONE = 0, CT_LOC_INTERNAL = 1, CT_LOC_SD = 2 };

struct CtStorageDecision {
    CtStorageLoc loc;
    bool fellBack;  // true when the user's explicit choice was unavailable
};

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Storage resolution
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// Keep this much internal space free before AUTO moves data to the SD card.
#define CT_INTERNAL_RESERVE_BYTES 32768u

static inline bool ctStorageChoiceValid(uint8_t v) { return v <= CT_STORE_SD; }

// AUTO: internal while it has room, then SD if present, else internal (never drop data).
// Explicit choice: that location when healthy, otherwise the other healthy one
// (fellBack=true) so the caller can tell the user. NONE only if both are unusable.
static inline CtStorageDecision ctResolveStorage(uint8_t choice,
        bool internalOk, uint32_t internalFree,
        bool sdOk, uint32_t sdFree, uint32_t needBytes) {
    CtStorageDecision d = { CT_LOC_NONE, false };
    const bool intFits = internalOk && internalFree >= needBytes + CT_INTERNAL_RESERVE_BYTES;
    const bool sdFits  = sdOk && sdFree >= needBytes;
    if (choice == CT_STORE_INTERNAL) {
        if (intFits) { d.loc = CT_LOC_INTERNAL; return d; }
        if (sdFits)  { d.loc = CT_LOC_SD; d.fellBack = true; }
        return d;
    }
    if (choice == CT_STORE_SD) {
        if (sdFits)  { d.loc = CT_LOC_SD; return d; }
        if (intFits) { d.loc = CT_LOC_INTERNAL; d.fellBack = true; }
        return d;
    }
    // AUTO (also used for any invalid stored value)
    if (intFits) { d.loc = CT_LOC_INTERNAL; return d; }
    if (sdFits)  { d.loc = CT_LOC_SD; return d; }
    return d;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ SD pin policy
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// GPIO acceptable for optional peripherals on ESP32-S3: conservative reserve
// list. Exclude strapping, USB/UART, nonexistent GPIO22-25, and flash/PSRAM
// pins; `inUse` lists pins already assigned by the selected board profile.
static inline bool ctSdCsPinAllowed(int pin, const int* inUse, size_t inUseCount) {
    if (pin < 0 || pin > 48) return false;
    if (pin == 0 || pin == 3 || pin == 45 || pin == 46) return false;
    if (pin == 19 || pin == 20) return false;
    if (pin >= 22 && pin <= 25) return false;
    if (pin >= 26 && pin <= 37) return false;
    if (pin == 43 || pin == 44) return false;
    for (size_t i = 0; i < inUseCount; i++) if (inUse[i] == pin) return false;
    return true;
}
