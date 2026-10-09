#include "ct_ota_authenticity.h"
#include "ct_sha256.h"
#include <stdint.h>
#include <string.h>

#ifndef CT_PRODUCT_MODE
#define CT_PRODUCT_MODE 0
#endif

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ ECDSA P-256 release verifier
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

#if defined(ARDUINO) && CT_PRODUCT_MODE
#include <mbedtls/pk.h>
#include <mbedtls/md.h>

namespace {
// DEVELOPMENT KEY ONLY. Replace with the product release public key before
// enabling CT_PRODUCT_MODE=1. Keep the corresponding private key offline.
static const char CT_OTA_PUBLIC_KEY_PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEzPtQYcPGuNE06lDHpd0U11YHlilt\n"
    "ZwAL+EwjIcAsD1Vmzws7aLcU1brn8lWh/yZ3t0xF3FQdl18iItucvZXzqQ==\n"
    "-----END PUBLIC KEY-----\n";

static int ctHexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool ctHexDecode(const char* hex, uint8_t* out, size_t outSize) {
    if (!hex || !out || !ctOtaSignatureHexValid(hex)) return false;
    const size_t n = strlen(hex) / 2;
    if (n > outSize) return false;
    for (size_t i = 0; i < n; ++i) {
        const int hi = ctHexNibble(hex[i * 2]);
        const int lo = ctHexNibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool ctDigestHexDecode(const char* hex, uint8_t out[32]) {
    if (!ctSha256HexValid(hex)) return false;
    for (size_t i = 0; i < 32; ++i) {
        const int hi = ctHexNibble(hex[i * 2]);
        const int lo = ctHexNibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
}  // namespace

bool ctOtaVerifySignature(const char* sha256Hex, const char* signatureHex) {
    uint8_t digest[32] = {};
    uint8_t signature[72] = {};
    if (!ctDigestHexDecode(sha256Hex, digest) ||
        !ctHexDecode(signatureHex, signature, sizeof(signature))) return false;
    const size_t signatureLength = strlen(signatureHex) / 2;

    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    const int parseResult = mbedtls_pk_parse_public_key(
        &key, reinterpret_cast<const unsigned char*>(CT_OTA_PUBLIC_KEY_PEM),
        sizeof(CT_OTA_PUBLIC_KEY_PEM));
    const int verifyResult = parseResult == 0
        ? mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                            signature, signatureLength)
        : parseResult;
    mbedtls_pk_free(&key);
    memset(digest, 0, sizeof(digest));
    memset(signature, 0, sizeof(signature));
    return verifyResult == 0;
}
#else
// Personal builds do not link the release-signature verifier. Host builds test
// the policy through an injected verifier callback; a native build can never
// validate the device's compiled public key, so this stub always refuses.
bool ctOtaVerifySignature(const char*, const char*) { return false; }
#endif
