#pragma once
// Validation of the 24-byte ESP image header at the start of a firmware
// image (no hardware dependencies, native-testable).
//
// Why: an image built for another board variant can fit the OTA slot and be
// accepted by Update.end(), then fail to boot with no rollback available.
// The header tells us the target chip and the flash size the image was built
// for, so the obvious mix-ups (wrong chip, 16 MB build on a 4 MB board) can be
// refused before the device reboots into them.
//
// Layout (ESP-IDF esp_image_header_t): byte 0 magic 0xE9, byte 1 segment
// count, byte 3 high nibble = flash size code, bytes 12..13 = chip id (LE).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CT_OTA_HEADER_LEN     24
#define CT_OTA_IMAGE_MAGIC    0xE9
#define CT_OTA_CHIP_ESP32S3   0x0009
#define CT_OTA_MAX_SEGMENTS   16

enum CtOtaHdrResult : uint8_t {
    CT_OTA_HDR_NEED_MORE = 0,    // fewer than 24 bytes seen so far
    CT_OTA_HDR_OK,
    CT_OTA_HDR_BAD_MAGIC,
    CT_OTA_HDR_BAD_SEGMENTS,
    CT_OTA_HDR_WRONG_CHIP,
    CT_OTA_HDR_FLASH_TOO_BIG     // built for more flash than this device has
};

// Collects the header across chunks (BLE writes can be as small as 20 bytes).
struct CtOtaHeaderCheck {
    uint8_t buf[CT_OTA_HEADER_LEN];
    uint8_t have;
    bool    done;                // header complete and accepted
    CtOtaHeaderCheck() : have(0), done(false) { memset(buf, 0, sizeof(buf)); }
    void reset() { have = 0; done = false; memset(buf, 0, sizeof(buf)); }
};

// Flash size in bytes for the header's size code, 0 if the code is unknown.
static inline uint32_t ctOtaFlashCodeBytes(uint8_t code) {
    return code <= 7 ? ((uint32_t)1048576u << code) : 0u;
}

// Feed the next bytes of the image. Returns NEED_MORE until 24 header bytes
// have arrived, then OK or the reason for refusal. A wrong first byte is
// refused immediately. flashBytes is the detected flash chip size.
static inline CtOtaHdrResult ctOtaHeaderFeed(CtOtaHeaderCheck& st,
                                             const uint8_t* data, size_t len,
                                             uint32_t flashBytes) {
    if (st.done) return CT_OTA_HDR_OK;
    if (!data || len == 0) return CT_OTA_HDR_NEED_MORE;
    if (st.have == 0 && data[0] != CT_OTA_IMAGE_MAGIC) return CT_OTA_HDR_BAD_MAGIC;

    size_t take = CT_OTA_HEADER_LEN - st.have;
    if (take > len) take = len;
    memcpy(st.buf + st.have, data, take);
    st.have = (uint8_t)(st.have + take);
    if (st.have < CT_OTA_HEADER_LEN) return CT_OTA_HDR_NEED_MORE;

    const uint8_t segments = st.buf[1];
    if (segments == 0 || segments > CT_OTA_MAX_SEGMENTS) return CT_OTA_HDR_BAD_SEGMENTS;

    const uint16_t chip = (uint16_t)(st.buf[12] | (st.buf[13] << 8));
    if (chip != CT_OTA_CHIP_ESP32S3) return CT_OTA_HDR_WRONG_CHIP;

    const uint32_t imageFlash = ctOtaFlashCodeBytes((uint8_t)(st.buf[3] >> 4));
    if (imageFlash == 0 || imageFlash > flashBytes) return CT_OTA_HDR_FLASH_TOO_BIG;

    st.done = true;
    return CT_OTA_HDR_OK;
}

// Text for the web response / log.
static inline const char* ctOtaHeaderMessage(CtOtaHdrResult r) {
    switch (r) {
        case CT_OTA_HDR_BAD_MAGIC:    return "Not a valid ESP32 firmware image (bad header)";
        case CT_OTA_HDR_BAD_SEGMENTS: return "Not a valid ESP32 firmware image (bad segment count)";
        case CT_OTA_HDR_WRONG_CHIP:   return "This firmware is not built for the ESP32-S3";
        case CT_OTA_HDR_FLASH_TOO_BIG:return "This firmware was built for a larger flash than this device has (wrong board variant)";
        default:                      return "Firmware header check incomplete";
    }
}
