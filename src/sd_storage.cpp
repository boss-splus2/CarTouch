#include "sd_storage.h"
#include <SD.h>
#include <SPI.h>
#include <nvs.h>
#include "config.h"
#include "buttons.h"
#include "ct_sync_policy.h"

SdStorage sdStorage;

static const uint32_t SD_RETRY_MS = 5000;
static const uint32_t SD_CHECK_MS = 2000;
static const uint32_t SD_SPI_HZ   = 10000000;   // conservative on a shared bus

static bool nvsOpen(nvs_handle_t& h, bool write) {
    return nvs_open("CarTouch", write ? NVS_READWRITE : NVS_READONLY, &h) == ESP_OK;
}

void SdStorage::begin() {
    nvs_handle_t h;
    int8_t pin = -1;
    if (nvsOpen(h, false)) { nvs_get_i8(h, "sd_cs", &pin); nvs_close(h); }
    _cs = pin;
    if (_cs < 0) { _state = SD_DISABLED; return; }
    _mount();
}

bool SdStorage::_mount() {
    _lastTryMs = millis();
    if (_cs < 0) { _state = SD_DISABLED; return false; }
    // Never format: SD.begin(..., format_if_empty=false).
    if (!SD.begin(_cs, SPI, SD_SPI_HZ, "/sd", 5, false) || SD.cardType() == CARD_NONE) {
        SD.end();
        _total = 0;
        _state = NOT_PRESENT;
        return false;
    }
    _total = SD.totalBytes();
    _state = _total > 0 ? READY : ERROR_STATE;
    return _state == READY;
}

void SdStorage::_unmount() {
    SD.end();
    _total = 0;
    _state = (_cs < 0) ? SD_DISABLED : NOT_PRESENT;
}

void SdStorage::update() {
    const uint32_t now = millis();
    if (_state == SD_DISABLED) return;
    if (_state == READY) {
        if (now - _lastCheckMs < SD_CHECK_MS) return;
        _lastCheckMs = now;
        // cardType() is cached by the library, so probe by opening the root
        // directory. Whether this notices a pulled card on real hardware
        // still needs a bench test.
        File root = SD.open("/");
        const bool alive = root && root.isDirectory();
        if (root) root.close();
        if (!alive || SD.cardType() == CARD_NONE) {
            Serial.println("[SD] Card removed or not responding");
            _unmount();
        }
        return;
    }
    if (now - _lastTryMs >= SD_RETRY_MS) _mount();
}

uint64_t SdStorage::freeBytes() const {
    if (_state != READY) return 0;
    const uint64_t t = SD.totalBytes(), u = SD.usedBytes();
    return t > u ? t - u : 0;
}

bool SdStorage::setCsPin(int pin) {
    if (!ctSync().lockWrite()) return false;
    struct Unlock { ~Unlock(){ ctSync().unlockWrite(); } } unlock;
    if (pin >= 0) {
        const AppConfig* c = getConfig();
        const int inUse[] = { PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST, PIN_TFT_MOSI, PIN_TFT_SCLK,
                              PIN_TFT_MISO, PIN_TFT_BL, PIN_TOUCH_CS, c->canTxPin, c->canRxPin,
                              c->can1CsPin, c->can1IntPin };
        if (!ctSdCsPinAllowed(pin, inUse, sizeof(inUse) / sizeof(inUse[0]))) return false;
        // Cross-check against button GPIO configuration
        int btnPins[5] = {-1, -1, -1, -1, -1};
        if (buttons.mode() == Buttons::GPIO_MODE) {
            for (uint8_t i = 0; i < 5; i++) btnPins[i] = buttons.pin(i);
        }
        if (!ctSync().validateSdCsVsButtons(pin, btnPins)) {
            Serial.println("[SD] CS pin conflicts with button GPIO configuration");
            return false;
        }
    }
    nvs_handle_t h;
    if (!nvsOpen(h, true)) return false;
    esp_err_t e = nvs_set_i8(h, "sd_cs", (int8_t)pin);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return false;
    if (_state == READY) _unmount();
    _cs = pin;
    if (_cs < 0) _state = SD_DISABLED; else _mount();
    return true;
}

const char* SdStorage::stateText() const {
    switch (_state) {
        case READY: return "READY";
        case NOT_PRESENT: return "NOT DETECTED";
        case ERROR_STATE: return "ERROR";
        default: return "DISABLED (no CS pin set)";
    }
}

// ---- per-category storage choice (NVS u8 keys; AUTO is the default) ----
static const char* choiceKey(const char* c) {
    if (!c) return nullptr;
    if (!strcmp(c, "db"))   return "st_db";
    if (!strcmp(c, "rec"))  return "st_rec";
    if (!strcmp(c, "prof")) return "st_prof";
    if (!strcmp(c, "bak"))  return "st_bak";
    return nullptr;
}

CtStorageChoice getStorageChoice(const char* category) {
    const char* key = choiceKey(category);
    nvs_handle_t h;
    uint8_t v = CT_STORE_AUTO;
    if (key && nvsOpen(h, false)) { nvs_get_u8(h, key, &v); nvs_close(h); }
    return ctStorageChoiceValid(v) ? (CtStorageChoice)v : CT_STORE_AUTO;
}

bool setStorageChoice(const char* category, uint8_t choice) {
    const char* key = choiceKey(category);
    if (!key || !ctStorageChoiceValid(choice)) return false;
    nvs_handle_t h;
    if (!nvsOpen(h, true)) return false;
    esp_err_t e = nvs_set_u8(h, key, choice);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

void resetStorageChoices() {
    static const char* cats[] = { "db", "rec", "prof", "bak" };
    for (const char* c : cats) setStorageChoice(c, CT_STORE_AUTO);
}
