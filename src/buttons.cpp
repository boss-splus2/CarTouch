#include "buttons.h"
#include <nvs.h>
#include "config.h"
#include "sd_storage.h"
#include "ct_sync_policy.h"

Buttons buttons;

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Constants and helpers
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

static const uint16_t ADC_TOL = 80;

static void inUsePins(int* out, size_t& n) {
    const AppConfig* c = getConfig();
    const int base[] = { PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST, PIN_TFT_MOSI, PIN_TFT_SCLK, PIN_TFT_MISO,
                         PIN_TFT_BL, PIN_TOUCH_CS, c->canTxPin, c->canRxPin, c->can1CsPin, c->can1IntPin,
                         sdStorage.csPin() };
    n = sizeof(base) / sizeof(base[0]);
    for (size_t i = 0; i < n; i++) out[i] = base[i];
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Setup and polling
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

void Buttons::begin() {
    nvs_handle_t h;
    uint8_t m = OFF;
    if (nvs_open("CarTouch", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "btn_mode", &m);
        size_t len = sizeof(_pins);
        nvs_get_blob(h, "btn_pins", _pins, &len);
        nvs_get_i8(h, "btn_adc", &_adcPin);
        len = sizeof(_ladder);
        nvs_get_blob(h, "btn_lad", _ladder, &len);
        nvs_close(h);
    }
    _mode = (m <= ADC_MODE) ? (Mode)m : OFF;
    // Re-validate stored values: a corrupt entry must never configure a bad pin.
    int used[16]; size_t n; inUsePins(used, n);
    if (_mode == GPIO_MODE) {
        for (uint8_t i = 0; i < 5; i++) {
            if (!ctSdCsPinAllowed(_pins[i], used, n)) { _mode = OFF; break; }
            for (uint8_t j = i + 1; j < 5; j++) if (_pins[i] == _pins[j]) _mode = OFF;
        }
    } else if (_mode == ADC_MODE) {
        if (!ctAdcPinAllowed(_adcPin, used, n) || !ctLadderValid(_ladder, ADC_TOL)) _mode = OFF;
    }
    _apply();
}

void Buttons::_apply() {
    _state = CtKeyState();
    _qHead = _qCount = 0;
    _everPressed = false;
    _invalid = 0;
    if (_mode == GPIO_MODE) {
        for (uint8_t i = 0; i < 5; i++) pinMode(_pins[i], INPUT_PULLUP);
    } else if (_mode == ADC_MODE) {
        pinMode(_adcPin, INPUT);
        analogReadResolution(12);
    }
}

void Buttons::update() {
    if (_mode == OFF) return;
    const uint32_t now = millis();
    if (now - _lastPollMs < 5) return;
    _lastPollMs = now;

    CtKey raw = CT_KEY_NONE;
    if (_mode == GPIO_MODE) {
        for (uint8_t i = 0; i < 5; i++) {
            if (digitalRead(_pins[i]) == LOW) { raw = (CtKey)(i + 1); break; }
        }
    } else {
        raw = ctAdcClassify((uint16_t)analogRead(_adcPin), _ladder, ADC_TOL);
        if (raw == CT_KEY_INVALID) _invalid++;
    }
    const CtKeyEvent ev = ctKeyStep(_state, raw, now);
    if (ev.type == CT_EV_NONE) return;
    if (ev.type == CT_EV_PRESS) _everPressed = true;
    if (_qCount < 8) { _queue[(_qHead + _qCount) % 8] = ev; _qCount++; }  // drop when full
}

bool Buttons::poll(CtKeyEvent& ev) {
    if (_qCount == 0) return false;
    ev = _queue[_qHead];
    _qHead = (_qHead + 1) % 8;
    _qCount--;
    return true;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Configuration
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool Buttons::_save() {
    nvs_handle_t h;
    if (nvs_open("CarTouch", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_u8(h, "btn_mode", (uint8_t)_mode);
    if (e == ESP_OK) e = nvs_set_blob(h, "btn_pins", _pins, sizeof(_pins));
    if (e == ESP_OK) e = nvs_set_i8(h, "btn_adc", _adcPin);
    if (e == ESP_OK) e = nvs_set_blob(h, "btn_lad", _ladder, sizeof(_ladder));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

bool Buttons::setOff() {
    if (!ctSync().lockWrite()) return false;
    struct Unlock { ~Unlock(){ ctSync().unlockWrite(); } } unlock;
    const Mode old = _mode;
    _mode = OFF;
    if (!_save()) { _mode = old; return false; }
    _apply();
    return true;
}

bool Buttons::setGpioPins(const int pins[5]) {
    if (!ctSync().lockWrite()) return false;
    struct Unlock { ~Unlock(){ ctSync().unlockWrite(); } } unlock;
    int used[16]; size_t n; inUsePins(used, n);
    for (uint8_t i = 0; i < 5; i++) {
        if (!ctSdCsPinAllowed(pins[i], used, n)) return false;
        for (uint8_t j = i + 1; j < 5; j++) if (pins[i] == pins[j]) return false;
    }
    // Cross-check against SD CS configuration
    if (!ctSync().validateButtonsVsGivenSdCs(pins, sdStorage.csPin())) {
        Serial.println("[BTN] GPIO pins conflict with SD CS pin configuration");
        return false;
    }
    int8_t oldPins[5]; memcpy(oldPins, _pins, 5); const Mode oldMode = _mode;
    for (uint8_t i = 0; i < 5; i++) _pins[i] = (int8_t)pins[i];
    _mode = GPIO_MODE;
    if (!_save()) { memcpy(_pins, oldPins, 5); _mode = oldMode; return false; }
    _apply();
    return true;
}

bool Buttons::setAdc(int pin, const uint16_t ladder[5]) {
    if (!ctSync().lockWrite()) return false;
    struct Unlock { ~Unlock(){ ctSync().unlockWrite(); } } unlock;
    int used[16]; size_t n; inUsePins(used, n);
    if (!ctAdcPinAllowed(pin, used, n) || !ctLadderValid(ladder, ADC_TOL)) return false;
    const int8_t oldPin = _adcPin; uint16_t oldLad[5]; memcpy(oldLad, _ladder, sizeof(oldLad));
    const Mode oldMode = _mode;
    _adcPin = (int8_t)pin; memcpy(_ladder, ladder, sizeof(_ladder)); _mode = ADC_MODE;
    if (!_save()) { _adcPin = oldPin; memcpy(_ladder, oldLad, sizeof(_ladder)); _mode = oldMode; return false; }
    _apply();
    return true;
}

bool Buttons::resetToDefaults() {
    if (!ctSync().lockWrite()) return false;
    struct Unlock { ~Unlock(){ ctSync().unlockWrite(); } } unlock;
    const Mode oldMode = _mode;
    int8_t oldPins[5];
    const int8_t oldAdcPin = _adcPin;
    uint16_t oldLadder[5];
    memcpy(oldPins, _pins, sizeof(oldPins));
    memcpy(oldLadder, _ladder, sizeof(oldLadder));

    _mode = OFF;
    memset(_pins, -1, sizeof(_pins));
    _adcPin = -1;
    memset(_ladder, 0, sizeof(_ladder));
    if (!_save()) {
        _mode = oldMode;
        memcpy(_pins, oldPins, sizeof(_pins));
        _adcPin = oldAdcPin;
        memcpy(_ladder, oldLadder, sizeof(_ladder));
        return false;
    }
    _apply();
    return true;
}
