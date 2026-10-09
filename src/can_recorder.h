#ifndef CAN_RECORDER_H
#define CAN_RECORDER_H

#include <Arduino.h>
#include <FS.h>

#include "can_service.h"
#include "ct_storage_policy.h"

class CanRecorder {
public:
    static const uint8_t  BUS_MASK_CAN1       = 0x01;
    static const uint8_t  BUS_MASK_CAN2       = 0x02;
    static const uint8_t  BUS_MASK_BOTH       = BUS_MASK_CAN1 | BUS_MASK_CAN2;
    static const uint32_t MAX_RECORDING_BYTES = 256u * 1024u;
    static const uint16_t MAX_RECORDING_FILES = 100;

    explicit CanRecorder(CANService& canService);

    void setStorageAvailable(bool available);
    bool start(uint8_t busMask);
    bool stop();
    bool deleteRecording(const char* fileName);
    void update();

    bool isRecording() const { return _recording; }
    uint8_t getBusMask() const { return _busMask; }
    uint32_t getFrameCount() const { return _frameCount; }
    uint32_t getDroppedFrameCount() const { return _droppedFrameCount; }
    const char* getFileName() const { return _lastFileName.c_str(); }
    const char* getLastError() const { return _lastError.c_str(); }
    // Where the active/last recording went and why (e.g. fallback notice).
    const char* getLocationText() const { return _loc == CT_LOC_SD ? "sd" : "internal"; }
    const char* getNotice() const { return _notice.c_str(); }

private:
    static const uint32_t MIN_FREE_BYTES      = 1024;
    static const uint32_t FREE_SPACE_CHECK_MS = 1000;

    CANService& _canService;
    File _file;
    bool _storageAvailable;
    bool _recording;
    uint8_t _busMask;
    uint32_t _frameCount;
    uint32_t _droppedFrameCount;
    uint32_t _lastDropCount[2];
    uint32_t _lastFlushMs;
    uint32_t _lastSpaceCheckMs;
    String _lastFileName;
    String _lastError;
    String _notice;
    fs::FS* _fs = nullptr;
    CtStorageLoc _loc = CT_LOC_INTERNAL;
    uint64_t _freeBytes() const;
    uint64_t _totalBytes() const;

    void _closeFile();
    void _fail(const char* error);
    bool _writeFrame(const CanRxFrame& frame);
};

#endif