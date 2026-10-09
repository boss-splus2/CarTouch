#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "ct_sha256.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ OTA signature policy
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// ECDSA P-256 signatures are DER-encoded and represented as hexadecimal text.
// The SHA-256 digest is the digest of the exact uploaded artifact bytes.
// A DER signature is at most 72 bytes (144 hex characters) and is shorter when
// r or s has leading zero bytes, so only a loose lower bound is enforced here;
// the real structure check is done by the crypto library.
#define CT_OTA_SIG_HEX_MIN 16
#define CT_OTA_SIG_HEX_MAX 144
typedef bool (*CtOtaSignatureVerifier)(const char* sha256Hex,
                                       const char* signatureHex);

static inline bool ctOtaSignatureHexValid(const char* signatureHex) {
    if (!signatureHex) return false;
    size_t n = 0;
    while (signatureHex[n] != '\0' && n <= CT_OTA_SIG_HEX_MAX) ++n;
    if (n < CT_OTA_SIG_HEX_MIN || n > CT_OTA_SIG_HEX_MAX || (n & 1u) != 0) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = signatureHex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

// Personal mode preserves legacy SHA-256/header behaviour. Commercial mode
// fails closed unless a valid signature and a real verifier are supplied.
static inline bool ctOtaVerifyAuthenticity(bool productMode, const char* sha256Hex,
                                            const char* signatureHex,
                                            CtOtaSignatureVerifier verifier) {
    if (!productMode) return true;
    if (!ctSha256HexValid(sha256Hex) || !ctOtaSignatureHexValid(signatureHex) ||
        !verifier) return false;
    return verifier(sha256Hex, signatureHex);
}

// Verifies ECDSA P-256/SHA-256 against the public key compiled into the
// firmware. Replace the development public key before enabling product mode.
bool ctOtaVerifySignature(const char* sha256Hex, const char* signatureHex);
