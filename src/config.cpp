/**
 * config.cpp - Configuration management implementation
 *
 * Settings are persisted in the ESP32's NVS (Non-Volatile Storage) and
 * survive reboots.
 */

#include "config.h"
#include "ct_password.h"
#include "ct_credentials.h"
#include "sd_storage.h"
#include "buttons.h"
#include <esp_random.h>
#include "ct_can_config.h"
#include "ct_listen_override.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

static AppConfig currentConfig;
static bool       configLoaded = false;
static portMUX_TYPE s_credentialsMux = portMUX_INITIALIZER_UNLOCKED;

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

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Initial login (single provisioning point)
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

// Wi-Fi AP key when it is separate from the web password (commercial mode).
// In personal mode it stays empty and the AP key follows the web password.
static char s_apKey[16] = "";

#if CT_AP_KEY_SEPARATE
static bool persistApKey() {
    nvs_handle_t h;
    if (nvs_open("CarTouch", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, "ap_key", s_apKey);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static void loadApKey() {
    s_apKey[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("CarTouch", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_apKey);
    if (nvs_get_str(h, "ap_key", s_apKey, &len) != ESP_OK) s_apKey[0] = '\0';
    nvs_close(h);
    s_apKey[sizeof(s_apKey) - 1] = '\0';
}
#endif

#if CT_PRODUCT_MODE
static uint32_t ctRandom32() { return esp_random(); }
#define CT_RND_FN ctRandom32
#else
#define CT_RND_FN nullptr
#endif

// The ONLY place a login is created for a device without stored settings
// (first boot, factory reset, damaged data). Personal mode: the fixed default
// login, nothing random. Commercial mode: unique login, owner must change it.
static void provisionLogin() {
    CtCredentials c;
    ctProvisionInitialCredentials(&c, CT_PRODUCT_MODE, CT_RND_FN);
    ctCopyStr(currentConfig.webUser, sizeof(currentConfig.webUser), c.user);
    ctCopyStr(currentConfig.webPass, sizeof(currentConfig.webPass), c.webPass);
    currentConfig.forcePasswordChange = c.mustChange;
#if CT_AP_KEY_SEPARATE
    ctCopyStr(s_apKey, sizeof(s_apKey), c.apKey);
    if (!persistApKey()) Serial.println("[NVS] Failed to persist Wi-Fi AP key");
#endif
#if CT_PRODUCT_MODE
    // Factory line only: the label printer / flashing tool reads this once.
    Serial.printf("[FACTORY] user=%s web=%s ap=%s\n", c.user, c.webPass, c.apKey);
#endif
}

static void applyDefaultConfigValues() {
    memset(&currentConfig, 0, sizeof(currentConfig));
    strcpy(currentConfig.wifiSSID, "");
    strcpy(currentConfig.wifiPassword, "");
    currentConfig.wifiEnabled = true;
    provisionLogin();
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

    const bool legacyConfig    = err == ESP_OK && configSize == legacyConfigSize;
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
    // In commercial mode the flag is meaningful ("factory password not yet
    // changed"), so only personal mode treats it as the old legacy marker.
    const bool legacyForce = (CT_PRODUCT_MODE == 0) && currentConfig.forcePasswordChange;
    if (legacyForce || currentConfig.webUser[0] == '\0' ||
        strlen(currentConfig.webPass) < 8) {
        provisionLogin();
        if (!saveConfig()) {
            Serial.println("[NVS] Failed to persist default login");
        }
    }
#if CT_AP_KEY_SEPARATE
    // Separate AP key: read it; a missing or damaged one (for example after
    // moving from personal firmware) is replaced by a freshly provisioned key.
    loadApKey();
    if (strlen(s_apKey) < 8) {
        CtCredentials c;
        if (ctProvisionInitialCredentials(&c, CT_PRODUCT_MODE, CT_RND_FN)) {
            ctCopyStr(s_apKey, sizeof(s_apKey), c.apKey);
            persistApKey();
        }
    }
#endif
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

void getWebCredentialsSnapshot(char* user, size_t userSize,
                               char* password, size_t passwordSize) {
    getConfig();  // load the stored login first (NVS access must stay outside the lock)
    portENTER_CRITICAL(&s_credentialsMux);
    if (user && userSize) ctCopyStr(user, userSize, currentConfig.webUser);
    if (password && passwordSize) ctCopyStr(password, passwordSize, currentConfig.webPass);
    portEXIT_CRITICAL(&s_credentialsMux);
}

bool isUsingDefaultPassword() {
    // CT_REQUIRE_PASSWORD_CHANGE = 0 (personal mode): always false, the default
    // login is allowed everywhere. Otherwise: true while the factory flag is
    // set or the password still equals the compiled-in default.
    char password[sizeof(currentConfig.webPass)];
    bool forceChange;
    getConfig();
    portENTER_CRITICAL(&s_credentialsMux);
    ctCopyStr(password, sizeof(password), currentConfig.webPass);
    forceChange = currentConfig.forcePasswordChange;
    portEXIT_CRITICAL(&s_credentialsMux);
    return ctPasswordChangePending(CT_REQUIRE_PASSWORD_CHANGE != 0,
                                   forceChange, password);
}

const char* getWifiApKey() {
    return ctEffectiveApKey(CT_AP_KEY_SEPARATE != 0, getConfig()->webPass, s_apKey);
}

void getWifiApKeySnapshot(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    getConfig();  // make sure the stored login is loaded before the lock is taken
    portENTER_CRITICAL(&s_credentialsMux);
    ctCopyStr(out, outSize, ctEffectiveApKey(CT_AP_KEY_SEPARATE != 0,
                                             currentConfig.webPass, s_apKey));
    portEXIT_CRITICAL(&s_credentialsMux);
}

bool setWifiApKey(const char* newKey) {
#if CT_AP_KEY_SEPARATE
    if (!newKey || strlen(newKey) < 8 || strlen(newKey) >= sizeof(s_apKey)) {
        Serial.println("[CONFIG] Wi-Fi AP key must be 8 to 15 characters");
        return false;
    }
    char old[sizeof(s_apKey)];
    portENTER_CRITICAL(&s_credentialsMux);
    memcpy(old, s_apKey, sizeof(old));
    ctCopyStr(s_apKey, sizeof(s_apKey), newKey);
    portEXIT_CRITICAL(&s_credentialsMux);
    if (!persistApKey()) {
        portENTER_CRITICAL(&s_credentialsMux);
        memcpy(s_apKey, old, sizeof(old));
        portEXIT_CRITICAL(&s_credentialsMux);
        return false;
    }
    return true;  // takes effect the next time the access point starts
#else
    (void)newKey;
    Serial.println("[CONFIG] Wi-Fi AP key follows the web password in this build");
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
    // Credential readers run in the async HTTP task and NimBLE callbacks.
    // Publish the complete pair atomically; never hold the spinlock across NVS IO.
    char oldUser[sizeof(cfg->webUser)], oldPass[sizeof(cfg->webPass)];
    bool oldForce;
    portENTER_CRITICAL(&s_credentialsMux);
    memcpy(oldUser, cfg->webUser, sizeof(oldUser));
    memcpy(oldPass, cfg->webPass, sizeof(oldPass));
    oldForce = cfg->forcePasswordChange;
    if (newUser && strlen(newUser) > 0) {
        strncpy(cfg->webUser, newUser, sizeof(cfg->webUser) - 1);
        cfg->webUser[sizeof(cfg->webUser) - 1] = '\0';
    }
    strncpy(cfg->webPass, newPass, sizeof(cfg->webPass) - 1);
    cfg->webPass[sizeof(cfg->webPass) - 1] = '\0';
    cfg->forcePasswordChange = false;
    portEXIT_CRITICAL(&s_credentialsMux);

    bool saved = saveConfig();
    if (!saved) {
        portENTER_CRITICAL(&s_credentialsMux);
        memcpy(cfg->webUser, oldUser, sizeof(oldUser));
        memcpy(cfg->webPass, oldPass, sizeof(oldPass));
        cfg->forcePasswordChange = oldForce;
        portEXIT_CRITICAL(&s_credentialsMux);
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
    resetStorageChoices();  // storage choices back to AUTO (SD CS pin is kept)
    if (!buttons.resetToDefaults()) {
        Serial.println("[NVS] Button defaults reset failed");
    }
    if (saveConfig()) {
        configLoaded = true;
    }
}
