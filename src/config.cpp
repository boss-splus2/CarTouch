/**
 * config.cpp - Configuration management implementation
 *
 * Settings are persisted in the ESP32's NVS (Non-Volatile Storage) and
 * survive reboots.
 */

#include "config.h"
#include "ct_password.h"
#include "sd_storage.h"
#include "buttons.h"
#include <esp_random.h>
#include "ct_can_config.h"
#include "ct_listen_override.h"
#include <nvs_flash.h>
#include <nvs.h>

static AppConfig currentConfig;
static bool       configLoaded = false;

static PasswordChangeCallback _passwordChangeCallback = nullptr;

static bool isReservedProjectPin(uint8_t pin) {
    // Fixed project peripherals.
    switch (pin) {
        case PIN_TFT_CS: case PIN_TFT_DC: case PIN_TFT_RST:
        case PIN_TFT_MOSI: case PIN_TFT_SCLK: case PIN_TFT_MISO:
        case PIN_TFT_BL: case PIN_TOUCH_CS: case PIN_LED_INTERNAL:
            return true;
        default: break;
    }

    // ESP32-S3 N16R8 with qio_opi uses these GPIOs for Octal flash/PSRAM.
    // They must never be offered as runtime CAN pins.
    if (pin >= 26 && pin <= 37) return true;

    // GPIO22..25 do not exist on ESP32-S3 (twai_driver_install would fail).
    if (pin >= 22 && pin <= 25) return true;

    // Boot/USB/JTAG/strapping-sensitive pins that are intentionally not
    // exposed through the runtime CAN pin selector. GPIO43/44 are UART0
    // TX/RX (the Serial console / USB-UART bridge used for flashing).
    switch (pin) {
        case 0: case 3: case 19: case 20: case 43: case 44: case 45: case 46:
            return true;
        default:
            return false;
    }
}

bool isValidCanPin(uint8_t pin) {
    if (pin > 48) return false;
    return !isReservedProjectPin(pin);
}

bool validateCanPins(uint8_t txPin, uint8_t rxPin) {
    return validateCanPinAssignment(txPin, rxPin,
                                    currentConfig.can1CsPin,
                                    currentConfig.can1IntPin);
}

bool validateCan1Pins(uint8_t csPin, uint8_t intPin) {
    return validateCanPinAssignment(currentConfig.canTxPin,
                                    currentConfig.canRxPin,
                                    csPin, intPin);
}

bool validateCanPinAssignment(uint8_t txPin, uint8_t rxPin,
                              uint8_t csPin, uint8_t intPin) {
    return ctCanPinsConflictFree(txPin, rxPin, csPin, intPin) &&
           isValidCanPin(txPin) && isValidCanPin(rxPin) &&
           isValidCanPin(csPin) && isValidCanPin(intPin);
}

bool isValidCanSpeed(uint32_t speed) {
    switch (speed) {
        case 100000:
        case 125000:
        case 250000:
        case 500000:
        case 800000:
        case 1000000:
            return true;
        default:
            return false;
    }
}

bool isValidCan1Speed(uint32_t speed) {
    return ctMcp2515BitrateValid(speed);
}

void registerPasswordChangeCallback(PasswordChangeCallback cb) {
    _passwordChangeCallback = cb;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Load / save
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

static void applyDefaultConfigValues() {
    memset(&currentConfig, 0, sizeof(currentConfig));
    strcpy(currentConfig.wifiSSID, "");
    strcpy(currentConfig.wifiPassword, "");
    currentConfig.wifiEnabled = true;
    strcpy(currentConfig.webUser, WEB_DEFAULT_USER);
    strcpy(currentConfig.webPass, WEB_DEFAULT_PASS);
    currentConfig.forcePasswordChange = false;
    strcpy(currentConfig.vehicleBrand, "Generic");
    strcpy(currentConfig.vehicleModel, "OBD-II");
    currentConfig.vehicleYear = 2020;
    currentConfig.theme = THEME_AUTO;
    currentConfig.brightnessDay = TFT_BRIGHTNESS_DAY;
    currentConfig.brightnessNight = TFT_BRIGHTNESS_NIGHT;
    currentConfig.canTxPin = PIN_CAN_TX;
    currentConfig.canRxPin = PIN_CAN_RX;
    currentConfig.canSpeed = CAN_SPEED;
    currentConfig.listenOnlyMode = true;
    currentConfig.can1CsPin = PIN_CAN1_CS;
    currentConfig.can1IntPin = PIN_CAN1_INT;
    currentConfig.can1Speed = CAN1_SPEED;
    currentConfig.can1ListenOnly = CAN1_LISTEN_ONLY;
    currentConfig.sleepTimeout = AUTO_SLEEP_TIMEOUT;
    currentConfig.touchCalibrated = false;
    memset(currentConfig.touchCalData, 0, sizeof(currentConfig.touchCalData));
    currentConfig.configMagic = 0xCAFE1234;
}

bool loadConfig() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.printf("[NVS] Init returned %d; preserving stored data and using RAM defaults\n", (int)err);
    }
    if (err != ESP_OK) {
        Serial.printf("[NVS] Init failed (%d) - using in-memory defaults; settings will not persist\n", (int)err);
        applyDefaultConfigValues();
        configLoaded = true;
        return true;
    }

    nvs_handle_t nvsHandle;
    err = nvs_open("CarTouch", NVS_READWRITE, &nvsHandle);
    if (err != ESP_OK) {
        Serial.println("[NVS] Failed to open namespace - using in-memory defaults");
        applyDefaultConfigValues();
        configLoaded = true;
        return true;
    }

    const size_t legacyConfigSize = offsetof(AppConfig, can1CsPin);
    const size_t priorCan2ConfigSize = offsetof(AppConfig, obdCanBus);
    const size_t priorObdConfigSize =
        ((offsetof(AppConfig, learnCanBus) + alignof(AppConfig) - 1) /
         alignof(AppConfig)) * alignof(AppConfig);
    size_t configSize = sizeof(AppConfig);
    memset(&currentConfig, 0, sizeof(currentConfig));
    err = nvs_get_blob(nvsHandle, "config", &currentConfig, &configSize);
    nvs_close(nvsHandle);

    const bool legacyConfig = err == ESP_OK && configSize == legacyConfigSize;
    const bool priorCan2Config = err == ESP_OK && configSize == priorCan2ConfigSize;
    // NOTE: with the current layout priorObdConfigSize == sizeof(AppConfig),
    // so a size match alone cannot identify an older blob. It is only a
    // distinct "prior" layout when strictly smaller; otherwise the previously
    // saved obd/learn bus bytes must be kept (they used to be reset to 0 on
    // every boot, so a Learn route chosen on CAN2 was lost after reboot).
    const bool priorObdConfig = err == ESP_OK && configSize == priorObdConfigSize &&
                                priorObdConfigSize < sizeof(AppConfig);
    if (legacyConfig) {
        currentConfig.can1CsPin = PIN_CAN1_CS;
        currentConfig.can1IntPin = PIN_CAN1_INT;
        currentConfig.can1Speed = CAN1_SPEED;
        currentConfig.can1ListenOnly = CAN1_LISTEN_ONLY;
    }
    if (legacyConfig || priorCan2Config) {
        currentConfig.obdCanBus = 0;
    }
    if (legacyConfig || priorCan2Config || priorObdConfig) {
        currentConfig.learnCanBus = 0;
    }
    if (err == ESP_OK) {
        // Bus-route bytes may sit in old padding: sanitise instead of
        // discarding the whole configuration (Wi-Fi, password, pins...).
        currentConfig.obdCanBus     = ctSanitizeBusIndex(currentConfig.obdCanBus);
        currentConfig.learnCanBus   = ctSanitizeBusIndex(currentConfig.learnCanBus);
        currentConfig.vehicleCanBus = ctSanitizeBusIndex(currentConfig.vehicleCanBus);
    }

    const bool validStoredConfig =
        err == ESP_OK &&
        (configSize == sizeof(AppConfig) || priorObdConfig ||
         priorCan2Config || legacyConfig) &&
        currentConfig.obdCanBus <= 1 &&
        currentConfig.learnCanBus <= 1 &&
        currentConfig.configMagic == 0xCAFE1234 &&
        validateCanPinAssignment(currentConfig.canTxPin, currentConfig.canRxPin,
                                 currentConfig.can1CsPin, currentConfig.can1IntPin) &&
        isValidCanSpeed(currentConfig.canSpeed) &&
        isValidCan1Speed(currentConfig.can1Speed);

    if (!validStoredConfig) {
        Serial.println("[NVS] Missing, incompatible, or invalid configuration - restoring defaults");
        applyDefaultConfigValues();
        if (!saveConfig()) {
            Serial.println("[NVS] Failed to persist default configuration");
            configLoaded = false;
            return false;
        }
    }

    // Blob came from flash: guarantee NUL termination before any string use.
    currentConfig.wifiSSID[sizeof(currentConfig.wifiSSID) - 1] = '\0';
    currentConfig.wifiPassword[sizeof(currentConfig.wifiPassword) - 1] = '\0';
    currentConfig.webUser[sizeof(currentConfig.webUser) - 1] = '\0';
    currentConfig.webPass[sizeof(currentConfig.webPass) - 1] = '\0';
    currentConfig.vehicleBrand[sizeof(currentConfig.vehicleBrand) - 1] = '\0';
    currentConfig.vehicleModel[sizeof(currentConfig.vehicleModel) - 1] = '\0';

    // Devices flashed with the old firmware hold a random temporary password
    // that nobody could read without a serial cable, and BLE stayed locked
    // until it was changed. If that password was never changed, replace it
    // with the default login (other settings are kept). The same is done for
    // a missing user name or password (damaged data): without this the Wi-Fi
    // access point could start with no password at all.
    if (currentConfig.forcePasswordChange || currentConfig.webUser[0] == '\0' ||
        strlen(currentConfig.webPass) < 8) {
        strcpy(currentConfig.webUser, WEB_DEFAULT_USER);
        strcpy(currentConfig.webPass, WEB_DEFAULT_PASS);
        currentConfig.forcePasswordChange = false;
        if (!saveConfig()) {
            Serial.println("[NVS] Failed to persist default login");
        }
    }
    configLoaded = true;
    Serial.println(validStoredConfig ? "[NVS] Configuration loaded" : "[NVS] Default configuration loaded");
    return true;
}

static bool s_learnForced[2]     = {false, false};
static bool s_learnUserChoice[2] = {false, false};

void configSetLearnListenOverride(uint8_t busIndex, bool forced, bool userChoice) {
    if (busIndex > 1) return;
    s_learnForced[busIndex]     = forced;
    s_learnUserChoice[busIndex] = userChoice;
}

bool saveConfig() {
    nvs_handle_t nvsHandle;
    esp_err_t err = nvs_open("CarTouch", NVS_READWRITE, &nvsHandle);
    if (err != ESP_OK) {
        Serial.println("[NVS] Failed to open for saving");
        return false;
    }

    currentConfig.configMagic = 0xCAFE1234;
    // Write a copy: the temporary Learn Listen-Only override must not reach flash.
    AppConfig toSave = currentConfig;
    toSave.listenOnlyMode = ctPersistedListenOnly(currentConfig.listenOnlyMode,
                                                  s_learnForced[0], s_learnUserChoice[0]);
    toSave.can1ListenOnly = ctPersistedListenOnly(currentConfig.can1ListenOnly,
                                                  s_learnForced[1], s_learnUserChoice[1]);
    err = nvs_set_blob(nvsHandle, "config", &toSave, sizeof(AppConfig));

    if (err == ESP_OK) {
        err = nvs_commit(nvsHandle);
    }

    nvs_close(nvsHandle);

    if (err == ESP_OK) {
        Serial.println("[NVS] Configuration saved");
        return true;
    }

    Serial.println("[NVS] Save failed");
    return false;
}

bool isTouchCalibrationSkipped() {
    nvs_handle_t h;
    if (nvs_open("CarTouch", NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, "touch_skip", &v);
    nvs_close(h);
    return err == ESP_OK && v != 0;
}

void setTouchCalibrationSkipped(bool skipped) {
    nvs_handle_t h;
    if (nvs_open("CarTouch", NVS_READWRITE, &h) != ESP_OK) {
        Serial.println("[NVS] Could not store touch skip flag");
        return;
    }
    esp_err_t err = nvs_set_u8(h, "touch_skip", skipped ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) Serial.println("[NVS] Touch skip flag save failed");
}

AppConfig* getConfig() {
    if (!configLoaded) {
        loadConfig();
    }
    return &currentConfig;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Password helpers
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool isUsingDefaultPassword() {
#if CT_REQUIRE_PASSWORD_CHANGE
    // Real comparison with the compiled-in default (the old version only read
    // a flag that was never set, so the "refuse the default" checks in BLE did
    // nothing).
    return strcmp(getConfig()->webPass, WEB_DEFAULT_PASS) == 0;
#else
    // Development setting: the default login is allowed everywhere.
    return false;
#endif
}

bool setWebPassword(const char* newUser, const char* newPass) {
    if (!newPass || strlen(newPass) < 8) {
        Serial.println("[CONFIG] New password must be at least 8 characters");
        return false;
    }
    if (strlen(newPass) >= sizeof(getConfig()->webPass)) {
        Serial.println("[CONFIG] New password is too long (max 15 characters)");
        return false;
    }
#if CT_REQUIRE_PASSWORD_CHANGE
    if (strcmp(newPass, WEB_DEFAULT_PASS) == 0) {
        Serial.println("[CONFIG] New password must differ from the default");
        return false;
    }
#endif

    AppConfig* cfg = getConfig();
    // Keep the old credentials so a failed NVS write does not leave RAM and
    // flash disagreeing (RAM would accept a password that is lost on reboot).
    char oldUser[sizeof(cfg->webUser)], oldPass[sizeof(cfg->webPass)];
    memcpy(oldUser, cfg->webUser, sizeof(oldUser));
    memcpy(oldPass, cfg->webPass, sizeof(oldPass));
    const bool oldForce = cfg->forcePasswordChange;
    if (newUser && strlen(newUser) > 0) {
        strncpy(cfg->webUser, newUser, sizeof(cfg->webUser) - 1);
        cfg->webUser[sizeof(cfg->webUser) - 1] = '\0';
    }
    strncpy(cfg->webPass, newPass, sizeof(cfg->webPass) - 1);
    cfg->webPass[sizeof(cfg->webPass) - 1] = '\0';
    cfg->forcePasswordChange = false;

    bool saved = saveConfig();
    if (!saved) {
        memcpy(cfg->webUser, oldUser, sizeof(oldUser));
        memcpy(cfg->webPass, oldPass, sizeof(oldPass));
        cfg->forcePasswordChange = oldForce;
    }

    // Regardless of which interface called this (TFT or web), notify any
    // module holding its own session state so it can invalidate stale
    // sessions right away.
    if (saved && _passwordChangeCallback) {
        _passwordChangeCallback();
    }

    return saved;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Defaults
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

void setDefaultConfig() {
    applyDefaultConfigValues();
    // Factory Reset must also clear the separate touch-skip flag so the
    // touch wizard is offered again.
    setTouchCalibrationSkipped(false);
    resetStorageChoices();   // storage choices back to AUTO (SD CS pin is kept)
    if (!buttons.resetToDefaults()) {
        Serial.println("[NVS] Button defaults reset failed");
    }
    if (saveConfig()) {
        configLoaded = true;
    }
}
