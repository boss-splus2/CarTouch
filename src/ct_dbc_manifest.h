#ifndef CT_DBC_MANIFEST_H
#define CT_DBC_MANIFEST_H
// Pure helpers for the DBC manifests (no filesystem, no JSON library): native-testable.
// Both manifests (the built-in data/dbc/manifest.json written by scripts/audit_dbc.py and the
// user manifest written by the firmware) use ONE compact JSON object per line:
//   {"format":1,"files":[
//   {"name":"x.dbc","size":123,"sha256":"<64 hex>","messages":12,...},
//   ]}
// so a file can be searched line by line with a tiny buffer instead of loading it all.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "ct_dbc_store.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Manifest entry and format constants
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

struct CtDbcManifestEntry {
    char     name[CT_DBC_NAME_MAX + 1];
    uint32_t size;
    char     sha256[65];
    uint32_t messages;
    char     time[24];
    char     source[48];
    char     license[48];
};

#define CT_DBC_MANIFEST_HEADER "{\"format\":1,\"files\":["
#define CT_DBC_MANIFEST_FOOTER "]}"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ JSON field helpers
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

static inline bool ctDbcIsHex64(const char* s) {
    if (!s || strlen(s) != 64) return false;
    for (int i = 0; i < 64; ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

// Finds "key":"value" on the line and copies value (no escape sequences allowed).
static inline bool ctDbcJsonStr(const char* line, const char* key, char* out, size_t outSize) {
    if (!line || !key || !out || outSize == 0) return false;
    char pat[24];
    int pn = snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    if (pn <= 0 || (size_t)pn >= sizeof(pat)) return false;
    const char* p = strstr(line, pat);
    if (!p) return false;
    p += pn;
    size_t n = 0;
    while (p[n] && p[n] != '"') {
        if (p[n] == '\\') return false;
        if (n + 1 >= outSize) return false;
        out[n] = p[n];
        ++n;
    }
    if (p[n] != '"') return false;  // unterminated (truncated line)
    out[n] = '\0';
    return true;
}

// Finds "key":<digits> and parses it strictly (digits only, no overflow).
static inline bool ctDbcJsonUint(const char* line, const char* key, uint32_t& value) {
    if (!line || !key) return false;
    char pat[24];
    int pn = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (pn <= 0 || (size_t)pn >= sizeof(pat)) return false;
    const char* p = strstr(line, pat);
    if (!p) return false;
    p += pn;
    if (*p < '0' || *p > '9') return false;
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        if (v > 0xFFFFFFFFull) return false;
        ++p;
    }
    value = (uint32_t)v;
    return true;
}

static inline bool ctDbcJsonStrOptional(const char* line, const char* key,
                                        const char* fallback, char* out, size_t outSize) {
    char pattern[24];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) return false;
    if (strstr(line, pattern)) return ctDbcJsonStr(line, key, out, outSize);
    const size_t fallbackLength = strlen(fallback);
    if (fallbackLength >= outSize) return false;
    memcpy(out, fallback, fallbackLength + 1);
    return true;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Manifest line parse and format
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// One manifest line -> entry. False for header/footer lines and for anything invalid
// (bad name, bad hash, zero size, missing field, truncated line).
static inline bool ctDbcManifestParseLine(const char* line, CtDbcManifestEntry& e) {
    if (!line) return false;
    while (*line == ' ' || *line == '\t') ++line;
    if (*line != '{') return false;
    memset(&e, 0, sizeof(e));
    if (!ctDbcJsonStr(line, "name", e.name, sizeof(e.name)) || !ctDbcNameValid(e.name)) return false;
    if (!ctDbcJsonStr(line, "sha256", e.sha256, sizeof(e.sha256)) || !ctDbcIsHex64(e.sha256)) return false;
    if (!ctDbcJsonUint(line, "size", e.size) || e.size == 0) return false;
    if (!ctDbcJsonUint(line, "messages", e.messages)) return false;
    if (!ctDbcJsonStrOptional(line, "time", "unknown", e.time, sizeof(e.time)) ||
        !ctDbcJsonStrOptional(line, "source", "user", e.source, sizeof(e.source)) ||
        !ctDbcJsonStrOptional(line, "license", "unverified", e.license, sizeof(e.license))) return false;
    const char* end = line + strlen(line);
    while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == ',')) --end;
    return end > line && end[-1] == '}';  // line must be a complete object
}

// Entry -> one manifest line (no trailing newline or comma). Returns the length, 0 on failure.
static inline size_t ctDbcManifestFormatLine(const CtDbcManifestEntry& e, char* out, size_t outSize) {
    if (!out || outSize == 0) return 0;
    out[0] = '\0';
    if (!ctDbcNameValid(e.name) || !ctDbcIsHex64(e.sha256) || e.size == 0) return 0;
    const char* time = e.time[0] ? e.time : "unknown";
    const char* source = e.source[0] ? e.source : "user";
    const char* license = e.license[0] ? e.license : "unverified";
    const char* metadata[] = { time, source, license };
    for (size_t i = 0; i < sizeof(metadata) / sizeof(metadata[0]); ++i) {
        for (const char* p = metadata[i]; *p; ++p) {
            if (static_cast<unsigned char>(*p) < 0x20 || *p == '"' || *p == '\\') return 0;
        }
    }
    int n = snprintf(out, outSize,
                     "{\"name\":\"%s\",\"size\":%lu,\"sha256\":\"%s\",\"messages\":%lu,"
                     "\"time\":\"%s\",\"source\":\"%s\",\"license\":\"%s\"}",
                     e.name, (unsigned long)e.size, e.sha256, (unsigned long)e.messages,
                     time, source, license);
    if (n <= 0 || (size_t)n >= outSize) { out[0] = '\0'; return 0; }
    return (size_t)n;
}

#endif
