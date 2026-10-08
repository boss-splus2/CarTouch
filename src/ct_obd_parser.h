#ifndef CT_OBD_PARSER_H
#define CT_OBD_PARSER_H

#include <stdint.h>
#include <string.h>

struct CtObdSingleFrame {
    uint8_t payloadLength = 0; // bytes after PCI
    uint8_t service = 0;
    uint8_t pid = 0;
    uint8_t dataOffset = 0;
};

struct CtIsoTpReassembly {
    uint32_t canId = 0;
    uint16_t totalLength = 0;
    uint16_t receivedLength = 0;
    uint8_t nextSequence = 1;
    bool isExtended = false;
};

static inline bool ctIsoTpBegin(const uint8_t* frame, uint8_t dlc,
                                uint32_t canId, bool isExtended,
                                uint8_t* payload, uint16_t capacity,
                                CtIsoTpReassembly& state) {
    if (!frame || !payload || dlc != 8 || capacity < 7 ||
        (frame[0] & 0xF0u) != 0x10u) return false;

    const uint16_t totalLength =
        (uint16_t)(((uint16_t)(frame[0] & 0x0Fu) << 8) | frame[1]);
    if (totalLength <= 6 || totalLength > capacity) return false;

    memcpy(payload, frame + 2, 6);
    state.canId = canId;
    state.totalLength = totalLength;
    state.receivedLength = 6;
    state.nextSequence = 1;
    state.isExtended = isExtended;
    return true;
}

static inline bool ctIsoTpAppend(const uint8_t* frame, uint8_t dlc,
                                 uint32_t canId, bool isExtended,
                                 uint8_t* payload, CtIsoTpReassembly& state) {
    if (!frame || !payload || dlc < 2 || dlc > 8 ||
        state.receivedLength >= state.totalLength ||
        canId != state.canId || isExtended != state.isExtended ||
        (frame[0] & 0xF0u) != 0x20u ||
        (frame[0] & 0x0Fu) != state.nextSequence) return false;

    const uint16_t remaining =
        (uint16_t)(state.totalLength - state.receivedLength);
    const uint8_t bytesAvailable = (uint8_t)(dlc - 1u);
    const uint8_t bytesToCopy = remaining < bytesAvailable
        ? (uint8_t)remaining : bytesAvailable;
    memcpy(payload + state.receivedLength, frame + 1, bytesToCopy);
    state.receivedLength = (uint16_t)(state.receivedLength + bytesToCopy);
    state.nextSequence = (uint8_t)((state.nextSequence + 1u) & 0x0Fu);
    return true;
}

static inline bool ctIsoTpComplete(const CtIsoTpReassembly& state) {
    return state.totalLength != 0 &&
           state.receivedLength == state.totalLength;
}

static inline bool ctIsObdReplyFrame(uint32_t canId, bool isExtended, bool isRemote) {
    return !isExtended && !isRemote && canId >= 0x7E8u && canId <= 0x7EFu;
}

// Validates an ISO-TP single-frame OBD response. The frame layout is:
// [PCI length][service][optional PID][payload...][padding].
// expectedPid may be 0xFF when no PID is expected (e.g. Mode 03 DTC).
static inline bool ctParseObdSingleFrame(const uint8_t* frame, uint8_t dlc,
                                         uint8_t expectedService, uint8_t expectedPid,
                                         CtObdSingleFrame& out) {
    if (!frame || dlc < 2) return false;
    const uint8_t pci = frame[0];
    if ((pci & 0xF0u) != 0x00u) return false; // single-frame only
    const uint8_t payloadLen = (uint8_t)(pci & 0x0Fu);
    if (payloadLen == 0 || payloadLen > 7) return false;
    if ((uint16_t)payloadLen + 1u > dlc) return false;
    if (frame[1] != expectedService) return false;

    uint8_t offset = 2;
    if (expectedPid != 0xFFu) {
        if (payloadLen < 2 || frame[2] != expectedPid) return false;
        offset = 3;
    }
    out.payloadLength = payloadLen;
    out.service = frame[1];
    out.pid = (expectedPid == 0xFFu) ? 0 : frame[2];
    out.dataOffset = offset;
    return true;
}

static inline bool ctParseObdPositiveServiceAck(const uint8_t* frame,
                                                uint8_t dlc,
                                                uint8_t expectedService) {
    return frame && dlc >= 2 && frame[0] == 0x01u &&
           frame[1] == expectedService;
}

static inline bool ctParseObdNegativeResponse(const uint8_t* frame,
                                              uint8_t dlc,
                                              uint8_t requestedService,
                                              uint8_t& responseCode) {
    if (!frame || dlc < 4 || frame[0] != 0x03u || frame[1] != 0x7Fu ||
        frame[2] != requestedService) return false;
    responseCode = frame[3];
    return true;
}

static inline bool ctParseObdDtcPayload(const uint8_t* payload,
                                        uint16_t payloadLength,
                                        uint16_t* dtcList,
                                        uint8_t maxCount,
                                        uint8_t& dtcCount) {
    dtcCount = 0;
    if (!payload || !dtcList || maxCount == 0 || payloadLength < 1 ||
        payload[0] != 0x43u || ((payloadLength - 1u) % 2u) != 0u) return false;

    for (uint16_t i = 1; i + 1u < payloadLength && dtcCount < maxCount; i += 2) {
        const uint16_t dtc = (uint16_t)(((uint16_t)payload[i] << 8) | payload[i + 1]);
        if (dtc != 0) dtcList[dtcCount++] = dtc;
    }
    return true;
}

static inline bool ctBuildIsoTpFlowControl(uint8_t* frame, uint8_t capacity,
                                           uint8_t flowStatus,
                                           uint8_t blockSize,
                                           uint8_t separationTime) {
    const bool validSeparationTime = separationTime <= 0x7Fu ||
        (separationTime >= 0xF1u && separationTime <= 0xF9u);
    if (!frame || capacity < 8 || flowStatus > 2 || !validSeparationTime) return false;
    memset(frame, 0, 8);
    frame[0] = (uint8_t)(0x30u | flowStatus);
    frame[1] = blockSize;
    frame[2] = separationTime;
    return true;
}

static inline bool ctDtcPayloadHasValidPairLength(uint8_t payloadLength) {
    // payloadLength includes the positive 0x43 service byte. Remaining bytes
    // must be an integral number of 2-byte DTCs.
    return payloadLength >= 1 && ((payloadLength - 1u) % 2u) == 0u;
}

#endif
