#include "ct_ota_lock.h"

CtOtaLock::CtOtaLock() : _mutex(xSemaphoreCreateMutex()), _owner(CT_OTA_OWNER_NONE) {}
CtOtaLock& CtOtaLock::instance() { static CtOtaLock inst; return inst; }

bool CtOtaLock::tryAcquire(CtOtaOwner owner) {
    if (owner == CT_OTA_OWNER_NONE || !_mutex) return false;
    if (xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
    if (_owner == CT_OTA_OWNER_NONE || _owner == owner) { _owner = owner; xSemaphoreGive(_mutex); return true; }
    xSemaphoreGive(_mutex); return false;
}
bool CtOtaLock::owns(CtOtaOwner owner) const { return owner != CT_OTA_OWNER_NONE && _owner == owner; }
void CtOtaLock::release(CtOtaOwner owner) {
    if (!_mutex || owner == CT_OTA_OWNER_NONE) return;
    if (xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        if (_owner == owner) _owner = CT_OTA_OWNER_NONE;
        xSemaphoreGive(_mutex);
    }
}
