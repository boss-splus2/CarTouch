#pragma once
#include <Arduino.h>
#include "ct_storage_policy.h"

// Optional SD card on the shared SPI bus. Disabled until a chip-select pin is
// stored (NVS key "sd_cs"); a missing card is a normal state, never an error
// for the rest of the firmware. Used by the CAN recorder and the DBC store
// (custom profiles are still kept in SPIFFS only).
class SdStorage {
public:
    enum State : uint8_t { SD_DISABLED, NOT_PRESENT, READY, ERROR_STATE };
    void begin();            // read pin from NVS, try one mount
    void update();           // call from loop(); rate-limited, no blocking retries
    State state() const { return _state; }
    int  csPin() const { return _cs; }
    uint64_t totalBytes() const { return _total; }
    uint64_t freeBytes() const;
    bool setCsPin(int pin);  // validates, stores in NVS, remounts. -1 disables.
    const char* stateText() const;
private:
    bool _mount();
    void _unmount();
    State _state = SD_DISABLED;
    int _cs = -1;
    uint64_t _total = 0;
    uint32_t _lastTryMs = 0;
    uint32_t _lastCheckMs = 0;
};
extern SdStorage sdStorage;

CtStorageChoice getStorageChoice(const char* category);       // "db","rec","prof","bak"
bool setStorageChoice(const char* category, uint8_t choice);  // stored in NVS
void resetStorageChoices();                                   // back to AUTO
