#ifndef CT_TEST_ECU_SIM_H
#define CT_TEST_ECU_SIM_H

// Test-only OBD-II ECU simulator. It is NOT part of the firmware: it lives
// in test/ and is only included by the native unit tests. It answers the
// same request frames the firmware sends, so the parsing code can be
// exercised without a car, including bad ECU behaviour.

#include <stdint.h>
#include <string.h>

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Simulator types
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

enum SimEcuMode {
    SIM_ECU_NORMAL = 0,  // correct reply
    SIM_ECU_SILENT,      // no reply at all (unsupported PID, ignition off)
    SIM_ECU_WRONG_PID,   // reply for a different PID than requested
    SIM_ECU_TRUNCATED,   // PCI length says more bytes than the DLC holds
    SIM_ECU_NEGATIVE     // 7F negative response
};

struct SimFrame {
    uint32_t id;
    uint8_t  data[8];
    uint8_t  length;
};

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Mode 01 reply
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// Mode 01 request [02 01 PID] -> single frame reply on 0x7E8.
// Returns number of reply frames written (0 or 1).
static inline uint8_t simEcuMode01(SimEcuMode mode, uint8_t pid,
                                   const uint8_t* value, uint8_t valueLen,
                                   SimFrame* out) {
    if (mode == SIM_ECU_SILENT || valueLen > 4) return 0;
    memset(out, 0, sizeof(*out));
    out->id = 0x7E8;
    out->length = 8;
    if (mode == SIM_ECU_NEGATIVE) {
        out->data[0] = 0x03; out->data[1] = 0x7F; out->data[2] = 0x01; out->data[3] = 0x12;
        return 1;
    }
    out->data[0] = (uint8_t)(2 + valueLen);
    out->data[1] = 0x41;
    out->data[2] = (mode == SIM_ECU_WRONG_PID) ? (uint8_t)(pid ^ 0x01) : pid;
    memcpy(out->data + 3, value, valueLen);
    if (mode == SIM_ECU_TRUNCATED) {
        out->data[0] = 7;  // claims 7 payload bytes
        out->length = 4;   // but only 4 bytes were received
    }
    return 1;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Mode 03 reply
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// Mode 03 reply for a list of DTCs (2 bytes each) as ISO-TP.
// dtcBytes = 2*n. Writes First Frame + Consecutive Frames. When
// dropFrame >= 0, that consecutive-frame index (0-based) is lost on the bus.
// Returns the number of frames written.
static inline uint8_t simEcuMode03(const uint8_t* dtcBytes, uint8_t dtcByteCount,
                                   int dropFrame, SimFrame* out, uint8_t maxOut) {
    uint8_t payload[64];
    const uint8_t total = (uint8_t)(1 + dtcByteCount);
    if (total > sizeof(payload) || total <= 6) return 0;  // multi-frame only
    payload[0] = 0x43;
    memcpy(payload + 1, dtcBytes, dtcByteCount);

    uint8_t n = 0;
    if (maxOut < 1) return 0;
    memset(&out[n], 0, sizeof(SimFrame));
    out[n].id = 0x7E8; out[n].length = 8;
    out[n].data[0] = (uint8_t)(0x10 | (total >> 8));
    out[n].data[1] = total;
    memcpy(out[n].data + 2, payload, 6);
    ++n;

    uint8_t sent = 6, seq = 1;
    int cfIndex = 0;
    while (sent < total) {
        const uint8_t chunk = (uint8_t)((total - sent) < 7 ? (total - sent) : 7);
        if (cfIndex != dropFrame) {
            if (n >= maxOut) return n;
            memset(&out[n], 0, sizeof(SimFrame));
            out[n].id = 0x7E8; out[n].length = 8;
            out[n].data[0] = (uint8_t)(0x20 | (seq & 0x0F));
            memcpy(out[n].data + 1, payload + sent, chunk);
            ++n;
        }
        sent = (uint8_t)(sent + chunk);
        seq = (uint8_t)((seq + 1) & 0x0F);
        ++cfIndex;
    }
    return n;
}

#endif
