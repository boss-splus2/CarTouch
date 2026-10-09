#pragma once
// Per-device random web password (no hardware dependencies, native-testable).
// Alphabet avoids look-alike characters (0/O, 1/l/I). Length 12 fits webPass[16].
#include <stddef.h>
#include <stdint.h>

#define CT_GEN_PASS_LEN 12

static inline void ctGeneratePassword(char* out, size_t outSize,
                                      uint32_t (*rnd)(void)) {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
    const uint32_t n = (uint32_t)(sizeof(alphabet) - 1);
    if (!out || outSize == 0) return;
    size_t len = CT_GEN_PASS_LEN;
    if (len > outSize - 1) len = outSize - 1;
    // Rejection sampling: avoid modulo bias.
    const uint32_t limit = (0xFFFFFFFFu / n) * n;
    for (size_t i = 0; i < len; i++) {
        uint32_t r;
        do { r = rnd(); } while (r >= limit);
        out[i] = alphabet[r % n];
    }
    out[len] = '\0';
}
