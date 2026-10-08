#ifndef CT_OBD_FORMULAS_H
#define CT_OBD_FORMULAS_H

#include <stdint.h>

static inline uint16_t ctObdEngineRpm(uint8_t a, uint8_t b) {
    return (uint16_t)(((uint16_t)a * 256u + b) / 4u);
}

static inline uint8_t ctObdVehicleSpeed(uint8_t a) {
    return a;
}

static inline int8_t ctObdCoolantTemp(uint8_t a) {
    return (int8_t)((int16_t)a - 40);
}

static inline uint8_t ctObdPercent(uint8_t a) {
    return (uint8_t)((float)a * 100.0f / 255.0f);
}

static inline uint16_t ctObdEngineRuntime(uint8_t a, uint8_t b) {
    return (uint16_t)((uint16_t)a * 256u + b);
}

static inline float ctObdControlModuleVoltage(uint8_t a, uint8_t b) {
    return (float)((uint16_t)a * 256u + b) / 1000.0f;
}

#endif
