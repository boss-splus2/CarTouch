#ifndef CT_CAN_RECORD_H
#define CT_CAN_RECORD_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "can_service.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Record line formatting
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

static inline bool ctFormatCanRecordLine(const CanRxFrame& frame,
                                        char* output, size_t capacity,
                                        size_t& written) {
    if (!output || capacity == 0 || frame.message.length > 8 ||
        (frame.bus != CAN_BUS_1 && frame.bus != CAN_BUS_2)) return false;

    char payload[17] = {};
    if (!frame.message.isRemote) {
        for (uint8_t i = 0; i < frame.message.length; ++i) {
            const int result = snprintf(payload + (i * 2), sizeof(payload) - (i * 2),
                                        "%02X", frame.message.data[i]);
            if (result != 2) return false;
        }
    }

    const int result = snprintf(output, capacity, "%lu,CAN%u,%08lX,%u,%u,%u,%s\n",
                                (unsigned long)frame.receivedAtMs,
                                (unsigned)(frame.bus + 1),
                                (unsigned long)frame.message.id,
                                frame.message.isExtended ? 1u : 0u,
                                frame.message.isRemote ? 1u : 0u,
                                (unsigned)frame.message.length,
                                payload);
    if (result < 0 || (size_t)result >= capacity) return false;
    written = (size_t)result;
    return true;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Filename validation
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

static inline bool ctCanRecordFilenameValid(const char* name) {
    if (!name || strlen(name) != 11 || memcmp(name, "can", 3) != 0 ||
        memcmp(name + 7, ".csv", 4) != 0) return false;
    for (uint8_t i = 3; i < 7; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

#endif