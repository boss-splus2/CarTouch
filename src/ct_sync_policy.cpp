/**
 * ct_sync_policy.cpp - Synchronisation policy for SD and Buttons
 */

#include "ct_sync_policy.h"

#ifdef UNIT_TEST
// Stub implementation for unit tests
bool CtSyncPolicy::validateSdCsVsButtons(int proposedCs, const int buttonPins[5]) {
    if (proposedCs < 0) return true;
    if (!buttonPins) return true;
    for (uint8_t i = 0; i < 5; i++) {
        if (buttonPins[i] >= 0 && buttonPins[i] == proposedCs) return false;
    }
    return true;
}

bool CtSyncPolicy::validateButtonsVsGivenSdCs(const int proposedPins[5], int givenSdCs) {
    if (givenSdCs < 0) return true;
    if (!proposedPins) return true;
    for (uint8_t i = 0; i < 5; i++) {
        if (proposedPins[i] >= 0 && proposedPins[i] == givenSdCs) return false;
    }
    return true;
}
#else
// Full implementation with FreeRTOS
#include <Arduino.h>

bool CtSyncPolicy::initMutex() {
    if (_mutex != nullptr) return true;
    _mutex = xSemaphoreCreateRecursiveMutex();
    return _mutex != nullptr;
}

bool CtSyncPolicy::lockWrite() {
    if (_mutex == nullptr) return false;
    return xSemaphoreTakeRecursive(_mutex, portMAX_DELAY) == pdTRUE;
}

bool CtSyncPolicy::unlockWrite() {
    if (_mutex == nullptr) return false;
    return xSemaphoreGiveRecursive(_mutex) == pdTRUE;
}

bool CtSyncPolicy::tryLockWrite() {
    if (_mutex == nullptr) return false;
    return xSemaphoreTakeRecursive(_mutex, 0) == pdTRUE;
}

bool CtSyncPolicy::validateSdCsVsButtons(int proposedCs, const int buttonPins[5]) {
    if (proposedCs < 0) return true;
    if (!buttonPins) return true;
    for (uint8_t i = 0; i < 5; i++) {
        if (buttonPins[i] >= 0 && buttonPins[i] == proposedCs) return false;
    }
    return true;
}

bool CtSyncPolicy::validateButtonsVsGivenSdCs(const int proposedPins[5], int givenSdCs) {
    if (givenSdCs < 0) return true;
    if (!proposedPins) return true;
    for (uint8_t i = 0; i < 5; i++) {
        if (proposedPins[i] >= 0 && proposedPins[i] == givenSdCs) return false;
    }
    return true;
}
#endif
