#ifndef CT_DBC_STORE_H
#define CT_DBC_STORE_H
// Pure rules for user-supplied DBC files (no hardware, no filesystem): native-testable.
// The firmware calls these before it touches storage, so a bad upload is refused
// with a clear reason instead of being cut off or loaded silently.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Limits
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// Built-in profiles keep the path in char dbcFileName[32]: "/dbc/" (5) + name (<=26) + NUL.
#define CT_DBC_DIR       "/dbc/"
#define CT_DBC_NAME_MAX  26u
#define CT_DBC_MAX_BYTES 409600u     // largest accepted upload (400 KB)
// Must equal MAX_DBC_MESSAGES in vehicle_db.h (the loader silently skips the rest).
#define CT_DBC_MAX_MESSAGES  400u
#define CT_DBC_RESERVE_BYTES 32768u  // free space that must remain after an upload

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Validation results and rules
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

enum CtDbcResult : uint8_t {
    CT_DBC_OK = 0,
    CT_DBC_BAD_NAME,
    CT_DBC_EMPTY,
    CT_DBC_TOO_BIG,
    CT_DBC_NO_SPACE,
    CT_DBC_BAD_CONTENT,  // control/binary bytes: not a text DBC
    CT_DBC_NO_MESSAGES,
    CT_DBC_TOO_MANY_MESSAGES
};

static inline const char* ctDbcResultText(CtDbcResult r) {
    switch (r) {
        case CT_DBC_OK:                return "OK";
        case CT_DBC_BAD_NAME:          return "Invalid file name (use letters, digits, _ - . and end with .dbc, max 26 characters)";
        case CT_DBC_EMPTY:             return "File is empty";
        case CT_DBC_TOO_BIG:           return "File is larger than the 400 KB limit";
        case CT_DBC_NO_SPACE:          return "Not enough free storage space";
        case CT_DBC_BAD_CONTENT:       return "File is not a text DBC file";
        case CT_DBC_NO_MESSAGES:       return "No messages (BO_ lines) found in the file";
        case CT_DBC_TOO_MANY_MESSAGES: return "File has more messages than the device can hold (400)";
    }
    return "Unknown error";
}

static inline bool ctDbcNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.';
}

// Letters, digits, '_', '-', '.', must end in ".dbc" (any case), not start with '.',
// no "..", at most CT_DBC_NAME_MAX characters. No '/' or '\\' ever passes (no path tricks).
static inline bool ctDbcNameValid(const char* name) {
    if (!name) return false;
    size_t n = strlen(name);
    if (n < 5 || n > CT_DBC_NAME_MAX) return false;
    if (name[0] == '.') return false;
    for (size_t i = 0; i < n; ++i) {
        if (!ctDbcNameChar(name[i])) return false;
        if (name[i] == '.' && i + 1 < n && name[i + 1] == '.') return false;
    }
    const char* e = name + n - 4;
    return e[0] == '.' && (e[1] | 0x20) == 'd' && (e[2] | 0x20) == 'b' && (e[3] | 0x20) == 'c';
}

// Builds "/dbc/<name>" into out. False (out untouched or empty) for an invalid name or small buffer.
static inline bool ctDbcBuildPath(const char* name, char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = '\0';
    if (!ctDbcNameValid(name)) return false;
    size_t d = strlen(CT_DBC_DIR), n = strlen(name);
    if (d + n + 1 > outSize) return false;
    memcpy(out, CT_DBC_DIR, d);
    memcpy(out + d, name, n + 1);
    return true;
}

// Size check before any byte is written. replacingBytes = size of an existing file with the
// same name that this upload replaces (its space becomes free again).
static inline CtDbcResult ctDbcSizeCheck(uint32_t sizeBytes, uint64_t freeBytes, uint32_t replacingBytes) {
    if (sizeBytes == 0) return CT_DBC_EMPTY;
    if (sizeBytes > CT_DBC_MAX_BYTES) return CT_DBC_TOO_BIG;
    const uint64_t avail = freeBytes + replacingBytes;
    if (avail < (uint64_t)sizeBytes + CT_DBC_RESERVE_BYTES) return CT_DBC_NO_SPACE;
    return CT_DBC_OK;
}

// Streaming content scan: feed it every line (without '\n'). Counts "BO_ " records (after
// optional indentation) and flags control bytes (other than TAB and CR) that a text DBC never has.
struct CtDbcScan {
    uint32_t messages;
    bool     badBytes;
};
static inline void ctDbcScanInit(CtDbcScan& s) { s.messages = 0; s.badBytes = false; }

static inline void ctDbcScanLine(CtDbcScan& s, const char* line) {
    if (!line) return;
    const char* p = line;
    for (; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 && c != '\t' && c != '\r') s.badBytes = true;
    }
    p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (strncmp(p, "BO_ ", 4) == 0) ++s.messages;
}

static inline CtDbcResult ctDbcScanVerdict(const CtDbcScan& s) {
    if (s.badBytes) return CT_DBC_BAD_CONTENT;
    if (s.messages == 0) return CT_DBC_NO_MESSAGES;
    if (s.messages > CT_DBC_MAX_MESSAGES) return CT_DBC_TOO_MANY_MESSAGES;
    return CT_DBC_OK;
}

// Deleting: a file shipped with the firmware or used by the active vehicle is never removed
// silently. A user file that is in use needs explicit confirmation.
enum CtDbcDeleteDecision : uint8_t { CT_DBC_DEL_ALLOW = 0, CT_DBC_DEL_REFUSE_BUILTIN, CT_DBC_DEL_NEEDS_CONFIRM };
static inline CtDbcDeleteDecision ctDbcDeleteDecision(bool isBuiltin, bool isActive, bool confirmed) {
    if (isBuiltin) return CT_DBC_DEL_REFUSE_BUILTIN;
    if (isActive && !confirmed) return CT_DBC_DEL_NEEDS_CONFIRM;
    return CT_DBC_DEL_ALLOW;
}

#endif
