/**
 * main.cpp - CarTouch entry point
 *
 * Wires together the core modules (CAN, OBD-II, vehicle control, TFT UI,
 * web server, WiFi) plus the Learn Mode stack (CustomVehicleStore,
 * LearnEngine, ActiveProfileManager). VehicleControl resolves commands
 * through ActiveProfileManager rather than taking a raw CAN ID.
 */

#include <Arduino.h>
#include <SPIFFS.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "sd_storage.h"
#include "buttons.h"

#include "config.h"
#include "ct_can_config.h"
#include "can_manager.h"
#include "can_service.h"
#include "can_recorder.h"
#include "ct_can_record.h"
#include "mcp2515_can_interface.h"
#include "obd2_reader.h"
#include "vehicle_control.h"
#include "vehicle_db.h"
#include "tft_ui.h"
#include "webserver.h"
#include "wifi_manager.h"
#include "ble_manager.h"
#include "module_status.h"

#include "custom_vehicle_store.h"
#include "learn_engine.h"
#include "active_profile_manager.h"
#include "error_log.h"
#include "ct_sync_policy.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Global objects
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

CANManager can1Interface(PIN_CAN_TX, PIN_CAN_RX, CAN_SPEED);
Mcp2515CanInterface can2Interface;
CANService canManager(can1Interface, can2Interface);
CanRecorder canRecorder(canManager);
OBD2Reader obd2Reader(canManager);

// Vehicle database (DBC). Allocated on the heap in setup() rather than as
// a static/global object because the parser owns dynamic signal vectors and
// the message table can represent large real-world DBC files. With PSRAM
// enabled, the Arduino-ESP32 allocator can place these larger allocations
// outside the limited internal DRAM pool.
// ActiveProfileManager and VehicleControl are allocated the same way
// since they hold references to the objects before them.
VehicleDB* vehicleDB = nullptr;

CustomVehicleStore customVehicleStore;
LearnEngine learnEngine(canManager);
ActiveProfileManager* activeProfileManager = nullptr;

VehicleControl* vehicleControl = nullptr;    // Resolves commands via activeProfileManager

TFT_UI tftUI;
WebServerManager webServer;
WiFiManager wifiManager;
ModuleStatusManager moduleStatusManager;
bool filesystemReady = false;
bool customVehicleStoreReady = false;

static void broadcastCanRecorderStatus() {
    webServer.broadcastCanRecordingStatus(canRecorder.isRecording(),
                                          canRecorder.getBusMask(),
                                          canRecorder.getFrameCount(),
                                          canRecorder.getDroppedFrameCount(),
                                          canRecorder.getFileName(),
                                          canRecorder.getLastError());
    char bleStatus[112];
    snprintf(bleStatus, sizeof(bleStatus), "RECORD:%u:%u:%lu:%lu",
             canRecorder.isRecording() ? 1u : 0u,
             canRecorder.getBusMask(),
             (unsigned long)canRecorder.getFrameCount(),
             (unsigned long)canRecorder.getDroppedFrameCount());
    bleManager.publishStatus(bleStatus);
}

static void broadcastObdDiagnosticStatus() {
    uint16_t dtcs[MAX_DTC_COUNT] = {};
    const uint8_t count = obd2Reader.getDtcCount();
    for (uint8_t i = 0; i < count; ++i) dtcs[i] = obd2Reader.getDtc(i);
    webServer.broadcastObdDiagnosticStatus(
        (uint8_t)obd2Reader.getDiagnosticState(),
        (uint8_t)obd2Reader.getDiagnosticOperation(),
        obd2Reader.getDiagnosticError(),
        obd2Reader.getDiagnosticResponseCode(), dtcs, count);

    static bool previousValid = false;
    static uint8_t previousState = 0;
    static uint8_t previousOperation = 0;
    static uint8_t previousError = 0;
    static uint8_t previousResponseCode = 0;
    static uint8_t previousCount = 0;
    static uint16_t previousDtcs[MAX_DTC_COUNT] = {};
    bool changed = !previousValid || previousState != (uint8_t)obd2Reader.getDiagnosticState() ||
        previousOperation != (uint8_t)obd2Reader.getDiagnosticOperation() ||
        previousError != obd2Reader.getDiagnosticError() ||
        previousResponseCode != obd2Reader.getDiagnosticResponseCode() ||
        previousCount != count;
    for (uint8_t i = 0; i < count && !changed; ++i) changed = previousDtcs[i] != dtcs[i];
    if (changed) {
        String bleStatus = "DTC:" + String((uint8_t)obd2Reader.getDiagnosticState()) + ":" +
            String((uint8_t)obd2Reader.getDiagnosticOperation()) + ":" +
            String(obd2Reader.getDiagnosticError()) + ":" +
            String(obd2Reader.getDiagnosticResponseCode()) + ":" + String(count);
        for (uint8_t i = 0; i < count; ++i) {
            char code[6];
            snprintf(code, sizeof(code), ":%04X", (unsigned)dtcs[i]);
            bleStatus += code;
            previousDtcs[i] = dtcs[i];
        }
        bleManager.publishStatus(bleStatus.c_str());
        previousState = (uint8_t)obd2Reader.getDiagnosticState();
        previousOperation = (uint8_t)obd2Reader.getDiagnosticOperation();
        previousError = obd2Reader.getDiagnosticError();
        previousResponseCode = obd2Reader.getDiagnosticResponseCode();
        previousCount = count;
        previousValid = true;
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Global state
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

VehicleData currentVehicleData;
uint32_t lastDataUpdateTime = 0;
uint32_t lastActivityTime   = 0;
uint32_t lastWakeTime       = 0;
static constexpr uint32_t WAKE_OBD_TX_HOLD_MS = 1000;
uint32_t    obdReadInterval    = 200;            // OBD poll interval, ms
DeviceMode  currentMode        = MODE_ACTIVE;

// Task watchdog timeout. If any stage of loop() stalls longer than this
// (e.g. a still-blocking OBD2Reader call, an unexpected infinite loop),
// the chip resets itself rather than hanging indefinitely in the vehicle.
#define WDT_TIMEOUT_S 8

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Forward declarations
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

void setup();
void loop();
void handleCommand(const char* command);      // thread-safe: enqueue only
void processCommand(const char* command);     // runs in loop() task
void drainCommandQueue();
void processSerialConsole();
void processSerialCommand(const char* command);
void handleControlCommand(const char* command);
void checkAutoSleep();
void wakeFromSleep();
void refreshModuleStatuses();

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ setup()
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n========================================");
    Serial.println(" CarTouch - ESP32-S3 Car Control");
    Serial.println(" (+ Learn Mode / Custom Vehicle Database)");
    Serial.println("========================================\n");

    const uint32_t flashBytes = ESP.getFlashChipSize();
    const bool psramAvailable = ESP.getPsramSize() > 0;
    const bool partitionLayoutFits = ctPartitionFitsFlash(flashBytes, CT_REQUIRED_FLASH_BYTES);
    Serial.printf("[INIT] Flash: %u bytes | PSRAM: %u bytes | heap: %u bytes\n",
                  (unsigned)flashBytes, (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());
    if (!partitionLayoutFits) {
        Serial.printf("[INIT] Configured partition layout needs %u bytes of flash; filesystem will remain disabled.\n",
                      (unsigned)CT_REQUIRED_FLASH_BYTES);
    }
    if (!psramAvailable) {
        Serial.println("[INIT] PSRAM unavailable; runtime memory is limited to internal SRAM.");
    }

    // Keep the large core objects resident in static storage so they remain
    // usable even on ESP32-S3 boards without PSRAM. A small board should not
    // hard-fail just because a 16MB+PSRAM layout is not available.
    static VehicleDB vehicleDBStorage;
    static ActiveProfileManager activeProfileManagerStorage(vehicleDBStorage, customVehicleStore);
    static VehicleControl vehicleControlStorage(canManager, activeProfileManagerStorage);
    vehicleDB = &vehicleDBStorage;
    activeProfileManager = &activeProfileManagerStorage;
    vehicleControl = &vehicleControlStorage;
    Serial.printf("[INIT] Configured partition layout fits detected flash: %s\n",
                  partitionLayoutFits ? "yes" : "no");

    // Watchdog - set up as early as possible so it covers the rest of
    // setup() too. Struct-based esp_task_wdt_config_t only exists on
    // Arduino-ESP32 3.x (ESP-IDF 5.x); this branch keeps the code
    // compiling on both 2.x and 3.x cores.
    {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
        esp_task_wdt_config_t wdtConfig = {
            .timeout_ms     = WDT_TIMEOUT_S * 1000,
            .idle_core_mask = 0,
            .trigger_panic  = true
        };
        esp_err_t wdtErr = esp_task_wdt_init(&wdtConfig);
#else
        esp_err_t wdtErr = esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
        // The core may already have initialised the TWDT (with its own
        // timeout); in that case the call fails and the core's timeout stays.
        if (wdtErr != ESP_OK) {
            Serial.printf("[INIT] Watchdog init returned %d - core default timeout stays\n", (int)wdtErr);
        }
        // NOTE: the loop task is subscribed at the END of setup(), not here.
        // setup() contains long blocking steps (DBC loading,
        // touch calibration) that would otherwise trip the watchdog and cause
        // a reboot loop.
    }

    // 1. Configuration
    Serial.println("[INIT] Loading configuration...");
    loadConfig();

    // 1b. Error log / telemetry (checklist item 16) - started right
    // after config so every subsequent init step can log through it.
    getErrorLog()->begin();
    moduleStatusManager.begin();
    learnEngine.setCanBus(getConfig()->learnCanBus == 1 ? CAN_BUS_2 : CAN_BUS_1);

    // 2. SPIFFS (web assets, DBC files, custom profiles)
    Serial.println("[INIT] Starting SPIFFS...");
    if (!partitionLayoutFits) {
        Serial.println("[INIT] SPIFFS disabled because its configured partition exceeds detected flash; data was not formatted");
        getErrorLog()->log(LOG_CAT_SYSTEM, LOG_ERROR,
                           "Configured SPIFFS partition does not fit detected flash; filesystem disabled without formatting");
        moduleStatusManager.setState(MODULE_STORAGE, MODULE_ERROR);
    } else if (!SPIFFS.begin(false)) {
        Serial.println("[INIT] SPIFFS mount failed - preserving data; filesystem features disabled");
        getErrorLog()->log(LOG_CAT_SYSTEM, LOG_ERROR,
                           "SPIFFS mount failed; filesystem features disabled without formatting");
        moduleStatusManager.setState(MODULE_STORAGE, MODULE_ERROR);
    } else {
        filesystemReady = true;
        Serial.println("[INIT] SPIFFS ready");
        moduleStatusManager.setState(MODULE_STORAGE, MODULE_READY);
    }
    canRecorder.setStorageAvailable(filesystemReady);

    // 3. CAN Bus
    Serial.println("[INIT] Starting CAN Bus...");
    moduleStatusManager.setState(MODULE_CAN, MODULE_INITIALIZING);
    moduleStatusManager.setState(MODULE_CAN1, MODULE_INITIALIZING);
    moduleStatusManager.setState(MODULE_CAN2, MODULE_INITIALIZING);
    const bool anyCanReady = canManager.begin();
    if (!anyCanReady) {
        getErrorLog()->log(LOG_CAT_CAN, LOG_ERROR, "CAN Bus failed to start at boot");
        moduleStatusManager.setState(MODULE_CAN, MODULE_ERROR);
    } else {
        canManager.flushRxQueue();
        canManager.subscribeRx(CAN_BUS_1, CAN_RX_WAKE);
        moduleStatusManager.setState(MODULE_CAN, MODULE_UNVERIFIED);
    }
    if (canManager.isActive(CAN_BUS_1)) {
        Serial.println("[CAN1] TWAI ready (TJA1051 transceiver path)");
        moduleStatusManager.setState(MODULE_CAN1, MODULE_UNVERIFIED);
    } else {
        Serial.println("[CAN1] TWAI unavailable; CAN2 remains independent");
        getErrorLog()->log(LOG_CAT_CAN, LOG_WARN, "CAN1 TWAI failed to initialize");
        moduleStatusManager.setState(MODULE_CAN1, MODULE_ERROR);
    }
    if (canManager.isActive(CAN_BUS_2)) {
        const AppConfig* config = getConfig();
        Serial.printf("[CAN2] MCP2515 ready (8 MHz oscillator, %lu bps, %s)\n",
                      (unsigned long)config->can1Speed,
                      config->can1ListenOnly ? "listen-only" : "normal");
        moduleStatusManager.setState(MODULE_CAN2, MODULE_UNVERIFIED);
    } else {
        Serial.println("[CAN2] MCP2515 unavailable; CAN1 remains independent");
        getErrorLog()->log(LOG_CAT_CAN, LOG_WARN, "CAN2 MCP2515 failed to initialize");
        moduleStatusManager.setState(MODULE_CAN2, MODULE_ERROR);
    }

    // 3b. Initialise shared synchronisation for SD and Buttons
    if (!ctSync().initMutex()) {
        Serial.println("[INIT] Failed to initialise SD/Buttons sync mutex; some features may be unsafe");
        getErrorLog()->log(LOG_CAT_SYSTEM, LOG_ERROR, "SD/Buttons sync mutex failed to initialise");
    }

    // 3c. Optional SD card (after CAN: SPI bus is already initialised).
    // Missing card or unset CS pin is normal and never blocks boot.
    sdStorage.begin();
    Serial.printf("[INIT] SD: %s\n", sdStorage.stateText());

    // 3d. Optional physical keys (disabled until configured)
    buttons.begin();

    // 4. OBD-II reader
    moduleStatusManager.setState(MODULE_OBD, MODULE_INITIALIZING);
    obd2Reader.setCanBus(getConfig()->obdCanBus == 1 ? CAN_BUS_2 : CAN_BUS_1);
    obd2Reader.begin();
    moduleStatusManager.setState(MODULE_OBD,
        !canManager.isActive(obd2Reader.getCanBus()) ? MODULE_ERROR :
        (!obd2Reader.canTransmit() ? MODULE_DISABLED : MODULE_UNVERIFIED));

    // 5. Vehicle DB (built-in DBC files)
    vehicleDB->begin();

    // 6. Custom vehicle store (must come after SPIFFS.begin)
    Serial.println("[INIT] Starting CustomVehicleStore...");
    if (filesystemReady) {
        customVehicleStoreReady = customVehicleStore.begin();
        if (!customVehicleStoreReady) {
            getErrorLog()->log(LOG_CAT_SYSTEM, LOG_ERROR, "Custom profile storage failed to initialize");
            moduleStatusManager.setState(MODULE_STORAGE, MODULE_ERROR);
        }
    }

    // 7. Vehicle control
    vehicleControl->begin();

    // No vehicle is auto-selected at boot - ActiveProfileManager starts
    // with ACTIVE_KIND_NONE. The user picks one from the TFT or web UI;
    // until then, resolveCommand() reports "no vehicle selected" instead
    // of sending anything.

    // 8. TFT + LVGL. Learn Mode modules must be attached before begin() -
    // otherwise the Learn tab's internal pointers stay null.
    tftUI.attachLearnModules(&learnEngine, &customVehicleStore,
                              activeProfileManager, vehicleControl);
#ifdef CARTOUCH_HEADLESS
    moduleStatusManager.setState(MODULE_DISPLAY, MODULE_DISABLED);
    moduleStatusManager.setState(MODULE_TOUCH, MODULE_DISABLED);
    Serial.println("[INIT] Headless build: display and touch are disabled");
#else
    moduleStatusManager.setState(MODULE_DISPLAY, MODULE_INITIALIZING);
    tftUI.begin();
    moduleStatusManager.setState(MODULE_DISPLAY,
        tftUI.isDisplayAvailable() ? MODULE_UNVERIFIED : MODULE_ERROR);
    tftUI.setControlCallback(handleCommand);
    // Update the visual CAN state only after the TFT/LVGL objects exist.
    // The driver being initialized does not prove that a transceiver is
    // physically wired to a live bus; the status indicator is therefore
    // deliberately based on the controller's current state here and is
    // refined by runtime diagnostics in the UI.
    tftUI.setCANStatus(canManager.isActive(CAN_BUS_1) || canManager.isActive(CAN_BUS_2));
    moduleStatusManager.setState(MODULE_TOUCH,
        tftUI.isTouchAvailable() ? MODULE_READY : MODULE_NOT_PRESENT);
    if (!tftUI.isDisplayAvailable()) {
        getErrorLog()->log(LOG_CAT_SYSTEM, LOG_WARN, "TFT/LVGL initialization failed; other services remain active");
    } else if (!canManager.isActive(CAN_BUS_1)) {
        tftUI.showNotification("CAN Bus error!");
    } else {
        tftUI.showNotification("CarTouch ready");
    }
#endif

    // 9. WiFi (AP mode by default)
    moduleStatusManager.setState(MODULE_WIFI, MODULE_INITIALIZING);
    // AP always; if a router was saved, join it as well (AP stays up).
    wifiManager.begin(getConfig()->wifiSSID[0] ? 2 : 1);
    tftUI.setWiFiStatus(wifiManager.isConnected());
    moduleStatusManager.setState(MODULE_WIFI,
        wifiManager.isEnabled() && wifiManager.isConnected() ? MODULE_READY :
        (wifiManager.isEnabled() ? MODULE_ERROR : MODULE_DISABLED));

    // 10. BLE - starts independently of Wi-Fi so local BLE access remains available.
    moduleStatusManager.setState(MODULE_BLE, MODULE_INITIALIZING);
    bleManager.setCommandCallback(handleCommand);
    if (!bleManager.begin()) {
        Serial.println("[BLE] Failed to start BLE");
        moduleStatusManager.setState(MODULE_BLE, MODULE_ERROR);
    } else {
        moduleStatusManager.setState(MODULE_BLE, MODULE_READY);
    }

    // 11. Web server - attach Learn Mode modules before begin()
    webServer.attachLearnModules(&learnEngine, &customVehicleStore,
                                  activeProfileManager, vehicleControl);
    webServer.attachCanService(&canManager);
    webServer.setModuleStatusManager(&moduleStatusManager);

    // 12. Start web server
    moduleStatusManager.setState(MODULE_WEB, MODULE_INITIALIZING);
    webServer.begin();
    moduleStatusManager.setState(MODULE_WEB, webServer.isStarted() ? MODULE_READY : MODULE_ERROR);
    webServer.broadcastModuleStatus();
    webServer.setCommandCallback(handleCommand);

    lastActivityTime = millis();

    // Setup is complete: from here on loop() feeds the watchdog.
    esp_task_wdt_add(NULL);
    esp_task_wdt_reset();
    Serial.printf("[INIT] Watchdog enabled (timeout: %ds)\n", WDT_TIMEOUT_S);

    Serial.println("\n[INIT] CarTouch ready");
    Serial.printf("[INIT] IP: %s\n", wifiManager.getIP().toString().c_str());
    Serial.printf("[INIT] CAN: %s\n", canManager.isActive() ? "OK" : "FAILED");
    Serial.printf("[INIT] Custom profiles found: %d\n", customVehicleStore.getProfileCount());
    Serial.println("[SERIAL] Type 'help' for diagnostics and guarded control commands");

    if (isUsingDefaultPassword()) {
        Serial.println("[SECURITY] Web password is still the default! Change it from Settings.");
        tftUI.showNotification("Please change the default password!");
    }
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ loop()
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// Last Bus-Off state of CAN1/CAN2, refreshed once per second in loop() and
// read by refreshModuleStatuses() so the status code adds no extra bus access.
static bool g_canBusOff[2] = {false, false};
// A channel counts as "connected" only if a frame arrived within this time.
static const uint32_t CAN_LINK_TRAFFIC_TIMEOUT_MS = 5000;

void loop() {
    // Feed the watchdog every iteration.
    esp_task_wdt_reset();

    // One task owns hardware RX and fans each frame out to independent,
    // bounded consumer queues (OBD, Learn, monitor, and wake detection).
    canManager.pumpRx();
    canRecorder.update();
    sdStorage.update();
    buttons.update();
    {
        CtKeyEvent keyEv;
        while (buttons.poll(keyEv)) {
            // Short press = navigate; long press only has a meaning for OK (back).
            if (keyEv.type == CT_EV_SHORT || (keyEv.type == CT_EV_LONG && keyEv.key == CT_KEY_OK))
                tftUI.pushKey(keyEv.key, keyEv.type == CT_EV_LONG);
        }
    }
    bool canWakeActivity = false;
    CanRxFrame wakeFrame;
    while (canManager.receiveRx(CAN_BUS_1, CAN_RX_WAKE, wakeFrame)) {
        canWakeActivity = true;
    }

    // Flush error-log counters to NVS at most every 5 minutes (see
    // error_log.h) - cheap to call every loop() since it no-ops unless
    // both the dirty flag and the interval have elapsed.
    getErrorLog()->maybeSaveCounters();

    // 1. LVGL
    tftUI.update();

    // 2. WebSocket
    webServer.update();
    wifiManager.update();

    static uint32_t lastCanRecordingStatusBroadcast = 0;
    if ((uint32_t)(millis() - lastCanRecordingStatusBroadcast) >= 1000) {
        broadcastCanRecorderStatus();
        lastCanRecordingStatusBroadcast = millis();
    }

    static uint32_t lastObdDiagnosticBroadcast = 0;
    if ((uint32_t)(millis() - lastObdDiagnosticBroadcast) >= 500) {
        broadcastObdDiagnosticStatus();
        lastObdDiagnosticBroadcast = millis();
    }

    static uint32_t lastCanDiagnosticsBroadcast = 0;
    if ((uint32_t)(millis() - lastCanDiagnosticsBroadcast) >= 1000) {
        // Bus-Off is checked and recovered on BOTH channels. Before, CAN1
        // (TWAI) was only recovered after a failed transmit, so a Bus-Off
        // that happened without further TX was never cleared.
        static bool     busOffSeen[2]     = {false, false};
        static uint32_t lastRecovery[2]   = {0, 0};
        static const char* const busName[2] = {"CAN1", "CAN2"};
        for (uint8_t b = 0; b < 2; ++b) {
            const CanBusId bus = (b == 0) ? CAN_BUS_1 : CAN_BUS_2;
            CanDiagnostics diagnostics = {};
            canManager.getDiagnostics(bus, diagnostics);
            g_canBusOff[b] = diagnostics.busOff;
            if (diagnostics.busOff) {
                const uint32_t now = millis();
                if (ctRecoveryDue(busOffSeen[b], lastRecovery[b], now, 5000)) {
                    getErrorLog()->log(LOG_CAT_CAN, LOG_ERROR,
                                       "%s bus-off detected; attempting recovery", busName[b]);
                    if (!canManager.recoverFromBusOff(bus)) {
                        getErrorLog()->log(LOG_CAT_CAN, LOG_WARN,
                                           "%s recovery failed; retrying in 5 seconds", busName[b]);
                    }
                    busOffSeen[b]   = true;
                    lastRecovery[b] = now;
                    diagnostics = {};
                    canManager.getDiagnostics(bus, diagnostics);
                    g_canBusOff[b] = diagnostics.busOff;
                }
            } else {
                busOffSeen[b] = false;
            }
            webServer.broadcastCanDiagnostics(diagnostics, busName[b]);
        }
        lastCanDiagnosticsBroadcast = millis();
    }

    // 2b. BLE / BLE OTA
    bleManager.update();

    // 2b'. Keep the module status (Wi-Fi/Web/CAN/OBD/Touch) live on TFT and Web.
    refreshModuleStatuses();

    // 2c. Run queued commands here so all CAN/UI/state access stays in this task
    processSerialConsole();
    drainCommandQueue();

    // 3. Learn engine (non-blocking; must run every iteration for correct
    // baseline/action capture timing).
    learnEngine.update();

    // 4. OBD-II polling (fully non-blocking). No requests are sent while
    // Listen-Only is active, since reading OBD data requires transmitting
    // a request. obd2Reader.update() advances one small step per call;
    // getLatestData() picks up the result once a full round completes.
    if (obd2Reader.isDiagnosticBusy()) {
        obd2Reader.update();
    } else if (currentMode == MODE_ACTIVE && obd2Reader.canTransmit() &&
               (millis() - lastWakeTime >= WAKE_OBD_TX_HOLD_MS)) {
        obd2Reader.update();

        if (millis() - lastDataUpdateTime > obdReadInterval) {
            if (obd2Reader.getLatestData(currentVehicleData)) {
                // Battery/control-module voltage comes from OBD-II PID 0x42
                // when the ECU supports it. Zero means unavailable; never
                // display a fabricated voltage value.
                tftUI.updateVehicleData(currentVehicleData);
                webServer.broadcastVehicleData(currentVehicleData);
            }
            lastDataUpdateTime = millis();
        }
    }

#ifndef CARTOUCH_HEADLESS
    // 4b. Touch activity: without this the device went to sleep (screen off,
    // Wi-Fi off) 10 minutes after the last *command* even while the user was
    // actively using the TFT. lv_disp_get_inactive_time() is LVGL's time
    // since the last pointer/touch event.
    if (lv_disp_get_inactive_time(NULL) < 1000) {
        if (currentMode == MODE_SLEEP) {
            wakeFromSleep();
        }
        lastActivityTime = millis();
    }
#endif

    // 4c. Web / BLE activity: a logged-in web request, a web command, or a BLE
    // connection/command counts as use, wakes the device and delays auto-sleep.
    {
        const bool webAct = webServer.consumeActivity();
        const bool bleAct = bleManager.consumeActivity();
        if (webAct || bleAct) {
            if (currentMode == MODE_SLEEP) wakeFromSleep();
            lastActivityTime = millis();
        }
    }

    // 5. Auto-sleep check. Deferred while a Learn Mode capture is in
    // progress so the session isn't interrupted.
    if (learnEngine.isActiveCaptureState()) {
        // Only real capture windows count as continuous activity. Terminal,
        // error and candidate-review states must not disable auto-sleep forever.
        lastActivityTime = millis();
    } else if (canRecorder.isRecording()) {
        lastActivityTime = millis();
    } else {
        checkAutoSleep();
    }

    // 6. Wake on CAN activity while asleep
    if ((currentMode == MODE_SLEEP || currentMode == MODE_DEEP_SLEEP) &&
        canWakeActivity) {
        wakeFromSleep();
    }

    delay(5);    // Yield briefly
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
void processSerialConsole() {
    static char line[64];
    static size_t length = 0;
    static bool overflow = false;

    while (Serial.available() > 0) {
        const int value = Serial.read();
        if (value < 0) break;
        const char ch = (char)value;
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (!overflow && length > 0) {
                line[length] = '\0';
                processSerialCommand(line);
            } else if (overflow) {
                Serial.println("[SERIAL] Input too long; line discarded");
            }
            length = 0;
            overflow = false;
            continue;
        }
        if (overflow) continue;
        if (length + 1 >= sizeof(line)) {
            overflow = true;
            continue;
        }
        line[length++] = ch;
    }
}

static const char* learnStateName(LearnModeState state) {
    switch (state) {
        case LEARN_IDLE: return "IDLE";
        case LEARN_BASELINE_CAPTURE: return "BASELINE_CAPTURE";
        case LEARN_WAITING_ACTION: return "WAITING_ACTION";
        case LEARN_ACTION_CAPTURE: return "ACTION_CAPTURE";
        case LEARN_CANDIDATES_READY: return "CANDIDATES_READY";
        case LEARN_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

void processSerialCommand(const char* command) {
    if (strcmp(command, "help") == 0) {
        Serial.println("Commands: help | status | memory | config | can | obd | dtc read | dtc clear | dtc status | learn | record status | record start <1|2|both> | record stop | record delete <canNNNN.csv> | storage | sd status | sd cs <gpio|-1> | sd store <cat> <auto|internal|sd> | sd reset | btn status | btn off | btn gpio u d l r ok | btn adc pin u d l r ok | errors | control <command>");
        Serial.println("Control commands use the same selected-profile, verification, Listen-Only, and rate-limit guards as Web/TFT.");
        return;
    }

    if (strcmp(command, "record status") == 0) {
        processCommand("record_status");
        return;
    }

    if (strncmp(command, "record start ", 13) == 0) {
        const char* bus = command + 13;
        const char* mask = strcmp(bus, "1") == 0 ? "1" :
                           strcmp(bus, "2") == 0 ? "2" :
                           strcmp(bus, "both") == 0 ? "3" : nullptr;
        if (!mask) {
            Serial.println("Usage: record start <1|2|both>");
            return;
        }
        char queuedCommand[32];
        snprintf(queuedCommand, sizeof(queuedCommand), "record_start:%s", mask);
        processCommand(queuedCommand);
        return;
    }

    if (strcmp(command, "record stop") == 0) {
        processCommand("record_stop");
        return;
    }

    if (strncmp(command, "record delete ", 14) == 0) {
        char queuedCommand[32];
        snprintf(queuedCommand, sizeof(queuedCommand), "record_delete:%s", command + 14);
        processCommand(queuedCommand);
        return;
    }

    if (strcmp(command, "dtc read") == 0 || strcmp(command, "dtc clear") == 0 ||
        strcmp(command, "dtc status") == 0) {
        processCommand(strcmp(command, "dtc read") == 0 ? "dtc_read" :
                       strcmp(command, "dtc clear") == 0 ? "dtc_clear" : "dtc_status");
        return;
    }

    if (strcmp(command, "status") == 0) {
        Serial.printf("Firmware=%s Flash=%u PSRAM=%u Heap=%u WiFi=%s Web=%s BLE=%s Storage=%s\n",
                      CAR_TOUCH_FIRMWARE_VERSION,
                      (unsigned)ESP.getFlashChipSize(),
                      (unsigned)ESP.getPsramSize(),
                      (unsigned)ESP.getFreeHeap(),
                      wifiManager.isConnected() ? "available" : "unavailable",
                      webServer.isStarted() ? "started" : "stopped",
                      bleManager.isEnabled() ? "enabled" : "unavailable",
                      filesystemReady ? "SPIFFS-mounted" : "unavailable");
        return;
    }

    if (strcmp(command, "memory") == 0) {
        Serial.printf("Internal heap: free=%u minFreeSinceBoot=%u largestBlock=%u bytes\n",
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getMinFreeHeap(),
                      (unsigned)ESP.getMaxAllocHeap());
        Serial.printf("PSRAM: total=%u free=%u minFreeSinceBoot=%u largestBlock=%u bytes\n",
                      (unsigned)ESP.getPsramSize(),
                      (unsigned)ESP.getFreePsram(),
                      (unsigned)ESP.getMinFreePsram(),
                      (unsigned)ESP.getMaxAllocPsram());
        Serial.printf("Loop task stack high-water minimum free=%u bytes\n",
                      (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        return;
    }

    if (strcmp(command, "config") == 0) {
        const AppConfig* cfg = getConfig();
        Serial.printf("CAN1 TX=%u RX=%u bitrate=%lu listenOnly=%s\n",
                      cfg->canTxPin, cfg->canRxPin, (unsigned long)cfg->canSpeed,
                      cfg->listenOnlyMode ? "yes" : "no");
        Serial.printf("CAN2 CS=%u INT=%u bitrate=%lu listenOnly=%s\n",
                      cfg->can1CsPin, cfg->can1IntPin, (unsigned long)cfg->can1Speed,
                      cfg->can1ListenOnly ? "yes" : "no");
        Serial.printf("OBD channel=CAN%u Learn channel=CAN%u Vehicle channel=CAN%u\n",
                      (unsigned)(cfg->obdCanBus + 1u),
                      (unsigned)(cfg->learnCanBus + 1u),
                      (unsigned)(cfg->vehicleCanBus + 1u));
        return;
    }

    if (strcmp(command, "can") == 0) {
        for (uint8_t index = 0; index < 2; ++index) {
            const CanBusId bus = index == 0 ? CAN_BUS_1 : CAN_BUS_2;
            CanDiagnostics diagnostics = {};
            const bool diagnosticsAvailable = canManager.getDiagnostics(bus, diagnostics);
            uint32_t tx = 0, rx = 0, errors = 0;
            canManager.getStats(bus, tx, rx, errors);
            Serial.printf("CAN%u active=%s listenOnly=%s diagnostics=%s busOff=%s RX=%lu TX=%lu errors=%lu txErr=%lu rxErr=%lu\n",
                          (unsigned)(index + 1),
                          canManager.isActive(bus) ? "yes" : "no",
                          canManager.isListenOnlyActive(bus) ? "yes" : "no",
                          diagnosticsAvailable ? "available" : "unavailable",
                          diagnostics.busOff ? "yes" : "no",
                          (unsigned long)rx,
                          (unsigned long)tx,
                          (unsigned long)errors,
                          (unsigned long)diagnostics.txErrorCounter,
                          (unsigned long)diagnostics.rxErrorCounter);
        }
        return;
    }

    if (strcmp(command, "obd") == 0) {
        VehicleData data = {};
        const bool hasData = obd2Reader.getLatestData(data);
        Serial.printf("OBD channel=CAN%u active=%s txAvailable=%s pollState=%u data=%s",
                      (unsigned)(obd2Reader.getCanBus() == CAN_BUS_1 ? 1 : 2),
                      canManager.isActive(obd2Reader.getCanBus()) ? "yes" : "no",
                      obd2Reader.canTransmit() ? "yes" : "no",
                      (unsigned)obd2Reader.getPollState(),
                      hasData ? "available" : "not-yet");
        if (hasData) {
            Serial.printf(" rpm=%u speed=%u coolant=%d voltage=%.2f",
                          data.engineRPM, data.vehicleSpeed, data.coolantTemp,
                          data.batteryVoltage);
        }
        Serial.println();
        return;
    }

    if (strcmp(command, "learn") == 0) {
        Serial.printf("Learn channel=CAN%u state=%s progress=%u%% candidates=%u\n",
                      (unsigned)(learnEngine.getCanBus() == CAN_BUS_1 ? 1 : 2),
                      learnStateName(learnEngine.getState()),
                      learnEngine.getProgressPercent(),
                      learnEngine.getCandidateCount());
        return;
    }

    if (strcmp(command, "sd status") == 0) {
        Serial.printf("SD state=%s cs=%d total=%llu free=%llu\n", sdStorage.stateText(),
                      sdStorage.csPin(), (unsigned long long)sdStorage.totalBytes(),
                      (unsigned long long)sdStorage.freeBytes());
        static const char* cats[] = { "db", "rec", "prof", "bak" };
        for (const char* c : cats) {
            const CtStorageChoice ch = getStorageChoice(c);
            Serial.printf("  %s: %s\n", c, ch == CT_STORE_SD ? "sd" : ch == CT_STORE_INTERNAL ? "internal" : "auto");
        }
        return;
    }
    if (strncmp(command, "sd cs ", 6) == 0) {
        char* end = nullptr;
        const long pin = strtol(command + 6, &end, 10);
        if (end == command + 6 || *end != '\0' || pin < -1 || pin > 48) {
            Serial.println("Usage: sd cs <gpio> (or -1 to disable)");
        } else if (sdStorage.setCsPin((int)pin)) {
            Serial.printf("SD CS pin set to %ld; state=%s\n", pin, sdStorage.stateText());
        } else {
            Serial.println("Pin rejected: reserved (strapping/USB/flash/PSRAM) or already used, or NVS write failed");
        }
        return;
    }
    if (strncmp(command, "sd store ", 9) == 0) {
        char cat[8] = {0}, val[12] = {0};
        if (sscanf(command + 9, "%7s %11s", cat, val) == 2) {
            const uint8_t v = !strcmp(val, "auto") ? CT_STORE_AUTO :
                              !strcmp(val, "internal") ? CT_STORE_INTERNAL :
                              !strcmp(val, "sd") ? CT_STORE_SD : 255;
            if (setStorageChoice(cat, v)) { Serial.println("Storage choice saved"); return; }
        }
        Serial.println("Usage: sd store <db|rec|prof|bak> <auto|internal|sd>");
        return;
    }
    if (strcmp(command, "btn status") == 0) {
        Serial.printf("Buttons mode=%s everPressed=%s invalidAdcReads=%lu\n",
                      buttons.mode() == Buttons::GPIO_MODE ? "gpio" : buttons.mode() == Buttons::ADC_MODE ? "adc" : "off",
                      buttons.everPressed() ? "yes" : "no (unverified)", (unsigned long)buttons.invalidReads());
        Serial.printf("  gpio pins U/D/L/R/OK: %d %d %d %d %d | adc pin: %d | ladder: %u %u %u %u %u\n",
                      buttons.pin(0), buttons.pin(1), buttons.pin(2), buttons.pin(3), buttons.pin(4), buttons.adcPin(),
                      buttons.ladder()[0], buttons.ladder()[1], buttons.ladder()[2], buttons.ladder()[3], buttons.ladder()[4]);
        return;
    }
    if (strcmp(command, "btn off") == 0) {
        Serial.println(buttons.setOff() ? "Buttons disabled" : "Could not save");
        return;
    }
    if (strncmp(command, "btn gpio ", 9) == 0) {
        int p[5];
        if (sscanf(command + 9, "%d %d %d %d %d", &p[0], &p[1], &p[2], &p[3], &p[4]) == 5 && buttons.setGpioPins(p))
            Serial.println("GPIO buttons saved (state stays UNVERIFIED until a key is pressed)");
        else
            Serial.println("Usage: btn gpio <up> <down> <left> <right> <ok> - pins must be distinct, free and not reserved");
        return;
    }
    if (strncmp(command, "btn adc ", 8) == 0) {
        int pin; unsigned v[5];
        if (sscanf(command + 8, "%d %u %u %u %u %u", &pin, &v[0], &v[1], &v[2], &v[3], &v[4]) == 6) {
            uint16_t lad[5];
            for (int i = 0; i < 5; i++) lad[i] = (uint16_t)(v[i] > 65535u ? 65535u : v[i]);
            if (buttons.setAdc(pin, lad)) { Serial.println("ADC buttons saved"); return; }
        }
        Serial.println("Usage: btn adc <adc1 gpio 1-10> <up> <down> <left> <right> <ok> (raw 12-bit values, distinct, < 3900)");
        return;
    }
    if (strcmp(command, "sd reset") == 0) {
        resetStorageChoices();
        Serial.println("Storage choices reset to auto");
        return;
    }

    if (strcmp(command, "storage") == 0) {
        if (!filesystemReady) {
            Serial.println("SPIFFS unavailable; user files were not formatted or erased");
            return;
        }
        const uint32_t total = SPIFFS.totalBytes();
        const uint32_t used = SPIFFS.usedBytes();
        Serial.printf("SPIFFS used=%u total=%u free=%u customProfiles=%u\n",
                      (unsigned)used,
                      (unsigned)total,
                      (unsigned)(used <= total ? total - used : 0),
                      customVehicleStore.getProfileCount());
        return;
    }

    if (strcmp(command, "errors") == 0) {
        ErrorLog* log = getErrorLog();
        const ErrorCounters& counters = log->getCounters();
        Serial.printf("Recent entries=%u bootCount=%lu CAN-TX=%lu CAN-RX=%lu busOff=%lu OBD-timeouts=%lu WiFi-failures=%lu Learn-errors=%lu\n",
                      log->getEntryCount(),
                      (unsigned long)counters.bootCount,
                      (unsigned long)counters.canTxErrors,
                      (unsigned long)counters.canRxErrors,
                      (unsigned long)counters.canBusOffEvents,
                      (unsigned long)counters.obd2Timeouts,
                      (unsigned long)counters.wifiConnectFailures,
                      (unsigned long)counters.learnErrors);
        const uint8_t count = log->getEntryCount();
        const uint8_t shown = count < 10 ? count : 10;
        for (uint8_t i = 0; i < shown; ++i) {
            LogEntry entry;
            if (log->getEntry(i, entry)) {
                Serial.printf("[%lu] category=%u severity=%u %s\n",
                              (unsigned long)entry.timestamp,
                              (unsigned)entry.category,
                              (unsigned)entry.severity,
                              entry.message);
            }
        }
        return;
    }

    if (strncmp(command, "control ", 8) == 0 && command[8] != '\0') {
        handleCommand(command + 8);
        Serial.println("[SERIAL] Control request submitted; check the command result above");
        return;
    }

    Serial.println("[SERIAL] Unknown command; enter 'help'");
}

// ○○○○○○○○○○ Command handling
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

// handleCommand() is called from the async web task, the BLE task and the
// TFT callback. It only enqueues; processCommand() runs in loop() so that
// VehicleControl, currentVehicleData and LVGL are never touched concurrently.
static constexpr uint8_t CMD_QUEUE_SIZE = 8;
static constexpr size_t  CMD_MAX_LEN    = 64;
static char       cmdQueue[CMD_QUEUE_SIZE][CMD_MAX_LEN];
static uint8_t    cmdHead = 0, cmdTail = 0;
static portMUX_TYPE cmdMux = portMUX_INITIALIZER_UNLOCKED;

void handleCommand(const char* command) {
    if (!command || strlen(command) >= CMD_MAX_LEN) {
        Serial.println("[CMD] Rejected: empty or too long");
        return;
    }
    bool queued = false;
    portENTER_CRITICAL(&cmdMux);
    uint8_t next = (cmdHead + 1) % CMD_QUEUE_SIZE;
    if (next != cmdTail) {
        strcpy(cmdQueue[cmdHead], command);
        cmdHead = next;
        queued = true;
    }
    portEXIT_CRITICAL(&cmdMux);
    if (!queued) Serial.println("[CMD] Queue full - command dropped");
}

void drainCommandQueue() {
    char cmd[CMD_MAX_LEN];
    for (;;) {
        bool have = false;
        portENTER_CRITICAL(&cmdMux);
        if (cmdTail != cmdHead) {
            strcpy(cmd, cmdQueue[cmdTail]);
            cmdTail = (cmdTail + 1) % CMD_QUEUE_SIZE;
            have = true;
        }
        portEXIT_CRITICAL(&cmdMux);
        if (!have) break;
        processCommand(cmd);
    }
}

void processCommand(const char* command) {
    lastActivityTime = millis();

    Serial.printf("[CMD] Received: %s\n", command);

    if (strncmp(command, "record_start:", 13) == 0) {
        char* end = nullptr;
        const long mask = strtol(command + 13, &end, 10);
        if (end == command + 13 || *end != '\0' || mask < 1 || mask > 3) {
            Serial.println("[RECORDER] Invalid bus mask");
        } else if (canRecorder.start((uint8_t)mask)) {
            Serial.printf("[RECORDER] Started %s on CAN mask %ld\n",
                          canRecorder.getFileName(), mask);
        } else {
            Serial.printf("[RECORDER] Start failed: %s\n", canRecorder.getLastError());
        }
        broadcastCanRecorderStatus();
        return;
    }

    if (strcmp(command, "record_stop") == 0) {
        canRecorder.stop();
        Serial.printf("[RECORDER] Stopped frames=%lu dropped=%lu file=%s error=%s\n",
                      (unsigned long)canRecorder.getFrameCount(),
                      (unsigned long)canRecorder.getDroppedFrameCount(),
                      canRecorder.getFileName(), canRecorder.getLastError());
                broadcastCanRecorderStatus();
        return;
    }

    if (strcmp(command, "record_status") == 0) {
        Serial.printf("[RECORDER] storage=%s notice=%s\n", canRecorder.getLocationText(), canRecorder.getNotice());
        Serial.printf("[RECORDER] recording=%s buses=%u frames=%lu dropped=%lu file=%s error=%s\n",
                      canRecorder.isRecording() ? "active" : "stopped",
                      canRecorder.getBusMask(),
                      (unsigned long)canRecorder.getFrameCount(),
                      (unsigned long)canRecorder.getDroppedFrameCount(),
                      canRecorder.getFileName(), canRecorder.getLastError());
                broadcastCanRecorderStatus();
        return;
    }

            if (strncmp(command, "record_delete:", 14) == 0) {
                const char* fileName = command + 14;
                if (!ctCanRecordFilenameValid(fileName)) {
                    Serial.println("[RECORDER] Invalid recording filename");
                } else if (canRecorder.deleteRecording(fileName)) {
                    Serial.printf("[RECORDER] Deleted %s\n", fileName);
                    webServer.broadcastCanRecordingFilesChanged();
                } else {
                    Serial.printf("[RECORDER] Delete failed: %s\n", canRecorder.getLastError());
                }
                broadcastCanRecorderStatus();
                return;
            }

    if (strcmp(command, "dtc_read") == 0 || strcmp(command, "dtc_clear") == 0) {
        if (currentMode != MODE_ACTIVE) {
            Serial.println("[OBD] DTC operation rejected while device is asleep");
            return;
        }
        const bool started = strcmp(command, "dtc_read") == 0
            ? obd2Reader.startDtcRead() : obd2Reader.startDtcClear();
        Serial.printf("[OBD] DTC request %s: %s\n",
                      strcmp(command, "dtc_read") == 0 ? "read" : "clear",
                      started ? "started" : "rejected (busy, Listen-Only, or CAN unavailable)");
        broadcastObdDiagnosticStatus();
        return;
    }

    if (strcmp(command, "dtc_status") == 0) {
        Serial.printf("[OBD] DTC operation=%u state=%u error=%u NRC=0x%02X count=%u\n",
                      (unsigned)obd2Reader.getDiagnosticOperation(),
                      (unsigned)obd2Reader.getDiagnosticState(),
                      (unsigned)obd2Reader.getDiagnosticError(),
                      (unsigned)obd2Reader.getDiagnosticResponseCode(),
                      (unsigned)obd2Reader.getDtcCount());
        for (uint8_t i = 0; i < obd2Reader.getDtcCount(); ++i) {
            Serial.printf("  DTC[%u]=0x%04X\n", (unsigned)i,
                          (unsigned)obd2Reader.getDtc(i));
        }
        broadcastObdDiagnosticStatus();
        return;
    }

    // While asleep, reject physical-control commands instead of allowing a
    // Web/TFT event queued before/around wake to trigger immediate CAN TX.
    // Wake is driven by CAN activity; after wake the user can explicitly
    // issue a fresh command. Safe configuration/navigation commands remain
    // available.
    if (currentMode != MODE_ACTIVE &&
        strcmp(command, "listen_only") != 0 &&
        strcmp(command, "vehicle_select") != 0 &&
        strncmp(command, "vehicle_select_dbc:", 19) != 0 &&
        strncmp(command, "vehicle_select_custom:", 22) != 0 &&
        strcmp(command, "toggle_theme") != 0) {
        Serial.println("[CMD] Device asleep - control command rejected");
        tftUI.showNotification("Device is asleep - wake it before controlling");
        return;
    }

    if (vehicleControl->isListenOnlyForSelectedBus()) {
        if (strcmp(command, "listen_only") == 0 ||
            strcmp(command, "vehicle_select") == 0 ||
            strncmp(command, "vehicle_select_dbc:", 19) == 0 ||
            strncmp(command, "vehicle_select_custom:", 22) == 0 ||
            strcmp(command, "toggle_theme") == 0) {
            handleControlCommand(command);
        } else {
            Serial.println("[CMD] Listen-Only mode - control command rejected");
            tftUI.showNotification("Listen-Only mode is active");
        }
        return;
    }

    handleControlCommand(command);
}

void handleControlCommand(const char* command) {
    bool result = false;

    if (strcmp(command, "lock") == 0) {
        result = vehicleControl->lockAllDoors();
    }
    else if (strcmp(command, "unlock") == 0) {
        result = vehicleControl->unlockAllDoors();
    }
    else if (strcmp(command, "windows_up") == 0) {
        result = vehicleControl->allWindowsUp();
    }
    else if (strcmp(command, "windows_down") == 0) {
        result = vehicleControl->allWindowsDown();
    }
    else if (strcmp(command, "sunroof") == 0) {
        result = vehicleControl->sunroofOpen();
    }
    else if (strcmp(command, "trunk") == 0) {
        result = vehicleControl->trunkOpen();
    }
    else if (strcmp(command, "mirror") == 0) {
        result = vehicleControl->foldMirrors();
    }
    else if (strcmp(command, "alarm") == 0) {
        if (currentVehicleData.alarmState == ALARM_DISARMED) {
            result = vehicleControl->alarmArm();
            if (result) {
                currentVehicleData.alarmState = ALARM_ARMED;
            }
        } else {
            result = vehicleControl->alarmDisarm();
            if (result) {
                currentVehicleData.alarmState = ALARM_DISARMED;
            }
        }
    }
    else if (strcmp(command, "listen_only") == 0) {
        // Toggles Listen-Only on the channel selected for vehicle commands.
        AppConfig*      cfg     = getConfig();
        const CanBusId  bus     = vehicleControl->selectedBus();
        const bool      current = (bus == CAN_BUS_2) ? cfg->can1ListenOnly
                                                     : cfg->listenOnlyMode;
        const bool      newMode = !current;

        // reconfigureMode() performs a real driver switch on that channel.
        if (!canManager.reconfigureMode(bus, newMode)) {
            tftUI.showNotification("CAN mode switch failed - please restart the device");
            Serial.println("[CMD] reconfigureMode failed - driver state unknown");
            return;
        }

        if (bus == CAN_BUS_2) cfg->can1ListenOnly = newMode;
        else                  cfg->listenOnlyMode = newMode;
        saveConfig();
        tftUI.showNotification(newMode ? "Listen-Only mode enabled"
                                       : "Normal mode enabled");
        Serial.printf("[CMD] CAN%u Listen-Only: %s (driver mode switched)\n",
                      bus == CAN_BUS_2 ? 2u : 1u, newMode ? "ON" : "OFF");
        return;
    }
    else if (strcmp(command, "toggle_theme") == 0) {
        AppConfig* cfg = getConfig();
        cfg->theme = (cfg->theme == THEME_DAY) ? THEME_NIGHT : THEME_DAY;
        saveConfig();
        tftUI.setTheme(cfg->theme);
        return;
    }
    else if (strcmp(command, "vehicle_select") == 0) {
        tftUI.showNotification("Select vehicle from the menu");
        return;
    }
    else if (strncmp(command, "vehicle_select_dbc:", 19) == 0) {
        // Format: "vehicle_select_dbc:Brand|Model"
        char buf[64];
        strncpy(buf, command + 19, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char* sep = strchr(buf, '|');
        if (sep) {
            *sep = '\0';
            if (activeProfileManager->selectDBCVehicle(buf, sep + 1)) {
                tftUI.showNotification("Vehicle (DBC) selected");
            } else {
                tftUI.showNotification("Vehicle profile not found");
            }
        } else {
            tftUI.showNotification("Invalid vehicle selection");
        }
        return;
    }
    else if (strncmp(command, "vehicle_select_custom:", 22) == 0) {
        char* endp = nullptr;
        long parsed = strtol(command + 22, &endp, 10);
        if (endp == command + 22 || *endp != '\0' || parsed < 0 || parsed > 255) {
            tftUI.showNotification("Invalid profile index");
            return;
        }
        uint8_t idx = (uint8_t)parsed;
        if (activeProfileManager->selectCustomVehicle(idx)) {
            tftUI.showNotification("Vehicle (custom) selected");
        } else {
            tftUI.showNotification("Profile not found");
        }
        return;
    }
    else {
        Serial.printf("[CMD] Unknown command: %s\n", command);
        tftUI.showNotification("Unknown command");
        return;
    }

    if (result) {
        tftUI.showNotification("Command sent");
        webServer.broadcastStatus(command);
    } else {
        String reason = vehicleControl->getLastErrorMessage();
        if (reason.length() > 0) {
            tftUI.showNotification(reason.c_str());
        } else {
            tftUI.showNotification("Command failed");
        }
        Serial.printf("[CMD] Command execution failed: %s\n", command);
    }
}

static ModuleState canLinkToModuleState(CtCanLinkState link) {
    switch (link) {
        case CT_LINK_TRAFFIC:    return MODULE_READY;
        case CT_LINK_NO_TRAFFIC: return MODULE_UNVERIFIED;
        case CT_LINK_BUS_OFF:
        case CT_LINK_DOWN:
        default:                 return MODULE_ERROR;
    }
}

void refreshModuleStatuses() {
    static uint32_t lastCheck = 0;
    static ModuleState lastStates[MODULE_COUNT] = {};
    const uint32_t now = millis();
    if (now - lastCheck < 500) return;
    lastCheck = now;

    // Real traffic decides "connected"; a started driver alone is UNVERIFIED.
    const CtCanLinkState can1Link = ctCanLinkState(
        canManager.isActive(CAN_BUS_1), g_canBusOff[0],
        canManager.getLastRxTime(CAN_BUS_1), now, CAN_LINK_TRAFFIC_TIMEOUT_MS);
    const CtCanLinkState can2Link = ctCanLinkState(
        canManager.isActive(CAN_BUS_2), g_canBusOff[1],
        canManager.getLastRxTime(CAN_BUS_2), now, CAN_LINK_TRAFFIC_TIMEOUT_MS);

    const ModuleState states[MODULE_COUNT] = {
        (wifiManager.isEnabled() && wifiManager.isConnected()) ? MODULE_READY : (wifiManager.isEnabled() ? MODULE_ERROR : MODULE_DISABLED),
        webServer.isStarted() ? MODULE_READY : MODULE_ERROR,
        (can1Link == CT_LINK_TRAFFIC || can2Link == CT_LINK_TRAFFIC) ? MODULE_READY
            : ((can1Link == CT_LINK_NO_TRAFFIC || can2Link == CT_LINK_NO_TRAFFIC) ? MODULE_UNVERIFIED : MODULE_ERROR),
        !canManager.isActive(obd2Reader.getCanBus()) ? MODULE_ERROR :
            (!obd2Reader.canTransmit() ? MODULE_DISABLED : MODULE_UNVERIFIED),
    #ifdef CARTOUCH_HEADLESS
        MODULE_DISABLED,
        MODULE_DISABLED,
    #else
        tftUI.isTouchAvailable() ? MODULE_READY : MODULE_NOT_PRESENT,
        tftUI.isDisplayAvailable() ? MODULE_UNVERIFIED : MODULE_ERROR,
    #endif
        bleManager.isEnabled() ? MODULE_READY : MODULE_ERROR,
        (filesystemReady && customVehicleStoreReady) ? MODULE_READY : MODULE_ERROR,
        canLinkToModuleState(can1Link),
        canLinkToModuleState(can2Link),
        // Not every board has PSRAM; its absence is normal, not an error.
        (ESP.getPsramSize() > 0) ? MODULE_READY : MODULE_NOT_PRESENT,
        // SD: DISABLED = no CS pin configured, NOT_PRESENT = no card answering.
        (sdStorage.state() == SdStorage::READY) ? MODULE_READY :
            (sdStorage.state() == SdStorage::ERROR_STATE ? MODULE_ERROR :
            (sdStorage.state() == SdStorage::SD_DISABLED ? MODULE_DISABLED : MODULE_NOT_PRESENT)),
        // Buttons: DISABLED = not configured; UNVERIFIED until a real press is
        // seen (a key that was never pressed looks identical to "not wired").
        buttons.mode() == Buttons::OFF ? MODULE_DISABLED :
            (buttons.everPressed() ? MODULE_READY : MODULE_UNVERIFIED)
    };
    bool changed = false;
    for (uint8_t i = 0; i < MODULE_COUNT; ++i) {
        if (states[i] != lastStates[i]) {
            moduleStatusManager.setState((ModuleId)i, states[i]);
            lastStates[i] = states[i];
            changed = true;
        }
    }
    if (changed) {
        tftUI.setCANStatus(states[MODULE_CAN] == MODULE_READY);
        tftUI.setWiFiStatus(states[MODULE_WIFI] == MODULE_READY);
        for (uint8_t i = 0; i < MODULE_COUNT; ++i) {
            tftUI.setModuleStatus((ModuleId)i, states[i]);
        }
        webServer.broadcastModuleStatus();
    }
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Sleep / wake
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

void checkAutoSleep() {
    if (currentMode != MODE_ACTIVE) return;

    AppConfig* cfg            = getConfig();
    uint32_t   inactivityTime = millis() - lastActivityTime;

    if (inactivityTime >= cfg->sleepTimeout) {
        Serial.println("[SLEEP] Entering sleep mode (inactivity timeout)");
        currentMode = MODE_SLEEP;

        tftUI.setDeviceMode(MODE_SLEEP);
        // Wi-Fi stays on in sleep: it used to be switched off here, which made the
        // web page unreachable until a CAN frame or a screen touch woke the device.

        Serial.println("[SLEEP] Device asleep - waiting for CAN activity to wake");
    }
}

void wakeFromSleep() {
    if (currentMode == MODE_ACTIVE) return;

    Serial.println("[WAKE] Waking from sleep...");

    currentMode = MODE_ACTIVE;
    lastActivityTime = millis();
    // Do not let the first active loop immediately start OBD polling after a
    // CAN wake event. The wake frame itself may be unrelated to OBD, so hold
    // all OBD requests briefly and let the user/device settle first.
    lastWakeTime = lastActivityTime;

    tftUI.setDeviceMode(MODE_ACTIVE);

    if (!wifiManager.isConnected()) {
        wifiManager.begin(getConfig()->wifiSSID[0] ? 2 : 1);
    }

    tftUI.showNotification("Awake!");

    Serial.println("[WAKE] Device is awake");
}
