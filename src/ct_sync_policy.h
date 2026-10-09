/**
 * ct_sync_policy.h - Shared synchronisation for SD and Buttons GPIO
 *
 * SD card (SPI CS pin) and button GPIO conflict detection and serialisation.
 * Provides a shared FreeRTOS recursive mutex to guard concurrent access from
 * Web/BLE threads and main loop.
 */

#pragma once
#include <cstdint>

#ifdef UNIT_TEST
// Stub for unit tests (no FreeRTOS available)
class CtSyncPolicy {
public:
    static CtSyncPolicy& instance() {
        static CtSyncPolicy _inst;
        return _inst;
    }
    bool initMutex() { return true; }
    bool lockWrite() { return true; }
    bool unlockWrite() { return true; }
    bool tryLockWrite() { return true; }
    bool validateSdCsVsButtons(int proposedCs, const int buttonPins[5]);
    bool validateButtonsVsGivenSdCs(const int proposedPins[5], int givenSdCs);
private:
    CtSyncPolicy() {}
};
#else
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class CtSyncPolicy {
public:
    static CtSyncPolicy& instance() {
        static CtSyncPolicy _inst;
        return _inst;
    }

    bool initMutex();
    bool lockWrite();     // acquire for read or write; true = success
    bool unlockWrite();   // release; true = success
    bool tryLockWrite();  // non-blocking acquire for async contexts

    // Conflict detection: does a proposed SD CS pin collide with Button GPIO?
    // Returns true if no conflict; false if unsafe.
    bool validateSdCsVsButtons(int proposedCs, const int buttonPins[5]);

    // Opposite: do proposed button pins collide with current SD CS?
    bool validateButtonsVsGivenSdCs(const int proposedPins[5], int givenSdCs);

private:
    CtSyncPolicy() {}
    SemaphoreHandle_t _mutex = nullptr;
};
#endif

inline CtSyncPolicy& ctSync() { return CtSyncPolicy::instance(); }
