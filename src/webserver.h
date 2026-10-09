/**
 * webserver.h - HTTP and WebSocket server
 *
 * HTTP routes and WebSocket messages share the same authentication,
 * session and command-safety rules. Learn Mode and custom vehicle
 * management use the same authenticated interface.
 */

#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include "config.h"
#include "can_manager.h"
#include "learn_engine.h"
#include "custom_vehicle_store.h"
#include "active_profile_manager.h"
#include "vehicle_control.h"
#include "ct_verify.h"
#include "module_status.h"

#define WS_MAX_CLIENTS        8       // Max WebSocket clients whose auth state is tracked at once
#define SESSION_TOKEN_TIMEOUT 900000  // Session token validity, ms (15 minutes)
#define COMMAND_RATE_LIMIT_MS 300     // Minimum spacing between control commands per client, ms

// Upper bound on the size of a single inbound WebSocket text message and
// on the size of the "json" argument of the profile-import route. Both
// exist purely to bound the heap a single unauthenticated/authenticated
// peer can force the device to allocate before any validation runs:
// every legitimate protocol message is well under 2 KB, and even a full
// 32-command profile export stays under 8 KB.
#define WS_MAX_MESSAGE_LEN    2048  // Max accepted WebSocket message, bytes
#define MAX_IMPORT_JSON_LEN   8192  // Max accepted profile-import JSON, bytes

// How long a verification transaction (verify_command -> verify_confirm)
// stays valid. After this, verify_confirm is rejected and the user must
// re-run verify_command. Bounds the window in which a confirmation could
// be replayed or applied to a command other than the one just tested.
#define VERIFY_PENDING_TIMEOUT_MS 120000  // 2 minutes

// Login brute-force protection.
// Applies to every entry point that directly compares a submitted
// password against cfg->webPass: Basic-Auth on "/" and on every route
// behind _authenticate(), and the "/login" form. Failures are counted
// per client IP (small LRU table), so one device guessing wrong passwords
// cannot lock the owner out.
#define LOGIN_MAX_ATTEMPTS 5      // Failed attempts allowed before lockout
#define LOGIN_LOCKOUT_MS   30000  // Lockout duration once the limit is hit, ms (30s)
#define LOGIN_TRACK_SLOTS  8      // Client IPs tracked at once (oldest entry is replaced)

typedef void (*WebCommandCallback)(const char* command);

// Per-WebSocket-client authentication state

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Web protocol state
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

struct WsClientAuth {
    uint32_t clientId        = 0;
    bool     authenticated   = false;
    bool     canMonitor      = false;
    uint8_t  monitorBus      = 0;
    uint32_t lastCommandTime = 0;
    String   sessionToken;
    bool     inUse                   = false;

    // -- Pending verification transaction (P0.1) ------------------------------
    // Set by a successful "verify_command" (one-shot real send of an
    // UNVERIFIED command) and consumed by the matching "verify_confirm".
    // This binds a confirmation to the exact client, profile and label
    // that were just test-sent, and rejects:
    //   - confirmations with no prior verify_command,
    //   - confirmations from a different client/session,
    //   - confirmations for a different profile or label,
    //   - confirmations after VERIFY_PENDING_TIMEOUT_MS have elapsed,
    //   - a second confirmation reusing an already-consumed transaction.
    bool     hasPendingVerify        = false;
    uint8_t  pendingVerifyProfileId  = 255;
    char     pendingVerifyLabel[32]  = {0};
    uint32_t pendingVerifyToken      = 0;
    uint32_t pendingVerifyAt         = 0;   // millis() timestamp of the send
    uint32_t pendingVerifyFingerprint = 0;  // exact command payload snapshot
};

class WebServerManager {

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Public API
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

public:
    WebServerManager();

    void begin(uint16_t port = WEB_PORT);
    void update();
    void setCommandCallback(WebCommandCallback cb);
    void broadcastVehicleData(const VehicleData& data);
    void broadcastStatus(const char* status);
    void broadcastCanDiagnostics(const CanDiagnostics& diagnostics,
                                 const char* interfaceName = "CAN1");
    void broadcastCanRecordingStatus(bool active, uint8_t busMask,
                                     uint32_t frames, uint32_t droppedFrames,
                                     const char* fileName, const char* error);
    void broadcastCanRecordingFilesChanged();
    void broadcastObdDiagnosticStatus(uint8_t state, uint8_t operation,
                                      uint8_t error, uint8_t responseCode,
                                      const uint16_t* dtcList, uint8_t dtcCount);
    void broadcastModuleStatus();
    bool isClientConnected();
    bool isStarted() const { return _started; }
    void setModuleStatusManager(ModuleStatusManager* manager);
    void attachCanService(CANService* canService);
    uint8_t getClientCount();

    /** True once if authenticated web traffic arrived since the last call (keeps auto-sleep away). */
    bool consumeActivity() { const bool a = _activity; _activity = false; return a; }

    /**
     * Attaches the Learn Mode / custom-profile modules. Must be called
     * once in setup(), before begin() (same pattern as
     * setCommandCallback). Kept as a separate method so the
     * WebServerManager constructor stays untouched and setup() ordering
     * in main.cpp remains flexible.
     */
    void attachLearnModules(LearnEngine* learnEngine,
                            CustomVehicleStore* customStore,
                            ActiveProfileManager* profileManager,
                            VehicleControl* vehicleControl);

    /**
     * Invalidates every active web session - both the HTTP session
     * token and every connected WebSocket client's auth state.
     * Registered as a PasswordChangeCallback (see config.h) via
     * registerPasswordChangeCallback so it fires whenever the password
     * is changed from *either* interface (TFT or web), not only when
     * changed from the web itself.
     */
    void invalidateAllSessions();

    // Invalidates pending verification transactions for a deleted profile.
    void invalidatePendingVerificationForProfile(uint8_t profileId);

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Authentication and handlers
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

private:
    AsyncWebServer      _server;
    AsyncWebSocket       _ws;
    WebCommandCallback     _commandCallback;
    bool                      _started;

    // Current session token (issued after a successful login) and when it was issued
    String    _sessionToken;
    uint32_t  _sessionTokenIssuedAt;

    // Login brute-force protection state (see LOGIN_MAX_ATTEMPTS above).
    // Shared across "/", "/login" and _authenticate() - all funnel
    // through _isLoginLocked()/_registerLoginFailure()/_registerLoginSuccess().
    struct LoginTrack {
        bool     used;
        uint32_t ip;
        uint8_t  fails;
        uint32_t lockUntil;  // 0 = not locked; otherwise millis() timestamp when lockout ends
        uint32_t lastSeen;
    };
    LoginTrack _loginTrack[LOGIN_TRACK_SLOTS];
    LoginTrack* _loginSlot(uint32_t ip, bool create);
    volatile bool _activity;  // set by authenticated web traffic, read by loop() for auto-sleep

    // Auth state for each connected WebSocket client
    WsClientAuth _clientAuth[WS_MAX_CLIENTS];

    // Learn Mode module pointers - raw pointers (not references) since
    // they may still be null before attachLearnModules() runs; every
    // method that uses them must null-check.
    ModuleStatusManager*      _moduleStatus;
    LearnEngine*             _learnEngine;
    CustomVehicleStore*        _customStore;
    ActiveProfileManager*        _profileManager;
    VehicleControl*                _vehicleControl;
    CANService*                   _canService;
    uint32_t                      _lastCanMonitorSend;
    bool                          _monitorSubscribed[2];

    void _handleWebSocketEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                               AwsEventType type, void* arg, uint8_t* data, size_t len);
    void _handleAPIControl(AsyncWebServerRequest* request);
    void _handleAPICanConfig(AsyncWebServerRequest* request);
    void _handleAPIStatus(AsyncWebServerRequest* request);
    void _handleNotFound(AsyncWebServerRequest* request);
    void _registerDbcRoutes();

    // -- OTA update via web (/update) - behind the same authentication ------------
    void _handleOtaUpload(AsyncWebServerRequest* request, const String& filename,
                          size_t index, uint8_t* data, size_t len, bool final);
    void _handleOtaFinished(AsyncWebServerRequest* request);
    String    _otaError;          // Most recent upload error (empty = none)
    size_t     _otaBytes;         // Bytes written so far in the current upload
    bool        _otaIsFs;         // true = filesystem image upload (spiffs.bin)
    bool         _rebootPending;  // Reboot after a successful OTA (handled in update())
    uint32_t      _rebootAt;

    // Static instance pointer - registerPasswordChangeCallback only
    // accepts a plain, non-capturing function pointer, so this mirrors
    // the pThisUI pattern in tft_ui.cpp.
    static WebServerManager* _instance;
    static void _staticInvalidateSessions();  // Non-capturing bridge to invalidateAllSessions()

    bool    _authenticate(AsyncWebServerRequest* request);
    String   _generateSessionToken();
    bool      _isValidSessionToken(const char* token);

    // Login brute-force protection (external review finding A)
    bool  _isLoginLocked(uint32_t ip, uint32_t& remainingMs);
    void  _registerLoginFailure(uint32_t ip);
    void  _registerLoginSuccess(uint32_t ip);

    WsClientAuth* _findOrCreateClientAuth(uint32_t clientId);
    WsClientAuth* _findClientAuth(uint32_t clientId);
    void           _removeClientAuth(uint32_t clientId);

    String _vehicleDataToJSON(const VehicleData& data);

    // Learn Mode WebSocket message handling - only called after a client
    // is fully authenticated (auth->authenticated==true), same as
    // existing "command" messages.
    void _handleLearnModeMessage(AsyncWebSocketClient* client, JsonDocument& doc, const char* type);
    void _syncCanMonitorSubscription();
    void _sendCanFrameToMonitorClients(const CanRxFrame& frame);

    String _learnStateToJSON();    // Serializes the learn engine's current state for the client

    void _registerCustomVehicleRoutes();    // Custom-profile management REST endpoints
};

#endif    // WEBSERVER_H
