/**
 * config.h - Global project configuration
 *
 * All constants, pin assignments, and user-adjustable settings live
 * here. Defaults are set below; users can change them from the TFT or
 * web settings menu, persisted via loadConfig()/saveConfig().
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include "ct_credentials.h"

// Firmware version reported by the authenticated Web status API and UI.
// Keep this as the single source of truth when a firmware release changes.
#define CAR_TOUCH_FIRMWARE_VERSION "1.0.0"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Hardware pins
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// CAN1 - ESP32-S3 TWAI with an external 3.3V-compatible transceiver.
#define PIN_CAN_TX 17  // GPIO17 - CAN Transmit (next to GPIO18 = same order as CTX/CRX on the TJA1051 module)
#define PIN_CAN_RX 18  // GPIO18 - CAN Receive

#define PIN_CAN1_CS       15  // MCP2515 CS; SPI lines are shared with TFT/Touch
#define PIN_CAN1_INT      16  // MCP2515 active-low interrupt
#define CAN1_SPEED        500000
#define CAN1_LISTEN_ONLY  true
#define MCP2515_SPI_CLOCK 10000000

// TFT display - ILI9341, 2.8" SPI, 240x320, with XPT2046 touch
#define PIN_TFT_CS   10  // GPIO10 - Chip Select
#define PIN_TFT_DC   7   // GPIO7  - Data/Command
#define PIN_TFT_RST  4   // GPIO4  - Reset
#define PIN_TFT_MOSI 11  // GPIO11 - Master Out Slave In
#define PIN_TFT_SCLK 12  // GPIO12 - Serial Clock
#define PIN_TFT_MISO 13  // GPIO13 - Master In Slave Out
#define PIN_TFT_BL   21  // GPIO21 - Backlight

#define PIN_TOUCH_CS          14    // GPIO14 - Touch Chip Select

#define PIN_LED_INTERNAL      38    // GPIO38 - ESP32-S3 DevKit onboard LED

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ CAN Bus settings
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

#define CAN_SPEED          500000  // 500 Kbps (standard OBD-II)
#define CAN_LISTEN_TIMEOUT 50      // Receive timeout, ms
#define CAN_MAX_RETRY      3       // Max retransmit attempts

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ OBD-II settings
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

// Standard PIDs (SAE J1979)
#define OBD_PID_ENGINE_RPM    0x0C  // Engine RPM
#define OBD_PID_VEHICLE_SPEED 0x0D  // Vehicle speed (km/h)
#define OBD_PID_COOLANT_TEMP  0x05  // Coolant temperature (degC)
#define OBD_PID_BATTERY_VOLT  0x42  // Control module voltage (V), SAE J1979
#define OBD_PID_THROTTLE_POS  0x11  // Throttle position (%)
#define OBD_PID_FUEL_LEVEL    0x2F  // Fuel level (%)
#define OBD_PID_RUNTIME       0x1F  // Engine runtime since start

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Display settings
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

#define TFT_WIDTH            240
#define TFT_HEIGHT           320
#define TFT_ROTATION         1  // 0-3
#define TFT_BRIGHTNESS_MAX   255
#define TFT_BRIGHTNESS_NIGHT 50
#define TFT_BRIGHTNESS_DAY   200

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ LVGL settings
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

#define LVGL_TICK_MS  5
#define LVGL_BUF_SIZE (TFT_WIDTH * 20)

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ WiFi / web server settings
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

#define WIFI_AP_NAME        "CarTouch"
#define WIFI_AP_CHANNEL     1  // 2.4 GHz channel of the access point
#define WIFI_AP_MAX_CLIENTS 4  // Simultaneous Wi-Fi clients on the access point
// The Wi-Fi access point key follows the web password in personal mode and is
// a separate stored value in commercial mode (see ct_credentials.h).
#define WIFI_MAX_RETRY  20
#define WIFI_TIMEOUT_MS 15000

#define WEB_PORT 80

// ---- Login defaults -------------------------------------------------------
// All login settings (WEB_DEFAULT_USER / WEB_DEFAULT_PASS, CT_PRODUCT_MODE,
// CT_REQUIRE_PASSWORD_CHANGE, Wi-Fi AP key) live in ct_credentials.h and can
// be overridden with -D flags in platformio.ini. Personal mode (default)
// keeps one fixed login on every device: CarTouch / 12345678.
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Power management
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

#define AUTO_SLEEP_TIMEOUT         600000  // 10 minutes of inactivity, ms
#define CAN_WAKEUP_ID              0x000   // CAN ID that wakes the device (0x000 = any)
#define DEEP_SLEEP_WAKEUP_DURATION 60      // Periodic wake interval in deep sleep, s

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Vehicle settings
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

#define MAX_DTC_COUNT           20
#define CAN_BUS_VOLTAGE_DIVIDER 2.0f

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Learn Mode settings - see README.md
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

#define MAX_CUSTOM_VEHICLES              8     // Max custom (Learned/Manual) profiles
#define MAX_LEARNED_COMMANDS_PER_VEHICLE 32    // Max commands per custom profile
#define LEARN_BASELINE_MS                2000  // Default baseline capture duration, ms
#define LEARN_ACTION_CAPTURE_MS          2000  // Default action capture duration, ms
#define LEARN_ACTION_CAPTURE_MAX_MS      5000  // Max allowed action capture duration, ms
#define BASELINE_MAX_IDS                 200   // Max distinct CAN IDs tracked during baseline (must stay <= 255: counter is uint8_t)
#define CANDIDATE_MAX                    10    // Max candidates shown after diffing

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Global data types
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

enum WindowState : uint8_t {
    WINDOW_UNKNOWN = 0,
    WINDOW_CLOSED  = 1,
    WINDOW_OPENING = 2,
    WINDOW_CLOSING = 3,
    WINDOW_OPEN    = 4
};

enum DoorLockState : uint8_t {
    LOCK_UNKNOWN  = 0,
    LOCK_LOCKED   = 1,
    LOCK_UNLOCKED = 2
};

enum AlarmState : uint8_t {
    ALARM_DISARMED  = 0,
    ALARM_ARMED     = 1,
    ALARM_TRIGGERED = 2
};

enum ThemeMode : uint8_t {
    THEME_DAY   = 0,
    THEME_NIGHT = 1,
    THEME_AUTO  = 2
};

enum DeviceMode : uint8_t {
    MODE_LISTEN_ONLY = 0,  // Listen-only - no commands are sent
    MODE_ACTIVE      = 1,  // Active - user can issue commands
    MODE_SLEEP       = 2,  // Low-power sleep
    MODE_DEEP_SLEEP  = 3   // Deep sleep - lowest power
};

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Vehicle data
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

struct VehicleData {
    // Powertrain
    uint16_t engineRPM      = 0;
    uint8_t  vehicleSpeed   = 0;     // km/h
    int8_t   coolantTemp    = -40;   // degC
    float    batteryVoltage = 0.0f;  // V
    uint8_t  throttlePos    = 0;     // %
    uint8_t  fuelLevel      = 0;     // %
    uint16_t engineRuntime  = 0;     // seconds
    uint8_t  validMask      = 0;     // CT_VD_* bits: which values the ECU really answered (see ct_obd_validity.h)

    // Doors
    DoorLockState doorFL     = LOCK_UNKNOWN;
    DoorLockState doorFR     = LOCK_UNKNOWN;
    DoorLockState doorRL     = LOCK_UNKNOWN;
    DoorLockState doorRR     = LOCK_UNKNOWN;
    DoorLockState trunkState = LOCK_UNKNOWN;

    // Windows
    WindowState windowFL     = WINDOW_UNKNOWN;
    WindowState windowFR     = WINDOW_UNKNOWN;
    WindowState windowRL     = WINDOW_UNKNOWN;
    WindowState windowRR     = WINDOW_UNKNOWN;
    WindowState sunroofState = WINDOW_UNKNOWN;

    // Other
    AlarmState alarmState   = ALARM_DISARMED;
    bool       mirrorFolded = false;
    uint8_t    errorCount   = 0;
};

// Persisted settings (NVS)
struct AppConfig {
    // WiFi
    char wifiSSID[32]     = "";
    char wifiPassword[64] = "";
    bool wifiEnabled      = true;

    // Web
    char webUser[16]         = WEB_DEFAULT_USER;
    char webPass[16]         = WEB_DEFAULT_PASS;
    bool forcePasswordChange = false;  // Legacy flag from old firmware (random temporary password); never set now

    // Vehicle
    char     vehicleBrand[32] = "Generic";
    char     vehicleModel[32] = "OBD-II";
    uint16_t vehicleYear      = 2020;

    // Display
    ThemeMode theme           = THEME_AUTO;
    uint8_t   brightnessDay   = TFT_BRIGHTNESS_DAY;
    uint8_t   brightnessNight = TFT_BRIGHTNESS_NIGHT;

    // CAN
    uint8_t  canTxPin       = PIN_CAN_TX;
    uint8_t  canRxPin       = PIN_CAN_RX;
    uint32_t canSpeed       = CAN_SPEED;
    bool     listenOnlyMode = true;  // Safe default: listen-only, nothing transmitted on the bus

    // Power
    uint32_t sleepTimeout = AUTO_SLEEP_TIMEOUT;

    // Touch calibration - output of TFT_eSPI's calibrateTouch(): 5
    // uint16_t values mapping raw ADC coordinates to screen pixels.
    uint16_t touchCalData[5] = {0, 0, 0, 0, 0};
    bool     touchCalibrated = false;  // false = not yet calibrated; first boot should run the wizard

    uint32_t configMagic = 0xCAFE1234;    // Validity marker

    // Appended to preserve the layout of the prior NVS blob for migration.
    uint8_t  can1CsPin      = PIN_CAN1_CS;
    uint8_t  can1IntPin     = PIN_CAN1_INT;
    uint32_t can1Speed      = CAN1_SPEED;
    bool     can1ListenOnly = CAN1_LISTEN_ONLY;

    // Explicit OBD route: 0 = CAN1/TWAI, 1 = CAN2/MCP2515.
    // Appended so earlier NVS layouts remain migratable.
    uint8_t obdCanBus   = 0;
    uint8_t learnCanBus = 0;

    // Explicit vehicle-command route: 0 = CAN1/TWAI, 1 = CAN2/MCP2515.
    // Stored in the former trailing padding, so sizeof(AppConfig) and the
    // NVS blob size do not change; old blobs read 0 (= previous behaviour).
    uint8_t  vehicleCanBus   = 0;
};

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Configuration management
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

/** Loads settings from NVS; falls back to defaults if invalid/absent. */
bool loadConfig();

/** Saves the current settings to NVS. */
bool saveConfig();

/** Returns a pointer to the current settings, loading them if needed. */
AppConfig* getConfig();

/** Resets settings to factory defaults. */
void setDefaultConfig();

/**
 * Touch-wizard skip flag (separate NVS key, does not change the AppConfig
 * blob layout). Set when the touch panel never answered the calibration
 * wizard, so later boots do not wait for it again. Cleared when a
 * calibration succeeds or the user starts "Recalibrate Touch" again.
 */
bool isTouchCalibrationSkipped();
void setTouchCalibrationSkipped(bool skipped);

bool isValidCanPin(uint8_t pin);
bool validateCanPins(uint8_t txPin, uint8_t rxPin);
bool validateCan1Pins(uint8_t csPin, uint8_t intPin);
bool validateCanPinAssignment(uint8_t txPin, uint8_t rxPin,
                              uint8_t csPin, uint8_t intPin);
bool isValidCanSpeed(uint32_t speed);
bool isValidCan1Speed(uint32_t speed);

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Password / session helpers
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

/**
 * Learn Mode forces Listen-Only temporarily. While `forced` is true, saveConfig()
 * writes `userChoice` for that bus instead of the temporary RAM value.
 * busIndex: 0 = CAN1/TWAI listenOnlyMode, 1 = CAN2/MCP2515 can1ListenOnly.
 */
void configSetLearnListenOverride(uint8_t busIndex, bool forced, bool userChoice);

/** True if the device is still using the default/temporary web password. */
bool isUsingDefaultPassword();
// Copies credential fields atomically for use by async Web/BLE tasks.
void getWebCredentialsSnapshot(char* user, size_t userSize, char* password, size_t passwordSize);

/**
 * Key for the Wi-Fi access point (8 to 15 characters, never empty).
 * Personal mode: the web password. Commercial mode (CT_AP_KEY_SEPARATE):
 * its own stored value.
 */
const char* getWifiApKey();

// Copies the Wi-Fi AP key into `out` under the credentials lock (the pointer form
// above can alias the web password, which another task may change).
void getWifiApKeySnapshot(char* out, size_t outSize);

/**
 * Sets the separate AP key (only when CT_AP_KEY_SEPARATE=1; otherwise returns
 * false). Takes effect the next time the access point starts.
 */
bool setWifiApKey(const char* newKey);

/**
 * Sets a new web password (min 8 characters, must differ from the
 * default). Returns true on success.
 */
bool setWebPassword(const char* newUser, const char* newPass);

/**
 * Callback type invoked whenever setWebPassword() succeeds, regardless
 * of which interface (TFT or web) called it. Lets any module that keeps
 * its own session state (currently WebServerManager) invalidate stale
 * sessions immediately rather than only after a full reboot.
 *
 * Only one callback is supported at a time - sufficient for the current
 * architecture, where WebServerManager is the only session holder.
 */
typedef void (*PasswordChangeCallback)();

/** Registers the callback described above. */
void registerPasswordChangeCallback(PasswordChangeCallback cb);

#endif    // CONFIG_H
