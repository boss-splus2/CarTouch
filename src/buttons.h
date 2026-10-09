#pragma once
#include <Arduino.h>
#include "ct_buttons.h"

// Optional physical 5-way keys. Disabled until configured (NVS). Two wirings:
//  GPIO: five pins, active-low with internal pull-up.
//  ADC : one ADC1 pin with a resistor ladder (calibrated values stored).
// A key that was never pressed cannot be told apart from "not wired", so the
// state stays UNVERIFIED until the first valid press in this session.
class Buttons {
public:
    enum Mode : uint8_t { OFF = 0, GPIO_MODE = 1, ADC_MODE = 2 };
    void begin();
    void update();              // poll; call every loop()
    bool poll(CtKeyEvent& ev);  // pop one event
    Mode mode() const { return _mode; }
    bool everPressed() const { return _everPressed; }
    uint32_t invalidReads() const { return _invalid; }
    int pin(uint8_t i) const { return i < 5 ? _pins[i] : -1; }
    int adcPin() const { return _adcPin; }
    const uint16_t* ladder() const { return _ladder; }
    bool setOff();
    bool setGpioPins(const int pins[5]);
    bool setAdc(int pin, const uint16_t ladder[5]);
    bool resetToDefaults();
private:
    void _apply();
    bool _save();
    Mode _mode = OFF;
    int8_t _pins[5] = {-1, -1, -1, -1, -1};
    int8_t _adcPin = -1;
    uint16_t _ladder[5] = {0, 0, 0, 0, 0};
    CtKeyState _state;
    CtKeyEvent _queue[8] = {};
    uint8_t _qHead = 0, _qCount = 0;
    bool _everPressed = false;
    uint32_t _invalid = 0;
    uint32_t _lastPollMs = 0;
};
extern Buttons buttons;
