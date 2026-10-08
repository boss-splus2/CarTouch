#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum CtOtaOwner : uint8_t { CT_OTA_OWNER_NONE=0, CT_OTA_OWNER_WEB=1, CT_OTA_OWNER_BLE=2 };

class CtOtaLock {
public:
    static CtOtaLock& instance();
    bool tryAcquire(CtOtaOwner owner);
    bool owns(CtOtaOwner owner) const;
    void release(CtOtaOwner owner);
private:
    CtOtaLock();
    SemaphoreHandle_t _mutex;
    volatile CtOtaOwner _owner;
};
inline CtOtaLock& ctOtaLock() { return CtOtaLock::instance(); }
