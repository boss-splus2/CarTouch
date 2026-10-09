/**
 * webserver.cpp - Web server and WebSocket implementation
 *
 * Session-token auth, rate limiting, and auth on static files apply
 * uniformly across every route, including Learn Mode's messages/routes.
 *
 * User-facing strings (HTML page text and JSON error messages shown in the
 * UI) are English. Protocol identifiers remain stable and are not localized.
 */

#include "webserver.h"
#include "ble_manager.h"
#include "custom_vehicle.h"
#include "error_log.h"
#include "ct_verify.h"
#include "ct_can_config.h"
#include "ct_storage_guard.h"
#include <SPIFFS.h>
#include <SD.h>
#include "sd_storage.h"
#include <esp_random.h>
#include <Update.h>
#include "ct_password.h"
#include "ct_ota_header.h"
#include "ct_ota_lock.h"
#include "ct_ota_authenticity.h"
#include "ct_credentials.h"
#include "ct_sha256.h"

#include "ct_hex_parser.h"
#include "ct_index_parser.h"
#include "ct_battery.h"
#include "ct_obd_validity.h"
#include "ct_can_record.h"
#include "dbc_store.h"
#include "ct_http_body_limit.h"
#include "ct_origin.h"
#include "ct_json_validation.h"
#include "wifi_manager.h"

extern WiFiManager wifiManager;    // defined in main.cpp

static inline uint32_t ctClientIp(AsyncWebServerRequest* r) {
    return (uint32_t)r->client()->remoteIP();
}
#include <stdlib.h>

// Constant-time string comparison (same idea as the BLE OTA check) so the
// response time does not reveal how much of a guess was correct.
static bool ctSecureEquals(const char* expected, const String& given) {
    const size_t el = strlen(expected);
    const size_t gl = given.length();
    uint8_t diff = (uint8_t)(el != gl);
    for (size_t i = 0; i < el; ++i) {
        diff |= (uint8_t)((uint8_t)expected[i] ^ (i < gl ? (uint8_t)given[i] : 0));
    }
    return diff == 0;
}

namespace {
static const size_t DBC_LIST_LIMIT = 64;
static const size_t DBC_HTTP_BODY_LIMIT = CT_DBC_MAX_BYTES + 2048u;
static const size_t HTTP_FORM_BODY_LIMIT = 2048u;
static const size_t PROFILE_IMPORT_BODY_LIMIT = MAX_IMPORT_JSON_LEN * 3u + 32u;
static CtSha256 gOtaSha256;
static char gOtaExpectedSha256[65] = {};
static char gOtaSignatureHex[CT_OTA_SIG_HEX_MAX + 1] = {};   // optional "sig" query parameter

class HttpBodyLimitHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* request) const override {
        const size_t limit = ctHttpBodyLimitForPath(
            request->url().c_str(), HTTP_FORM_BODY_LIMIT,
            PROFILE_IMPORT_BODY_LIMIT, DBC_HTTP_BODY_LIMIT);
        return ctHttpBodyExceedsLimit(request->contentLength(),
                                      request->hasHeader("Transfer-Encoding"),
                                      limit);
    }

    void handleRequest(AsyncWebServerRequest* request) override {
        request->send(413, "application/json",
                      "{\"error\":\"Request body exceeds the maximum accepted size\"}");
    }
};

// Refuses state-changing requests (POST/PUT/PATCH/DELETE) that a browser sends
// from another website. Basic-Auth credentials are remembered by the browser
// and attached automatically, so without this check any web page opened on a
// phone that is connected to the CarTouch Wi-Fi could send commands.
// Requests without an Origin header (curl, scripts) pass; they still need login.
class CrossOriginGuardHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* request) const override {
        const auto m = request->method();
        if (m != HTTP_POST && m != HTTP_PUT && m != HTTP_PATCH && m != HTTP_DELETE) return false;
        if (!request->hasHeader("Origin")) return false;
        const String origin = request->header("Origin");
        const String host = request->host();
        return !ctOriginAllowed(origin.c_str(), host.c_str());
    }

    void handleRequest(AsyncWebServerRequest* request) override {
        request->send(403, "application/json",
                      "{\"error\":\"Cross-site request refused\"}");
    }
};

// DNS-rebinding guard: every request (any method, including the WebSocket
// upgrade) must carry a Host header matching a current device IP or the
// explicitly allowed device name. See ctHostAllowed() in ct_origin.h.
class HostGuardHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* request) const override {
        // Every HTTP/WebSocket request must present a Host header. This
        // closes the HTTP/1.0-style bypass as well as DNS-rebinding attempts.
        if (!request->hasHeader("Host")) return true;
        const String host = request->host();
        const String ap  = WiFi.softAPIP().toString();
        const String sta = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
        return !ctHostAllowed(host.c_str(), ap.c_str(), sta.c_str(), WIFI_AP_NAME);
    }

    void handleRequest(AsyncWebServerRequest* request) override {
        request->send(403, "application/json",
                      "{\"error\":\"Host header not allowed\"}");
    }
};

struct DbcUploadRequestState {
    int httpStatus;
    bool started;
    bool dataComplete;
    bool complete;
    char error[112];
    char name[CT_DBC_NAME_MAX + 1];
    CtStorageLoc location;
};

static void setDbcUploadError(DbcUploadRequestState& state, int status,
                              const char* message) {
    state.httpStatus = status;
    state.complete = false;
    strncpy(state.error, message ? message : "DBC upload failed", sizeof(state.error) - 1);
    state.error[sizeof(state.error) - 1] = '\0';
}

static const char* dbcLocationName(CtStorageLoc location) {
    return location == CT_LOC_SD ? "sd" : "internal";
}
}

static bool parseHexUint32(const char* text, uint32_t& value, size_t maxDigits) {
    return ctParseHexUint32(text, value, maxDigits);
}

static bool parseHexByteToken(const char* token, uint8_t& value) {
    return ctParseHexByteToken(token, value);
}

static bool parseHttpProfileIndex(const String& text, uint8_t& index) {
    return ctParseBoundedIndex(text.c_str(), MAX_CUSTOM_VEHICLES, index);
}

static bool canReplaceFilesystemWithoutUserData() {
    const bool filesystemMounted = SPIFFS.totalBytes() != 0;
    if (!filesystemMounted) return ctFilesystemOtaAllowed(false, false, false);

    const char* userDbcManifestPaths[] = {
        "/dbc_user.json",
        "/dbc_user.json.tmp",
        "/dbc_user.json.bak"
    };
    for (const char* path : userDbcManifestPaths) {
        if (SPIFFS.exists(path)) return ctFilesystemOtaAllowed(true, false, true);
    }

    const char* suffixes[] = {".json", ".json.tmp", ".json.bak"};
    for (uint8_t index = 0; index < MAX_CUSTOM_VEHICLES; ++index) {
        const String slot = String(index);
        const String prefixes[] = {
            String("/custom_vehicles/p") + slot,
            String("/custom_vehicles/profile_") + slot
        };
        for (const String& prefix : prefixes) {
            for (const char* suffix : suffixes) {
                if (SPIFFS.exists(prefix + suffix)) return ctFilesystemOtaAllowed(true, true, false);
            }
        }
    }
    File root = SPIFFS.open("/");
    if (root) {
        File entry = root.openNextFile();
        while (entry) {
            String name = entry.name();
            if (name.startsWith("/")) name.remove(0, 1);
            const bool recording = ctCanRecordFilenameValid(name.c_str());
            entry.close();
            if (recording) {
                root.close();
                return ctFilesystemOtaAllowed(true, true, false);
            }
            entry = root.openNextFile();
        }
        root.close();
    }
    return ctFilesystemOtaAllowed(true, false, false);
}

static bool parseJsonBoundedIndex(JsonVariantConst value, uint8_t limit, uint8_t& index) {
    if (!value.is<int>()) return false;
    const int parsed = value.as<int>();
    if (parsed < 0 || parsed >= limit) return false;
    index = (uint8_t)parsed;
    return true;
}

static bool parseJsonProfileIndex(JsonVariantConst value, uint8_t& index) {
    return parseJsonBoundedIndex(value, MAX_CUSTOM_VEHICLES, index);
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ OTA page (embedded in firmware, independent of SPIFFS)
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// Deliberately kept in firmware rather than SPIFFS: this page still
// works even if the web asset files are corrupted.
static const char OTA_PAGE_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CarTouch - Firmware Update</title>
<style>
body{font-family:system-ui,sans-serif;background:#0f1a30;color:#eee;margin:0;padding:16px;max-width:760px;margin:auto}
.card{background:#16213e;border-radius:10px;padding:16px;margin-bottom:14px}
h3{margin:0 0 10px}.muted{opacity:.75;font-size:.9rem}input[type=file]{width:100%;margin:10px 0}button{width:100%;padding:12px;border:0;border-radius:8px;background:#e94560;color:#fff;font-size:1rem}button:disabled{opacity:.5}progress{width:100%;height:14px;margin-top:8px}.msg{margin-top:8px;font-size:.9rem}.warn{color:#f5a623;font-size:.85rem}a{color:#7fb3ff}
</style></head><body>
<div class="card"><h3>Firmware Update</h3><p class="muted">Upload a firmware .bin file and enter the SHA-256 from its matching .sha256 file. Do not power off the device during the update.</p>
<input type="file" id="f-fw" accept=".bin"><input type="text" id="h-fw" maxlength="64" autocomplete="off" placeholder="Expected SHA-256 (64 hex characters)"><button id="b-fw">Upload and Install Firmware</button>
<progress id="p-fw" value="0" max="100" hidden></progress><div class="msg" id="m-fw"></div></div>
<div class="card"><h3>Web Filesystem Update</h3><p class="warn">Filesystem updates are blocked while custom profiles, CAN recordings, or user DBC manifests/recovery files exist in internal SPIFFS. Export profiles and user DBCs, and back up recordings elsewhere before removing protected data and retrying. A filesystem image replaces all SPIFFS contents; this update does not create an automatic backup.</p>
<input type="file" id="f-fs" accept=".bin"><input type="text" id="h-fs" maxlength="64" autocomplete="off" placeholder="Expected SHA-256 (64 hex characters)"><button id="b-fs">Upload and Install Web Files</button>
<progress id="p-fs" value="0" max="100" hidden></progress><div class="msg" id="m-fs"></div></div>
<p><a href="/">← Back to CarTouch</a></p>
<script>
function up(t){
  var f=document.getElementById('f-'+t).files[0],m=document.getElementById('m-'+t),p=document.getElementById('p-'+t),b=document.getElementById('b-'+t);
  if(!f){m.textContent='Select a .bin file first.';return;}
  var sha=document.getElementById('h-'+t).value.trim();
  if(!/^[0-9a-fA-F]{64}$/.test(sha)){m.textContent='Enter the matching 64-character SHA-256 digest first.';return;}
  var x=new XMLHttpRequest(),fd=new FormData(); fd.append('file',f,f.name); b.disabled=true;p.hidden=false;p.value=0;
  m.textContent='Uploading... Do not close this page or power off the device.';
  x.upload.onprogress=function(e){if(e.lengthComputable)p.value=e.loaded*100/e.total;};
  x.onload=function(){b.disabled=false;var r;try{r=JSON.parse(x.responseText);}catch(e){r={ok:false,msg:'Invalid response ('+x.status+')'};}m.textContent=(r.ok?'✓ ':'✗ ')+r.msg;};
  x.onerror=function(){b.disabled=false;m.textContent='✗ Connection lost.';};
  x.open('POST','/update?type='+t+'&sha256='+encodeURIComponent(sha)); x.send(fd);
}
document.getElementById('b-fw').onclick=function(){up('fw');};
document.getElementById('b-fs').onclick=function(){up('fs');};
</script></body></html>)rawliteral";

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Constructor
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

WebServerManager* WebServerManager::_instance = nullptr;

WebServerManager::WebServerManager()
    : _server(WEB_PORT), _ws("/ws") {
    _commandCallback      = nullptr;
    _started                = false;
    _sessionToken             = "";
    _sessionTokenIssuedAt        = 0;

    memset(_loginTrack, 0, sizeof(_loginTrack));
    _activity                 = false;

    _otaError          = "";
    _otaBytes            = 0;
    _otaIsFs               = false;
    _rebootPending            = false;
    _rebootAt                   = 0;

    _moduleStatus       = nullptr;
    _learnEngine        = nullptr;
    _customStore           = nullptr;
    _profileManager           = nullptr;
    _vehicleControl              = nullptr;
    _canService = nullptr;
    _lastCanMonitorSend = 0;
    _monitorSubscribed[0] = false;
    _monitorSubscribed[1] = false;

    // Current architecture assumes a single WebServerManager instance
    // (matching webServer in main.cpp). If multiple instances are ever
    // created, this pattern would need to become a list/vector.
    _instance = this;
    registerPasswordChangeCallback(&WebServerManager::_staticInvalidateSessions);
}

void WebServerManager::_staticInvalidateSessions() {
    if (_instance) {
        _instance->invalidateAllSessions();
    }
}

void WebServerManager::invalidatePendingVerificationForProfile(uint8_t profileId) {
    for (int i = 0; i < WS_MAX_CLIENTS; ++i) {
        if (_clientAuth[i].inUse && _clientAuth[i].hasPendingVerify &&
            _clientAuth[i].pendingVerifyProfileId == profileId) {
            _clientAuth[i].hasPendingVerify = false;
            _clientAuth[i].pendingVerifyFingerprint = 0;
        }
    }
}

void WebServerManager::invalidateAllSessions() {
    // 1. Invalidate the HTTP session token.
    _sessionToken = "";
    _sessionTokenIssuedAt = 0;

    // 2. Invalidate every connected WebSocket client's auth state so
    // already-connected clients cannot continue using stale credentials.
    // Each post-auth WebSocket message re-validates the session token, so
    // an authenticated client cannot continue issuing commands after expiry
    // or password-change invalidation.
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        _clientAuth[i].authenticated = false;
        _clientAuth[i].canMonitor = false;
        _clientAuth[i].sessionToken = "";
        _clientAuth[i].hasPendingVerify      = false;
        _clientAuth[i].pendingVerifyProfileId = 255;
        memset(_clientAuth[i].pendingVerifyLabel, 0, sizeof(_clientAuth[i].pendingVerifyLabel));
        _clientAuth[i].pendingVerifyToken    = 0;
        _clientAuth[i].pendingVerifyAt       = 0;
            _clientAuth[i].pendingVerifyFingerprint = 0;
    }
    _syncCanMonitorSubscription();

    Serial.println("[WEB] All web sessions invalidated (password changed)");
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Learn Mode module wiring
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WebServerManager::attachLearnModules(LearnEngine* learnEngine,
                                          CustomVehicleStore* customStore,
                                          ActiveProfileManager* profileManager,
                                          VehicleControl* vehicleControl) {
    _learnEngine     = learnEngine;
    _customStore      = customStore;
    _profileManager     = profileManager;
    _vehicleControl       = vehicleControl;
}

void WebServerManager::setModuleStatusManager(ModuleStatusManager* manager) {
    _moduleStatus = manager;
}

void WebServerManager::attachCanService(CANService* canService) {
    _canService = canService;
    _syncCanMonitorSubscription();
}

void WebServerManager::_syncCanMonitorSubscription() {
    if (!_canService) return;
    bool requested[2] = {false, false};
    for (uint8_t i = 0; i < WS_MAX_CLIENTS; ++i) {
        const WsClientAuth& auth = _clientAuth[i];
        if (auth.inUse && auth.authenticated && auth.canMonitor && auth.monitorBus <= 1) {
            requested[auth.monitorBus] = true;
        }
    }

    for (uint8_t bus = 0; bus < 2; ++bus) {
        if (requested[bus] == _monitorSubscribed[bus]) continue;
        if (requested[bus]) {
            _canService->subscribeRx((CanBusId)bus, CAN_RX_MONITOR);
        } else {
            _canService->unsubscribeRx((CanBusId)bus, CAN_RX_MONITOR);
        }
        _monitorSubscribed[bus] = requested[bus];
    }
}

void WebServerManager::_sendCanFrameToMonitorClients(const CanRxFrame& frame) {
    JsonDocument doc;
    doc["type"] = "can_frame";
    doc["bus"] = frame.bus == CAN_BUS_1 ? "CAN1" : "CAN2";
    doc["id"] = frame.message.id;
    doc["extended"] = frame.message.isExtended;
    doc["remote"] = frame.message.isRemote;
    doc["length"] = frame.message.length;
    doc["timestamp"] = frame.receivedAtMs;
    doc["dropped"] = _canService
        ? _canService->getRxDrops(frame.bus, CAN_RX_MONITOR) : 0;
    JsonArray bytes = doc["data"].to<JsonArray>();
    for (uint8_t i = 0; i < frame.message.length; ++i) {
        bytes.add(frame.message.data[i]);
    }

    String json;
    serializeJson(doc, json);
    for (AsyncWebSocketClient& client : _ws.getClients()) {
        WsClientAuth* auth = _findClientAuth(client.id());
        if (auth && auth->authenticated && auth->canMonitor &&
            auth->monitorBus == (uint8_t)frame.bus) {
            client.text(json);
        }
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Session token
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

String WebServerManager::_generateSessionToken() {
    uint8_t randomBytes[16];
    esp_fill_random(randomBytes, sizeof(randomBytes));

    String token = "";
    char hexBuf[3];
    for (int i = 0; i < 16; i++) {
        snprintf(hexBuf, sizeof(hexBuf), "%02x", randomBytes[i]);
        token += hexBuf;
    }
    return token;
}

bool WebServerManager::_isValidSessionToken(const char* token) {
    if (!token || _sessionToken.length() == 0) return false;

    if (millis() - _sessionTokenIssuedAt > SESSION_TOKEN_TIMEOUT) {
        return false;
    }

    return ctSecureEquals(_sessionToken.c_str(), String(token));    // constant-time
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Client auth record management
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

WsClientAuth* WebServerManager::_findClientAuth(uint32_t clientId) {
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (_clientAuth[i].inUse && _clientAuth[i].clientId == clientId) {
            return &_clientAuth[i];
        }
    }
    return nullptr;
}

WsClientAuth* WebServerManager::_findOrCreateClientAuth(uint32_t clientId) {
    WsClientAuth* existing = _findClientAuth(clientId);
    if (existing) return existing;

    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (!_clientAuth[i].inUse) {
            _clientAuth[i].inUse             = true;
            _clientAuth[i].clientId            = clientId;
            _clientAuth[i].authenticated         = false;
            _clientAuth[i].canMonitor = false;
            _clientAuth[i].monitorBus = 0;
            _clientAuth[i].lastCommandTime          = 0;
            _clientAuth[i].sessionToken = "";
            _clientAuth[i].hasPendingVerify      = false;
            _clientAuth[i].pendingVerifyProfileId = 255;
            memset(_clientAuth[i].pendingVerifyLabel, 0, sizeof(_clientAuth[i].pendingVerifyLabel));
            _clientAuth[i].pendingVerifyToken    = 0;
            _clientAuth[i].pendingVerifyAt       = 0;
            _clientAuth[i].pendingVerifyFingerprint = 0;
            return &_clientAuth[i];
        }
    }

    Serial.println("[WEB] WebSocket client capacity full - reusing slot 0");
    _clientAuth[0].inUse             = true;
    _clientAuth[0].clientId            = clientId;
    _clientAuth[0].authenticated         = false;
    _clientAuth[0].canMonitor = false;
    _clientAuth[0].monitorBus = 0;
    _clientAuth[0].lastCommandTime          = 0;
    _clientAuth[0].sessionToken = "";
    _clientAuth[0].hasPendingVerify      = false;
    _clientAuth[0].pendingVerifyProfileId = 255;
    memset(_clientAuth[0].pendingVerifyLabel, 0, sizeof(_clientAuth[0].pendingVerifyLabel));
    _clientAuth[0].pendingVerifyToken    = 0;
    _clientAuth[0].pendingVerifyAt       = 0;
    _clientAuth[0].pendingVerifyFingerprint = 0;
    _syncCanMonitorSubscription();
    return &_clientAuth[0];
}

void WebServerManager::_removeClientAuth(uint32_t clientId) {
    WsClientAuth* c = _findClientAuth(clientId);
    if (c) {
        c->inUse           = false;
        c->authenticated      = false;
        c->canMonitor = false;
        c->sessionToken         = "";
        c->clientId              = 0;
        // Drop any in-flight verification transaction (P0.1 hardening):
        // a disconnected client must not leave a live pending verify that
        // could later be confirmed after the slot is reused.
        c->hasPendingVerify      = false;
        c->pendingVerifyProfileId = 255;
        memset(c->pendingVerifyLabel, 0, sizeof(c->pendingVerifyLabel));
        c->pendingVerifyToken    = 0;
        c->pendingVerifyAt       = 0;
        c->pendingVerifyFingerprint = 0;
        _syncCanMonitorSubscription();
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ begin()
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WebServerManager::begin(uint16_t port) {
    Serial.println("[WEB] Starting web server...");

    // Baseline hardening headers on every response (D11).
    DefaultHeaders::Instance().addHeader("X-Content-Type-Options", "nosniff");
    DefaultHeaders::Instance().addHeader("X-Frame-Options", "DENY");
    DefaultHeaders::Instance().addHeader("Content-Security-Policy", "frame-ancestors 'none'");
    DefaultHeaders::Instance().addHeader("Referrer-Policy", "no-referrer");
    // Avoid browser/proxy caching of authenticated pages and API responses.
    DefaultHeaders::Instance().addHeader("Cache-Control", "no-store");

    _server.addHandler(new HostGuardHandler());
    _server.addHandler(new CrossOriginGuardHandler());
    _server.addHandler(new HttpBodyLimitHandler());

    // -- WebSocket ----------------------------------------------------------
    _ws.onEvent([this](AsyncWebSocket* server, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
        this->_handleWebSocketEvent(server, client, type, arg, data, len);
    });
    _server.addHandler(&_ws);

    // -- Routes ---------------------------------------------------------------

    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        // This route owns the authentication response so the browser
        // receives one consistent Basic-Auth challenge.
        AppConfig* authCfg = getConfig();

        const uint32_t ip = ctClientIp(request);
        uint32_t remainingMs;
        if (_isLoginLocked(ip, remainingMs)) {
            AsyncWebServerResponse* response = request->beginResponse(429, "text/html; charset=utf-8",
                "<html><head><meta charset='utf-8'></head><body dir='ltr'><h3>Too many failed attempts</h3>"
                "<p>Too many failed login attempts. Please try again later.</p>"
                "</body></html>");
            response->addHeader("Retry-After", String(remainingMs / 1000 + 1));
            request->send(response);
            return;
        }

        if (!request->authenticate(authCfg->webUser, authCfg->webPass)) {
            _registerLoginFailure(ip);
            AsyncWebServerResponse* response = request->beginResponse(401, "text/html; charset=utf-8",
                "<html><head><meta charset='utf-8'></head><body dir='ltr'><h3>Unauthorized</h3>"
                "<p>Enter the username and password in the browser login dialog. If it does not appear, reopen the page.</p>"
                "</body></html>");
            response->addHeader("WWW-Authenticate", "Basic realm=\"CarTouch\"");
            request->send(response);
            return;
        }
        _registerLoginSuccess(ip);
        _activity = true;

        if (_sessionToken.length() == 0 ||
            millis() - _sessionTokenIssuedAt > SESSION_TOKEN_TIMEOUT) {
            _sessionToken = _generateSessionToken();
            _sessionTokenIssuedAt = millis();
        }

        AsyncWebServerResponse* response;
        if (SPIFFS.exists("/index.html")) {
            response = request->beginResponse(SPIFFS, "/index.html", "text/html; charset=utf-8");
        } else {
            response = request->beginResponse(200, "text/html; charset=utf-8",
                "<h1>CarTouch</h1><p>index.html was not found.</p>");
        }
        response->addHeader("Set-Cookie", "cartouch_session=" + _sessionToken + "; Path=/; HttpOnly; SameSite=Strict");
        request->send(response);
    });

    _server.on("/login", HTTP_POST, [this](AsyncWebServerRequest* request) {
        const uint32_t ip = ctClientIp(request);
        uint32_t remainingMs;
        if (_isLoginLocked(ip, remainingMs)) {
            AsyncWebServerResponse* response = request->beginResponse(429, "text/html; charset=utf-8",
                "<html><head><meta charset='utf-8'></head><body dir='ltr'><h3>Too many failed attempts</h3>"
                "<p>Too many failed login attempts. Please try again later.</p>"
                "<a href='/'>Back</a></body></html>");
            response->addHeader("Retry-After", String(remainingMs / 1000 + 1));
            request->send(response);
            return;
        }

        String     user = request->arg("user");
        String     pass = request->arg("pass");
        AppConfig* cfg  = getConfig();

        // Both fields are always compared (no early exit) in constant time.
        const bool userOk = ctSecureEquals(cfg->webUser, user);
        const bool passOk = ctSecureEquals(cfg->webPass, pass);
        if (userOk && passOk) {
            _registerLoginSuccess(ip);
            _activity = true;
            _sessionToken = _generateSessionToken();
            _sessionTokenIssuedAt = millis();

            AsyncWebServerResponse* response = request->beginResponse(302, "text/plain", "");
            response->addHeader("Location", "/");
            response->addHeader("Set-Cookie", "cartouch_session=" + _sessionToken + "; Path=/; HttpOnly; SameSite=Strict");
            request->send(response);
        } else {
            // Deliberately does not log the submitted username/password -
            // only that an attempt failed, for basic brute-force
            // visibility without storing credential-adjacent data.
            getErrorLog()->log(LOG_CAT_WEB, LOG_WARN, "Failed login attempt");
            _registerLoginFailure(ip);
            request->send(401, "text/html; charset=utf-8", "<html><head><meta charset='utf-8'></head><body dir='ltr'><h3>Invalid username or password</h3><a href='/'>Back</a></body></html>");
        }
    });

    _server.on("/api/session-token", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) {
            // _authenticate() already sent the 401/429 response itself
            // (including the WWW-Authenticate challenge or Retry-After
            // header) - sending a second response here would double-send
            // on the same AsyncWebServerRequest, which is undefined
            // behavior in ESPAsyncWebServer (can crash the async task or
            // corrupt the connection).
            return;
        }
        if (_sessionToken.length() == 0 ||
            millis() - _sessionTokenIssuedAt > SESSION_TOKEN_TIMEOUT) {
            _sessionToken = _generateSessionToken();
            _sessionTokenIssuedAt = millis();
        }
        String json = "{\"token\":\"" + _sessionToken + "\"}";
        AsyncWebServerResponse* tokenResponse = request->beginResponse(200, "application/json", json);
        tokenResponse->addHeader("Cache-Control", "no-store");
        request->send(tokenResponse);
    });

    _server.on("/api/control", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) {
            // _authenticate() already sent the 401/429 response itself
            // (including the WWW-Authenticate challenge or Retry-After
            // header) - sending a second response here would double-send
            // on the same AsyncWebServerRequest, which is undefined
            // behavior in ESPAsyncWebServer (can crash the async task or
            // corrupt the connection).
            return;
        }
        _handleAPIControl(request);
    });

    _server.on("/api/can-config", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) return;
        _handleAPICanConfig(request);
    });

    _server.on("/api/change-password", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) {
            // _authenticate() already sent the 401/429 response itself
            // (including the WWW-Authenticate challenge or Retry-After
            // header) - sending a second response here would double-send
            // on the same AsyncWebServerRequest, which is undefined
            // behavior in ESPAsyncWebServer (can crash the async task or
            // corrupt the connection).
            return;
        }

        String newUser     = request->arg("newUser");
        String newPass     = request->arg("newPass");
        String confirmPass = request->arg("confirmPass");

        if (newPass.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"New password is empty\"}");
            return;
        }
        if (!newPass.equals(confirmPass)) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Passwords do not match\"}");
            return;
        }

        // Manually clearing _sessionToken here used to be duplicated;
        // this is now handled automatically and identically (both for
        // this route and for the TFT) by the callback registered in
        // this class's constructor - see invalidateAllSessions() and
        // registerPasswordChangeCallback.
        bool ok = setWebPassword(newUser.length() > 0 ? newUser.c_str() : nullptr, newPass.c_str());
        if (ok) {
            Serial.println("[WEB] Web password changed successfully");
            request->send(200, "application/json", "{\"success\":true}");
        } else {
            request->send(400, "application/json",
                "{\"success\":false,\"error\":\"Password must be 8 to 15 characters\"}");
        }
    });

    _server.on("/api/wifi", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) return;
        if (request->arg("forget") == "1") {
            wifiManager.forgetNetwork();
            request->send(200, "application/json", "{\"success\":true,\"message\":\"Saved network removed\"}");
            return;
        }
        String ssid = request->arg("ssid");
        String pass = request->arg("pass");
        ssid.trim();
        if (ssid.length() < 1 || ssid.length() > 31) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Network name must be 1 to 31 characters\"}");
            return;
        }
        if (pass.length() != 0 && (pass.length() < 8 || pass.length() > 63)) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Wi-Fi password must be 8 to 63 characters (empty for an open network)\"}");
            return;
        }
        // The connection runs in loop(), not here, so this async task never blocks.
        // The CarTouch access point stays up; the network is saved only if it connects.
        wifiManager.requestConnect(ssid.c_str(), pass.c_str());
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Connecting. Check the IP shown in Settings.\"}");
    });

    _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) {
            // _authenticate() already sent the 401/429 response itself
            // (including the WWW-Authenticate challenge or Retry-After
            // header) - sending a second response here would double-send
            // on the same AsyncWebServerRequest, which is undefined
            // behavior in ESPAsyncWebServer (can crash the async task or
            // corrupt the connection).
            return;
        }
        _handleAPIStatus(request);
    });

    // SD pin and per-category storage choice. Authenticated like all settings.
    // POST csPin=<gpio|-1>  or  category=<db|rec|prof|bak>&choice=<auto|internal|sd>
    //   or reset=1 (all choices back to auto).
    _server.on("/api/storage", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) return;
        bool ok = false;
        if (request->hasArg("reset")) {
            resetStorageChoices();
            ok = true;
        } else if (request->hasArg("csPin")) {
            char* end = nullptr;
            const String v = request->arg("csPin");
            const long pin = strtol(v.c_str(), &end, 10);
            ok = (end != v.c_str() && *end == '\0' && pin >= -1 && pin <= 48) &&
                 sdStorage.setCsPin((int)pin);
        } else if (request->hasArg("category") && request->hasArg("choice")) {
            const String ch = request->arg("choice");
            const uint8_t v = ch == "auto" ? CT_STORE_AUTO : ch == "internal" ? CT_STORE_INTERNAL :
                              ch == "sd" ? CT_STORE_SD : 255;
            ok = setStorageChoice(request->arg("category").c_str(), v);
        }
        request->send(ok ? 200 : 400, "application/json",
                      ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Invalid or rejected storage setting\"}");
    });

    // Checklist item 16 (error logging/telemetry) - read-only, same
    // auth level as everything else. See error_log.h.
    _server.on("/api/logs", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) {
            // _authenticate() already sent the 401/429 response itself
            // (including the WWW-Authenticate challenge or Retry-After
            // header) - sending a second response here would double-send
            // on the same AsyncWebServerRequest, which is undefined
            // behavior in ESPAsyncWebServer (can crash the async task or
            // corrupt the connection).
            return;
        }
        request->send(200, "application/json", getErrorLog()->toJSON());
    });

    _server.on("/api/recordings", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) return;
        const bool sdReady = sdStorage.state() == SdStorage::READY;
        File root = SPIFFS.open("/");
        if (!root && !sdReady) {
            request->send(503, "application/json", "{\"error\":\"No storage available\"}");
            return;
        }

        JsonDocument doc;
        JsonArray recordings = doc["recordings"].to<JsonArray>();
        uint8_t count = 0;
        bool truncated = false;
        // Internal flash first, then SD. Names are unique across both.
        for (uint8_t pass = 0; pass < 2; ++pass) {
            File dir = (pass == 0) ? root : (sdReady ? SD.open("/") : File());
            if (!dir) continue;
            const char* locName = pass == 0 ? "internal" : "sd";
            File entry = dir.openNextFile();
            uint16_t scanned = 0;
            while (entry && count < 50 && scanned < 200) {
                ++scanned;
                String name = entry.name();
                if (name.startsWith("/")) name.remove(0, 1);
                if (!entry.isDirectory() && ctCanRecordFilenameValid(name.c_str())) {
                    JsonObject item = recordings.add<JsonObject>();
                    item["name"] = name;
                    item["bytes"] = entry.size();
                    item["location"] = locName;
                    ++count;
                }
                entry.close();
                entry = dir.openNextFile();
            }
            if (entry) truncated = true;
            entry.close();
            dir.close();
        }
        doc["truncated"] = truncated;

        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    _server.on("/api/recordings/download", HTTP_GET,
        [this](AsyncWebServerRequest* request) {
            if (!_authenticate(request)) return;
            if (!request->hasParam("name")) {
                request->send(400, "application/json", "{\"error\":\"Recording name is required\"}");
                return;
            }
            const String name = request->getParam("name")->value();
            if (!ctCanRecordFilenameValid(name.c_str())) {
                request->send(400, "application/json", "{\"error\":\"Invalid recording name\"}");
                return;
            }
            const String path = String("/") + name;
            fs::FS* source = nullptr;
            if (SPIFFS.exists(path)) source = &SPIFFS;
            else if (sdStorage.state() == SdStorage::READY && SD.exists(path)) source = &SD;
            if (!source) {
                request->send(404, "application/json", "{\"error\":\"Recording not found\"}");
                return;
            }
            AsyncWebServerResponse* response = request->beginResponse(*source, path, "text/csv");
            response->addHeader("Content-Disposition", String("attachment; filename=\"") + name + "\"");
            request->send(response);
        });

    // -- OTA: firmware/filesystem update via browser (behind the same auth) --------
    _server.on("/update", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) return;
        request->send(200, "text/html; charset=utf-8", OTA_PAGE_HTML);
    });
    _server.on("/update", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            _handleOtaFinished(request);
        },
        [this](AsyncWebServerRequest* request, const String& filename,
               size_t index, uint8_t* data, size_t len, bool final) {
            _handleOtaUpload(request, filename, index, data, len, final);
        });

    // -- Custom profile management routes (all behind the same auth) -----------------
    _registerCustomVehicleRoutes();
    _registerDbcRoutes();

    _server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (SPIFFS.exists("/app.js")) {
            request->send(SPIFFS, "/app.js", "application/javascript");
        } else {
            request->send(404, "text/plain", "404 - Not Found");
        }
    });
    _server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (SPIFFS.exists("/style.css")) {
            request->send(SPIFFS, "/style.css", "text/css");
        } else {
            request->send(404, "text/plain", "404 - Not Found");
        }
    });

    _server.onNotFound([this](AsyncWebServerRequest* request) {
        _handleNotFound(request);
    });

    _server.begin();
    _started = true;

    Serial.printf("[WEB] Web server started: http://%s:%d (user: %s)\n",
                  WiFi.softAPIP().toString().c_str(),
                  port,
                  getConfig()->webUser);
    if (isUsingDefaultPassword()) {
        Serial.println("[WEB] Please change the default web password from Settings");
    }
}

void WebServerManager::_registerDbcRoutes() {
    _server.on("/api/dbc/list", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) return;

        CtDbcManifestEntry* entries = new (std::nothrow) CtDbcManifestEntry[DBC_LIST_LIMIT];
        CtStorageLoc* locations = new (std::nothrow) CtStorageLoc[DBC_LIST_LIMIT];
        if (!entries || !locations) {
            delete[] entries;
            delete[] locations;
            request->send(503, "application/json", "{\"error\":\"Not enough memory to list DBC files\"}");
            return;
        }
        size_t count = 0;
        bool truncated = false;
        if (!dbcStore.listFiles(entries, locations, DBC_LIST_LIMIT, count, truncated)) {
            delete[] entries;
            delete[] locations;
            request->send(500, "application/json",
                          "{\"error\":\"Could not read DBC manifest\"}");
            return;
        }

        JsonDocument doc;
        JsonArray files = doc["files"].to<JsonArray>();
        for (size_t i = 0; i < count; ++i) {
            JsonObject item = files.add<JsonObject>();
            item["name"] = entries[i].name;
            item["size"] = entries[i].size;
            item["sha256"] = entries[i].sha256;
            item["messages"] = entries[i].messages;
            item["time"] = entries[i].time;
            item["source"] = entries[i].source;
            item["license"] = entries[i].license;
            item["location"] = dbcLocationName(locations[i]);
            item["builtin"] = strcmp(entries[i].source, "user") != 0;
        }
        doc["truncated"] = truncated;
        String json;
        serializeJson(doc, json);
        delete[] entries;
        delete[] locations;
        request->send(200, "application/json", json);
    });

    _server.on("/api/dbc/upload", HTTP_POST,
        [this](AsyncWebServerRequest* request) {
            if (!_authenticate(request)) {
                DbcUploadRequestState* state =
                    static_cast<DbcUploadRequestState*>(request->_tempObject);
                if (state && state->started && !state->complete) dbcStore.abortUpload();
                request->_tempObject = nullptr;
                free(state);
                return;
            }
            DbcUploadRequestState* state =
                static_cast<DbcUploadRequestState*>(request->_tempObject);
            request->_tempObject = nullptr;
            if (!state) {
                request->send(400, "application/json",
                              "{\"error\":\"No DBC file was received\"}");
                return;
            }
            if (state->dataComplete && state->error[0] == '\0') {
                if (!dbcStore.finishUpload()) {
                    setDbcUploadError(*state, 400, dbcStore.errorText());
                } else {
                    state->location = dbcStore.uploadLocation();
                    state->complete = true;
                    state->httpStatus = 200;
                }
            }
            if (state->started && !state->complete) dbcStore.abortUpload();
            if (!state->complete) {
                JsonDocument doc;
                doc["ok"] = false;
                doc["error"] = state->error[0] ? state->error : "Incomplete DBC upload";
                String json;
                serializeJson(doc, json);
                request->send(state->httpStatus, "application/json", json);
                free(state);
                return;
            }
            JsonDocument doc;
            doc["ok"] = true;
            doc["name"] = state->name;
            doc["location"] = dbcLocationName(state->location);
            String json;
            serializeJson(doc, json);
            request->send(200, "application/json", json);
            free(state);
        },
        [this](AsyncWebServerRequest* request, const String& filename,
               size_t index, uint8_t* data, size_t len, bool final) {
            if (index == 0) {
                if (!_authenticate(request)) return;
                DbcUploadRequestState* existing =
                    static_cast<DbcUploadRequestState*>(request->_tempObject);
                if (existing) {
                    if (existing->started) dbcStore.abortUpload();
                    setDbcUploadError(*existing, 400, "Only one DBC file may be uploaded per request");
                    return;
                }
                DbcUploadRequestState* state =
                    static_cast<DbcUploadRequestState*>(calloc(1, sizeof(DbcUploadRequestState)));
                if (!state) return;
                state->httpStatus = 400;
                request->_tempObject = state;
                if (request->contentLength() == 0 ||
                    request->contentLength() > DBC_HTTP_BODY_LIMIT) {
                    setDbcUploadError(*state, 413, "DBC upload request exceeds the size limit");
                    return;
                }
                const uint32_t requestBytes = request->contentLength() > CT_DBC_MAX_BYTES
                    ? CT_DBC_MAX_BYTES : static_cast<uint32_t>(request->contentLength());
                if (!dbcStore.beginUploadStream(filename.c_str(), requestBytes)) {
                    setDbcUploadError(*state, 400, dbcStore.errorText());
                    return;
                }
                state->started = true;
                request->onDisconnect([request]() {
                    DbcUploadRequestState* upload =
                        static_cast<DbcUploadRequestState*>(request->_tempObject);
                    if (upload && upload->started && !upload->complete) dbcStore.abortUpload();
                });
                strncpy(state->name, filename.c_str(), sizeof(state->name) - 1);
                state->name[sizeof(state->name) - 1] = '\0';
            }

            DbcUploadRequestState* state =
                static_cast<DbcUploadRequestState*>(request->_tempObject);
            if (!state || state->error[0] != '\0') return;
            if (!state->started || (len && dbcStore.writeChunk(data, len) != len)) {
                dbcStore.abortUpload();
                setDbcUploadError(*state,
                                  dbcStore.status() == DBC_STORE_INVALID_SIZE ? 413 : 400,
                                  dbcStore.errorText());
                return;
            }
            if (final) state->dataComplete = true;
        });

    _server.on("/api/dbc/download", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) return;
        if (!request->hasParam("name") || !request->hasParam("location")) {
            request->send(400, "application/json",
                          "{\"error\":\"DBC name and location are required\"}");
            return;
        }
        const String name = request->getParam("name")->value();
        const String where = request->getParam("location")->value();
        const CtStorageLoc location = where == "sd" ? CT_LOC_SD :
                                      where == "internal" ? CT_LOC_INTERNAL : CT_LOC_NONE;
        if (!ctDbcNameValid(name.c_str()) || location == CT_LOC_NONE) {
            request->send(400, "application/json", "{\"error\":\"Invalid DBC name or location\"}");
            return;
        }

        bool builtin = false;
        if (!dbcStore.isBuiltinFile(name.c_str(), builtin)) {
            request->send(500, "application/json", "{\"error\":\"Could not read DBC manifest\"}");
            return;
        }
        if (builtin && location != CT_LOC_INTERNAL) {
            request->send(404, "application/json", "{\"error\":\"DBC file not found\"}");
            return;
        }
        if (!builtin && !dbcStore.verifyFile(name.c_str(), location)) {
            request->send(409, "application/json",
                          "{\"error\":\"DBC integrity check failed; export refused\"}");
            return;
        }

        fs::FS* source = location == CT_LOC_SD ? static_cast<fs::FS*>(&SD) :
                                                 static_cast<fs::FS*>(&SPIFFS);
        char path[sizeof(CT_DBC_DIR) + CT_DBC_NAME_MAX];
        if (!ctDbcBuildPath(name.c_str(), path, sizeof(path)) || !source->exists(path)) {
            request->send(404, "application/json", "{\"error\":\"DBC file not found\"}");
            return;
        }
        AsyncWebServerResponse* response =
            request->beginResponse(*source, path, "application/octet-stream");
        response->addHeader("Content-Disposition",
                            String("attachment; filename=\"") + name + "\"");
        request->send(response);
    });

    _server.on("/api/dbc/delete", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) return;
        if (!request->hasArg("name") || !request->hasArg("location")) {
            request->send(400, "application/json",
                          "{\"error\":\"DBC name and location are required\"}");
            return;
        }
        const String name = request->arg("name");
        const String where = request->arg("location");
        const CtStorageLoc location = where == "sd" ? CT_LOC_SD :
                                      where == "internal" ? CT_LOC_INTERNAL : CT_LOC_NONE;
        if (!ctDbcNameValid(name.c_str()) || location == CT_LOC_NONE) {
            request->send(400, "application/json", "{\"error\":\"Invalid DBC name or location\"}");
            return;
        }
        const bool confirmed = request->hasArg("confirm") && request->arg("confirm") == "1";
        bool referenced = false;
        if (_customStore && !_customStore->referencesDbcFile(name.c_str(), referenced)) {
            request->send(500, "application/json",
                          "{\"error\":\"Could not verify DBC profile references\"}");
            return;
        }
        const bool active = _profileManager &&
                            _profileManager->activeProfileUsesDbc(name.c_str());
        if (referenced || active) {
            request->send(409, "application/json",
                          "{\"error\":\"Unassign this DBC from every custom profile before deleting it\"}");
            return;
        }
        if (!dbcStore.deleteUserFile(name.c_str(), location, active, confirmed)) {
            JsonDocument doc;
            doc["ok"] = false;
            doc["error"] = dbcStore.errorText();
            String json;
            serializeJson(doc, json);
            request->send(dbcStore.status() == DBC_STORE_CONFIRMATION_REQUIRED ? 409 : 400,
                          "application/json", json);
            return;
        }
        request->send(200, "application/json", "{\"ok\":true}");
    });
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Custom vehicle profile REST routes
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WebServerManager::_registerCustomVehicleRoutes() {
    // GET /api/vehicles/custom - list custom profiles (summary)
    _server.on("/api/vehicles/custom", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) {
            // _authenticate() already sent the 401/429 response itself
            // (including the WWW-Authenticate challenge or Retry-After
            // header) - sending a second response here would double-send
            // on the same AsyncWebServerRequest, which is undefined
            // behavior in ESPAsyncWebServer (can crash the async task or
            // corrupt the connection).
            return;
        }
        if (!_customStore) {
            request->send(500, "application/json", "{\"error\":\"Custom profile module is not initialized\"}");
            return;
        }

        JsonDocument doc;
        JsonArray arr = doc["profiles"].to<JsonArray>();
        for (int i = 0; i < MAX_CUSTOM_VEHICLES; i++) {
            CustomVehicleProfile summary;
            if (_customStore->getProfileSummary(i, summary)) {
                JsonObject item = arr.add<JsonObject>();
                item["id"]             = summary.id;
                item["name"]              = summary.name;
                item["brand"]               = summary.brand;
                item["model"]                  = summary.model;
                item["year"]                     = summary.year;
                item["commandCount"]                = summary.commandCount;
                CustomVehicleProfile fullProfile;
                if (!_customStore->loadProfile((uint8_t)i, fullProfile)) {
                    request->send(500, "application/json",
                                  "{\"error\":\"Could not read custom profile\"}");
                    return;
                }
                item["dbcFile"] = fullProfile.dbcFileName;
            }
        }

        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    // POST /api/vehicles/custom/new - create a new empty profile
    // Form body: name, brand (optional), model (optional), year (optional)
    _server.on("/api/vehicles/custom/new", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) {
            // _authenticate() already sent the 401/429 response itself
            // (including the WWW-Authenticate challenge or Retry-After
            // header) - sending a second response here would double-send
            // on the same AsyncWebServerRequest, which is undefined
            // behavior in ESPAsyncWebServer (can crash the async task or
            // corrupt the connection).
            return;
        }
        if (!_customStore) {
            request->send(500, "application/json", "{\"error\":\"Custom profile module is not initialized\"}");
            return;
        }

        String   name  = request->arg("name");
        String   brand = request->arg("brand");
        String   model = request->arg("model");
        uint16_t year  = request->hasArg("year") ? request->arg("year").toInt() : 0;

        if (name.length() == 0) {
            request->send(400, "application/json", "{\"error\":\"Profile name is required\"}");
            return;
        }

        uint8_t newIndex;
        bool ok = _customStore->createNewProfile(name.c_str(), brand.c_str(),
                                                   model.c_str(), year, newIndex);
        if (ok) {
            String json = "{\"success\":true,\"id\":" + String(newIndex) + "}";
            request->send(200, "application/json", json);
        } else {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Profile capacity is full\"}");
        }
    });

    // POST /api/vehicles/custom/manual-add - add a manual command (see CarTouch_SPEC.md)
    // Form body: profileId, label, displayName, canId (hex string like "1A0"),
    //            extended ("1"/"0"), dataHex (e.g. "01 FF 00"),
    //            optional actuatorClass: 1=none, 2=window, 3=sunroof, 4=mirror
    _server.on("/api/vehicles/custom/manual-add", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) {
            // _authenticate() already sent the 401/429 response itself
            // (including the WWW-Authenticate challenge or Retry-After
            // header) - sending a second response here would double-send
            // on the same AsyncWebServerRequest, which is undefined
            // behavior in ESPAsyncWebServer (can crash the async task or
            // corrupt the connection).
            return;
        }
        if (!_customStore) {
            request->send(500, "application/json", "{\"error\":\"Custom profile module is not initialized\"}");
            return;
        }

        if (!request->hasArg("profileId") || !request->hasArg("label") ||
            !request->hasArg("canId") || !request->hasArg("dataHex")) {
            request->send(400, "application/json", "{\"error\":\"Required fields are missing\"}");
            return;
        }

        uint8_t profileId;
        if (!parseHttpProfileIndex(request->arg("profileId"), profileId)) {
            request->send(400, "application/json", "{\"error\":\"Profile ID is invalid\"}");
            return;
        }
        String  label       = request->arg("label");
        String  displayName = request->hasArg("displayName") ? request->arg("displayName") : label;
        String  canIdHex    = request->arg("canId");
        bool    extended    = request->hasArg("extended") && request->arg("extended") == "1";
        String  dataHex     = request->arg("dataHex");

        // Parse CAN ID strictly; reject malformed text instead of accepting
        // only a fully valid hexadecimal token is accepted.
        uint32_t canId = 0;
        uint32_t maxId = extended ? 0x1FFFFFFF : 0x7FF;
        if (!parseHexUint32(canIdHex.c_str(), canId, 8) || canId > maxId) {
            request->send(400, "application/json",
                "{\"error\":\"CAN ID is invalid or out of range\"}");
            return;
        }

        // Parse data bytes (space-separated hex, e.g. "01 FF 00")
        LearnedCommand cmd;
        if (label.length() == 0 || label.length() >= sizeof(cmd.label) ||
            displayName.length() >= sizeof(cmd.displayName)) {
            request->send(400, "application/json",
                "{\"error\":\"Command label length is invalid\"}");
            return;
        }
        if (!ctLabelIsSafe(label.c_str())) {
            request->send(400, "application/json",
                "{\"error\":\"Command label may only contain letters, digits, space, _ - .\"}");
            return;
        }
        strncpy(cmd.label, label.c_str(), sizeof(cmd.label) - 1);
        cmd.label[sizeof(cmd.label) - 1] = '\0';
        strncpy(cmd.displayName, displayName.c_str(), sizeof(cmd.displayName) - 1);
        cmd.displayName[sizeof(cmd.displayName) - 1] = '\0';
        cmd.canId          = canId;
        cmd.isExtended       = extended;
        cmd.source              = SOURCE_MANUAL;
        if (request->hasArg("actuatorClass")) {
            int av = request->arg("actuatorClass").toInt();
            if (av < (int)COMMAND_ACTUATOR_NONE || av > (int)COMMAND_ACTUATOR_MIRROR) {
                request->send(400, "application/json", "{\"error\":\"Invalid actuatorClass\"}");
                return;
            }
            cmd.actuatorClass = (CommandActuatorClass)av;
        } else {
            cmd.actuatorClass = ctSuggestedActuatorClassForStandardLabel(cmd.label);
            if (cmd.actuatorClass == COMMAND_ACTUATOR_UNKNOWN) {
                request->send(400, "application/json", "{\"error\":\"actuatorClass is required for custom labels\"}");
                return;
            }
        }
        cmd.status                 = CMD_UNVERIFIED;  // Always starts unverified
        cmd.timesObserved             = 0;
        cmd.failCount                    = 0;
        cmd.createdAt                       = millis();

        uint8_t len = 0;
        char buf[64];
        strncpy(buf, dataHex.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char* token = strtok(buf, " \t\r\n");
        while (token) {
            if (len >= 8 || !parseHexByteToken(token, cmd.data[len])) {
                request->send(400, "application/json",
                    "{\"error\":\"CAN data must contain 1 to 8 valid hexadecimal bytes\"}");
                return;
            }
            ++len;
            token = strtok(nullptr, " \t\r\n");
        }
        if (len == 0) {
            request->send(400, "application/json",
                "{\"error\":\"Enter at least one data byte\"}");
            return;
        }
        cmd.length = len;

        bool ok = _customStore->upsertCommand(profileId, cmd);
        if (ok) {
            request->send(200, "application/json", "{\"success\":true}");
        } else {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Save failed\"}");
        }
    });

    // GET /api/vehicles/custom/export?id=N - download a profile as JSON
    _server.on("/api/vehicles/custom/export", HTTP_GET, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) {
            // _authenticate() already sent the 401/429 response itself
            // (including the WWW-Authenticate challenge or Retry-After
            // header) - sending a second response here would double-send
            // on the same AsyncWebServerRequest, which is undefined
            // behavior in ESPAsyncWebServer (can crash the async task or
            // corrupt the connection).
            return;
        }
        if (!_customStore || !request->hasArg("id")) {
            request->send(400, "application/json", "{\"error\":\"Profile ID is required\"}");
            return;
        }

        uint8_t id;
        if (!parseHttpProfileIndex(request->arg("id"), id)) {
            request->send(400, "application/json", "{\"error\":\"Profile ID is invalid\"}");
            return;
        }
        String json;
        if (!_customStore->exportProfileJSON(id, json)) {
            request->send(404, "application/json", "{\"error\":\"Profile not found\"}");
            return;
        }

        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Content-Disposition", "attachment; filename=cartouch_profile.json");
        request->send(response);
    });

    // POST /api/vehicles/custom/import - upload a profile JSON
    // Body: raw JSON in the "json" arg (urlencoded form) - kept simple
    // on ESPAsyncWebServer without needing multipart/file upload.
    _server.on("/api/vehicles/custom/import", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) {
            // _authenticate() already sent the 401/429 response itself
            // (including the WWW-Authenticate challenge or Retry-After
            // header) - sending a second response here would double-send
            // on the same AsyncWebServerRequest, which is undefined
            // behavior in ESPAsyncWebServer (can crash the async task or
            // corrupt the connection).
            return;
        }
        if (!_customStore || !request->hasArg("json")) {
            request->send(400, "application/json", "{\"error\":\"JSON content is required\"}");
            return;
        }

        if (request->arg("json").length() > MAX_IMPORT_JSON_LEN) {
            request->send(413, "application/json",
                          "{\"error\":\"Profile JSON exceeds the maximum accepted size\"}");
            return;
        }

        String jsonBody = request->arg("json");
        JsonDocument importedProfile;
        DeserializationError importError = deserializeJson(importedProfile, jsonBody);
        if (!importError && importedProfile["dbcFile"].is<const char*>()) {
            const char* dbcName = importedProfile["dbcFile"].as<const char*>();
            if (dbcName[0]) {
                File verified;
                CtStorageLoc location = CT_LOC_NONE;
                bool isUserFile = false;
                if (!ctDbcNameValid(dbcName) ||
                    !dbcStore.openVerifiedUserFile(dbcName, verified, location, isUserFile) ||
                    !isUserFile) {
                    request->send(409, "application/json",
                                  "{\"error\":\"Imported profile references a DBC that is unavailable or failed integrity validation\"}");
                    return;
                }
                verified.close();
            }
        }
        uint8_t newIndex;
        bool ok = _customStore->importProfileJSON(jsonBody, newIndex);
        if (ok) {
            String resp = "{\"success\":true,\"id\":" + String(newIndex) +
                         ",\"note\":\"All imported commands were marked unverified\"}";
            request->send(200, "application/json", resp);
        } else {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid file or profile capacity is full\"}");
        }
    });

    // POST /api/vehicles/custom/delete - delete a profile
    _server.on("/api/vehicles/custom/delete", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) {
            // _authenticate() already sent the 401/429 response itself
            // (including the WWW-Authenticate challenge or Retry-After
            // header) - sending a second response here would double-send
            // on the same AsyncWebServerRequest, which is undefined
            // behavior in ESPAsyncWebServer (can crash the async task or
            // corrupt the connection).
            return;
        }
        if (!_customStore || !request->hasArg("id")) {
            request->send(400, "application/json", "{\"error\":\"Profile ID is required\"}");
            return;
        }

        uint8_t id;
        if (!parseHttpProfileIndex(request->arg("id"), id)) {
            request->send(400, "application/json", "{\"error\":\"Profile ID is invalid\"}");
            return;
        }
        invalidatePendingVerificationForProfile(id);
        bool    ok = _customStore->deleteProfile(id);
        if (ok && _profileManager) {
            // If the deleted profile happened to be the active vehicle, clear
            // the selection so a dangling index can never be resolved later.
            _profileManager->clearActiveIfCustom(id);
        }
        request->send(ok ? 200 : 400, "application/json",
                      ok ? "{\"success\":true}" : "{\"success\":false}");
    });

    // POST /api/vehicles/custom/select - select a custom profile as the active vehicle
    _server.on("/api/vehicles/custom/select", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) {
            // _authenticate() already sent the 401/429 response itself
            // (including the WWW-Authenticate challenge or Retry-After
            // header) - sending a second response here would double-send
            // on the same AsyncWebServerRequest, which is undefined
            // behavior in ESPAsyncWebServer (can crash the async task or
            // corrupt the connection).
            return;
        }
        if (!_profileManager || !request->hasArg("id")) {
            request->send(400, "application/json", "{\"error\":\"Profile ID is required\"}");
            return;
        }

        uint8_t id;
        if (!parseHttpProfileIndex(request->arg("id"), id)) {
            request->send(400, "application/json", "{\"error\":\"Profile ID is invalid\"}");
            return;
        }
        bool    ok = _profileManager->selectCustomVehicle(id);
        request->send(ok ? 200 : 404, "application/json",
                      ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"Profile not found\"}");
    });

    // POST /api/vehicles/custom/set-dbc - bind or clear a verified user DBC.
    _server.on("/api/vehicles/custom/set-dbc", HTTP_POST, [this](AsyncWebServerRequest* request) {
        if (!_authenticate(request)) return;
        if (!_customStore || !_profileManager ||
            !request->hasArg("id") || !request->hasArg("dbcFile")) {
            request->send(400, "application/json",
                          "{\"error\":\"Profile ID and dbcFile are required\"}");
            return;
        }
        uint8_t id;
        if (!parseHttpProfileIndex(request->arg("id"), id)) {
            request->send(400, "application/json", "{\"error\":\"Profile ID is invalid\"}");
            return;
        }

        const String dbcName = request->arg("dbcFile");
        if (dbcName.length() && !ctDbcNameValid(dbcName.c_str())) {
            request->send(400, "application/json", "{\"error\":\"DBC file name is invalid\"}");
            return;
        }
        if (dbcName.length()) {
            File verified;
            CtStorageLoc location = CT_LOC_NONE;
            bool isUserFile = false;
            if (!dbcStore.openVerifiedUserFile(dbcName.c_str(), verified, location, isUserFile) ||
                !isUserFile) {
                request->send(409, "application/json",
                              "{\"error\":\"DBC file is unavailable or failed integrity validation\"}");
                return;
            }
            verified.close();
        }

        CustomVehicleProfile previous;
        if (!_customStore->loadProfile(id, previous)) {
            request->send(404, "application/json", "{\"error\":\"Custom profile not found\"}");
            return;
        }
        const bool active = _profileManager->getActiveKind() == ACTIVE_KIND_CUSTOM &&
                            _profileManager->getActiveCustomIndex() == id;
        if (!_customStore->setDbcFileName(id, dbcName.c_str())) {
            request->send(500, "application/json", "{\"error\":\"Could not save DBC profile reference\"}");
            return;
        }
        if (active && !_profileManager->selectCustomVehicle(id)) {
            const bool referenceRestored =
                _customStore->setDbcFileName(id, previous.dbcFileName);
            const bool profileRestored =
                referenceRestored && _profileManager->selectCustomVehicle(id);
            if (!profileRestored) {
                Serial.printf("[WebServer] ERROR: failed to restore active custom profile %u after DBC load failure\n",
                              id);
                request->send(500, "application/json",
                              "{\"error\":\"DBC load failed and previous profile could not be restored; reselect the profile\"}");
                return;
            }
            request->send(409, "application/json",
                          "{\"error\":\"DBC could not be loaded; previous profile reference restored\"}");
            return;
        }
        request->send(200, "application/json", "{\"success\":true}");
    });
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ update()
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WebServerManager::update() {
    // Periodic cleanup of disconnected clients is optional - AsyncWebSocket
    // already fires WS_EVT_DISCONNECT, which we handle there.

    // Reboot after a successful OTA (a short delay lets the HTTP response
    // reach the browser first)
    if (_rebootPending && (int32_t)(millis() - _rebootAt) >= 0) {
        Serial.println("[OTA] Rebooting to apply update...");
        delay(100);
        ESP.restart();
    }

    if (_canService && (_monitorSubscribed[0] || _monitorSubscribed[1]) &&
        (uint32_t)(millis() - _lastCanMonitorSend) >= 50) {
        _lastCanMonitorSend = millis();
        static uint8_t nextBus = 0;
        for (uint8_t sent = 0; sent < 8; ++sent) {
            bool found = false;
            for (uint8_t attempt = 0; attempt < 2; ++attempt) {
                const uint8_t bus = (uint8_t)((nextBus + attempt) % 2);
                if (!_monitorSubscribed[bus]) continue;
                CanRxFrame frame;
                if (_canService->receiveRx((CanBusId)bus, CAN_RX_MONITOR, frame)) {
                    _sendCanFrameToMonitorClients(frame);
                    nextBus = (uint8_t)(1 - bus);
                    found = true;
                    break;
                }
            }
            if (!found) break;
        }
    }
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ OTA: file upload
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

static CtOtaHeaderCheck gOtaHeader;    // one upload at a time

void WebServerManager::_handleOtaUpload(AsyncWebServerRequest* request, const String& filename,
                                        size_t index, uint8_t* data, size_t len, bool final) {
    // Use the same central authentication, lockout and session policy as every
    // other authenticated endpoint. Do not bypass it with raw Basic Auth.
    if (!_authenticate(request)) return;

    if (index == 0) {
        if (!ctOtaLock().tryAcquire(CT_OTA_OWNER_WEB)) {
            _otaError = "Another OTA transaction is already in progress";
            return;
        }
        _otaError = "";
        _otaBytes = 0;
        gOtaHeader.reset();
        gOtaExpectedSha256[0] = '\0';
        gOtaSignatureHex[0]   = '\0';
        ctSha256Init(gOtaSha256);
        _otaIsFs  = request->hasParam("type") && request->getParam("type")->value() == "fs";

        if (!request->hasParam("sha256", false)) {
            _otaError = "SHA-256 query parameter is required";
            return;
        }
        const String expectedHash = request->getParam("sha256", false)->value();
        if (!ctSha256HexValid(expectedHash.c_str())) {
            _otaError = "SHA-256 must contain exactly 64 hexadecimal characters";
            return;
        }
        strncpy(gOtaExpectedSha256, expectedHash.c_str(), sizeof(gOtaExpectedSha256) - 1);
        gOtaExpectedSha256[sizeof(gOtaExpectedSha256) - 1] = '\0';

        // Optional release signature (hex, DER). Only product mode requires it;
        // an over-long value is dropped, never truncated into a different one.
        if (request->hasParam("sig", false)) {
            const String sig = request->getParam("sig", false)->value();
            if (sig.length() <= CT_OTA_SIG_HEX_MAX) {
                strncpy(gOtaSignatureHex, sig.c_str(), sizeof(gOtaSignatureHex) - 1);
                gOtaSignatureHex[sizeof(gOtaSignatureHex) - 1] = '\0';
            }
        }

        if (!ctPartitionFitsFlash(ESP.getFlashChipSize(), CT_REQUIRED_FLASH_BYTES)) {
            _otaError = "Update blocked: detected flash is smaller than the configured partition layout.";
            return;
        }

        if (_otaIsFs && !canReplaceFilesystemWithoutUserData()) {
            _otaError = "Filesystem update blocked: SPIFFS is unavailable or contains custom profiles, CAN recordings, or user DBC data/recovery files. Export profiles and user DBCs and back up recordings elsewhere before removing protected data and retrying.";
            return;
        }

        String lower = filename;
        lower.toLowerCase();
        if (!lower.endsWith(".bin")) {
            _otaError = "File must use the .bin extension";
            return;
        }
        // Guard against mixing up firmware/filesystem/bootloader files
        bool looksBootloader = lower.indexOf("bootloader") >= 0 || lower.indexOf("partitions") >= 0;
        if (_otaIsFs) {
            if (looksBootloader || lower.indexOf("firmware") >= 0) {
                _otaError = "This is not a filesystem image (select spiffs.bin)";
                return;
            }
        } else {
            if (looksBootloader || lower.indexOf("spiffs") >= 0) {
                _otaError = "This is not a firmware image (select firmware.bin)";
                return;
            }
        }

        if (Update.isRunning()) { _otaError = "Another update transaction is active"; ctOtaLock().release(CT_OTA_OWNER_WEB); return; }
        if (_otaIsFs) SPIFFS.end();  // Before writing to the filesystem partition

        if (!Update.begin(UPDATE_SIZE_UNKNOWN, _otaIsFs ? U_SPIFFS : U_FLASH)) {
            _otaError = String("Failed to start update: ") + Update.errorString();
            ctOtaLock().release(CT_OTA_OWNER_WEB);
            return;
        }
        Serial.printf("[OTA] Starting: %s (%s)\n", filename.c_str(), _otaIsFs ? "filesystem" : "firmware");
    }

    if (_otaError.length() > 0) return;

    // The first 24 bytes of a firmware image are the ESP image header: it
    // must be for the ESP32-S3 and for no more flash than this device has.
    // Chunks are collected until the header is complete (nothing in the
    // running slot is touched; the update goes to the inactive slot).
    if (!_otaIsFs && !gOtaHeader.done) {
        const CtOtaHdrResult hr = ctOtaHeaderFeed(gOtaHeader, data, len, ESP.getFlashChipSize());
        if (hr != CT_OTA_HDR_OK && hr != CT_OTA_HDR_NEED_MORE) {
            _otaError = ctOtaHeaderMessage(hr);
            if (Update.isRunning()) Update.abort();
            return;
        }
    }

    if (len > 0) {
        if (Update.write(data, len) != len) {
            _otaError = String("Write error: ") + Update.errorString();
            Update.abort();
            return;
        }
        ctSha256Update(gOtaSha256, data, len);
        _otaBytes += len;
    }

    if (final) {
        if (!_otaIsFs && !gOtaHeader.done) {
            _otaError = "Firmware image is too short";
            if (Update.isRunning()) Update.abort();
            return;
        }
        char actualHash[65];
        ctSha256FinishHex(gOtaSha256, actualHash);
        if (!ctSha256HexEqual(actualHash, gOtaExpectedSha256)) {
            _otaError = "SHA-256 mismatch: uploaded image does not match the supplied digest";
            if (Update.isRunning()) Update.abort();
            return;
        }
        // Personal mode: always true (legacy SHA-256 + header behaviour).
        // Product mode: a valid release signature is required before Update.end().
        if (!ctOtaVerifyAuthenticity(CT_PRODUCT_MODE != 0, actualHash,
                                     gOtaSignatureHex, ctOtaVerifySignature)) {
            _otaError = "Signature missing or invalid: this build only accepts signed firmware";
            if (Update.isRunning()) Update.abort();
            return;
        }
        if (!Update.end(true)) {
            _otaError = String("Update finalization failed: ") + Update.errorString();
        } else {
            Serial.printf("[OTA] Finished successfully: %u bytes\n", (unsigned)_otaBytes);
        }
        ctOtaLock().release(CT_OTA_OWNER_WEB);
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ OTA: final response
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WebServerManager::_handleOtaFinished(AsyncWebServerRequest* request) {
    if (!_authenticate(request)) return;

    bool ok = (_otaError.length() == 0 && _otaBytes > 0 && !Update.hasError());

    if (ok) {
        _rebootPending = true;
        _rebootAt = millis() + 2000;
        request->send(200, "application/json",
            "{\"ok\":true,\"msg\":\"Installed. The device will reboot shortly; reconnect to the CarTouch Wi-Fi after about 15 seconds.\"}");
    } else {
        String err = _otaError;
        if (err.length() == 0) {
            err = (_otaBytes == 0) ? "No file received" : "Unknown error";
        }
        if (Update.isRunning()) Update.abort();
        if (_otaIsFs && !SPIFFS.begin(false)) {
            Serial.println("[OTA] ERROR: SPIFFS remount failed after unsuccessful filesystem update");
        }
        Serial.printf("[OTA] Failed: %s\n", err.c_str());
        JsonDocument errDoc;
        errDoc["ok"]  = false;
        errDoc["msg"] = err;
        String errJson;
        serializeJson(errDoc, errJson);
        request->send(400, "application/json", errJson);
    }

    _otaError = "";
    _otaBytes = 0;
    gOtaExpectedSha256[0] = '\0';
    ctOtaLock().release(CT_OTA_OWNER_WEB);
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Command callback
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WebServerManager::setCommandCallback(WebCommandCallback cb) {
    _commandCallback = cb;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Learn Mode state -> JSON
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

String WebServerManager::_learnStateToJSON() {
    if (!_learnEngine) return "{\"type\":\"learn_state\",\"state\":\"unavailable\"}";

    LearnEngineSnapshot snapshot;
    if (!_learnEngine->getSnapshot(snapshot)) {
        return "{\"type\":\"learn_state\",\"state\":\"error\"}";
    }

    JsonDocument doc;
    doc["type"] = "learn_state";

    LearnModeState state    = snapshot.state;
    const char*    stateStr = "idle";
    switch (state) {
        case LEARN_IDLE:              stateStr = "idle";               break;
        case LEARN_BASELINE_CAPTURE:  stateStr = "baseline_capture";   break;
        case LEARN_WAITING_ACTION:    stateStr = "waiting_action";     break;
        case LEARN_ACTION_CAPTURE:    stateStr = "action_capture";     break;
        case LEARN_CANDIDATES_READY:  stateStr = "candidates_ready";   break;
        case LEARN_ERROR:             stateStr = "error";              break;
    }
    doc["state"]         = stateStr;
    doc["progress"]         = snapshot.progressPercent;
    doc["label"]                = snapshot.label;
    doc["displayName"]              = snapshot.displayName;

    if (state == LEARN_CANDIDATES_READY) {
        JsonArray candidates = doc["candidates"].to<JsonArray>();
        for (int i = 0; i < snapshot.candidateCount; i++) {
            const LearnCandidate& c = snapshot.candidates[i];

            JsonObject item = candidates.add<JsonObject>();
            item["index"] = i;
            item["canId"] = c.canId;
            item["isExtended"] = c.isExtended;

            char hexId[12];
            snprintf(hexId, sizeof(hexId), c.isExtended ? "0x%08lX" : "0x%03lX",
                     (unsigned long)c.canId);
            item["canIdHex"] = hexId;

            JsonArray dataArr = item["data"].to<JsonArray>();
            for (int b = 0; b < c.length; b++) dataArr.add(c.data[b]);

            item["length"]      = c.length;
            item["isNew"]          = c.isNewMessage;
            item["seenCount"]         = c.seenCountInAction;
        }
    }

    String json;
    serializeJson(doc, json);
    return json;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Learn Mode WebSocket messages
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// Only called from _handleWebSocketEvent after auth->authenticated is
// confirmed - the same access level as regular "command" messages.

void WebServerManager::_handleLearnModeMessage(AsyncWebSocketClient* client, JsonDocument& doc, const char* type) {
    if (!_learnEngine || !_customStore || !_profileManager || !_vehicleControl) {
        client->printf("{\"type\":\"learn_error\",\"message\":\"Learn Mode modules are not initialized\"}");
        return;
    }

    if (strcmp(type, "learn_start") == 0) {
        const char* label       = doc["label"] | "";
        const char* displayName = doc["displayName"] | label;
        if (strlen(label) == 0) {
            client->printf("{\"type\":\"learn_error\",\"message\":\"Command label is required\"}");
            return;
        }
        if (!ctLabelIsSafe(label)) {
            client->printf("{\"type\":\"learn_error\",\"message\":\"Command label may only contain letters, digits, space, _ - .\"}");
            return;
        }
        uint8_t profileId;
        if (!parseJsonProfileIndex(doc["profileId"], profileId)) {
            client->printf("{\"type\":\"learn_error\",\"message\":\"Target profile is invalid\"}");
            return;
        }
        CustomVehicleProfile targetProfile;
        if (!_customStore->getProfileSummary(profileId, targetProfile)) {
            client->printf("{\"type\":\"learn_error\",\"message\":\"Target profile is invalid\"}");
            return;
        }
        _learnEngine->beginLearning(label, displayName, profileId);
        client->text(_learnStateToJSON());

    } else if (strcmp(type, "learn_capture_baseline") == 0) {
        _learnEngine->startBaselineCapture();
        client->text(_learnStateToJSON());

    } else if (strcmp(type, "learn_confirm_action") == 0) {
        // User signals "pressing the button now" - starts the action capture window
        _learnEngine->confirmReadyForAction();
        client->text(_learnStateToJSON());

    } else if (strcmp(type, "learn_get_state") == 0) {
        // For polling progress (baseline/action capture is time-based)
        client->text(_learnStateToJSON());

    } else if (strcmp(type, "learn_save") == 0) {
        uint8_t candidateIndex;
        uint8_t profileId;
        if (!parseJsonBoundedIndex(doc["candidateIndex"], CANDIDATE_MAX, candidateIndex) ||
            !parseJsonProfileIndex(doc["profileId"], profileId)) {
            client->printf("{\"type\":\"learn_error\",\"message\":\"Invalid profile or candidate index\"}");
            return;
        }

        // A profile may only be created from a *completed* learn cycle.
        // getCandidate() already fails outside CANDIDATES_READY because the
        // candidate count is reset to 0 there, but checking the state
        // explicitly makes the invariant impossible to break silently in a
        // future refactor.
        LearnEngineSnapshot snapshot;
        if (!_learnEngine->getSnapshot(snapshot) ||
            snapshot.state != LEARN_CANDIDATES_READY) {
            client->printf("{\"type\":\"learn_error\",\"message\":\"no completed learn cycle\"}");
            return;
        }

        if (profileId != snapshot.targetProfileId) {
            client->printf("{\"type\":\"learn_error\",\"message\":\"Saved profile does not match the learning session profile\"}");
            return;
        }

        CustomVehicleProfile targetProfile;
        if (!_customStore->getProfileSummary(profileId, targetProfile)) {
            client->printf("{\"type\":\"learn_error\",\"message\":\"Target profile no longer exists\"}");
            return;
        }

        if (candidateIndex >= snapshot.candidateCount) {
            client->printf("{\"type\":\"learn_error\",\"message\":\"Invalid candidate\"}");
            return;
        }
        const LearnCandidate& candidate = snapshot.candidates[candidateIndex];

        LearnedCommand cmd;
        strncpy(cmd.label, snapshot.label, sizeof(cmd.label) - 1);
        cmd.label[sizeof(cmd.label) - 1] = '\0';  // strncpy may not NUL-terminate on truncation
        strncpy(cmd.displayName, snapshot.displayName, sizeof(cmd.displayName) - 1);
        cmd.displayName[sizeof(cmd.displayName) - 1] = '\0';

        // The label is the command's unique key inside the profile
        // store and the payload must fit the fixed 8-byte frame, so
        // reject both before anything is written to the store.
        if (cmd.label[0] == '\0' || candidate.length == 0 ||
            candidate.length > sizeof(cmd.data)) {
            client->printf("{\"type\":\"learn_error\",\"message\":\"Invalid candidate\"}");
            return;
        }

        cmd.canId          = candidate.canId;
        cmd.isExtended       = candidate.isExtended;
        cmd.length              = candidate.length;
        memcpy(cmd.data, candidate.data, candidate.length);
        cmd.source                 = SOURCE_LEARNED;
        cmd.actuatorClass         = ctSuggestedActuatorClassForStandardLabel(cmd.label);
        cmd.status                    = CMD_UNVERIFIED;  // Always starts unverified
        cmd.timesObserved                = candidate.seenCountInAction;
        cmd.failCount                       = 0;
        cmd.createdAt                          = millis();

        bool ok = _customStore->upsertCommand(profileId, cmd);
        if (ok) {
            JsonDocument savedDoc;
            savedDoc["type"] = "learn_saved";
            savedDoc["success"] = true;
            savedDoc["label"] = cmd.label;
            String savedJson;
            serializeJson(savedDoc, savedJson);
            client->text(savedJson);
            _learnEngine->cancelIfSession(snapshot.sessionId);
        } else {
            client->printf("{\"type\":\"learn_saved\",\"success\":false}");
        }

    } else if (strcmp(type, "learn_cancel") == 0) {
        _learnEngine->cancel();
        client->text(_learnStateToJSON());

    } else if (strcmp(type, "verify_command") == 0) {
        // Verification step (see README.md): the user wants to
        // one-shot test an UNVERIFIED command. executeCommandForVerification()
        // is the only entry point that lets an UNVERIFIED command
        // through - regular command dispatch always rejects it.
        uint8_t profileId;
        if (!parseJsonProfileIndex(doc["vehicleId"], profileId)) {
            client->printf("{\"type\":\"verify_error\",\"message\":\"Command profile is invalid\"}");
            return;
        }
        const char* label     = doc["label"] | "";

        LearnedCommand cmd;
        CustomVehicleProfile cmdProfile;
        if (!_customStore->findCommand(profileId, label, cmd) ||
            !_customStore->getProfileSummary(profileId, cmdProfile)) {
            client->printf("{\"type\":\"verify_error\",\"message\":\"Command not found\"}");
            return;
        }

        // Real test send - goes through the same rate-limit/duty-cycle/
        // Listen-Only checks as any other command, via the dedicated
        // verification entry point. Never continue with the previously
        // active profile if the requested profile cannot be selected.
        String errReason;
        bool sent = _vehicleControl->executeCommandForVerification(profileId, label, errReason);

        // Open a verification transaction ONLY for a send that actually
        // happened. verify_confirm is then required to present this
        // transaction's token, from the same client, for the same profile
        // and label - so an arbitrary or replayed confirmation cannot
        // change a command's status.
        uint32_t token = 0;
        WsClientAuth* vAuth = _findClientAuth(client->id());
        if (vAuth) {
            if (sent) {
                token = esp_random();
                vAuth->hasPendingVerify       = true;
                vAuth->pendingVerifyProfileId = profileId;
                memset(vAuth->pendingVerifyLabel, 0, sizeof(vAuth->pendingVerifyLabel));
                strncpy(vAuth->pendingVerifyLabel, label,
                        sizeof(vAuth->pendingVerifyLabel) - 1);
                vAuth->pendingVerifyToken     = token;
                vAuth->pendingVerifyAt        = millis();
                vAuth->pendingVerifyFingerprint = ctVerifyFingerprint(
                    profileId, cmdProfile.revision, cmd.label, cmd.canId, cmd.isExtended, cmd.length, cmd.data);
            } else {
                // A failed send invalidates any earlier pending transaction.
                vAuth->hasPendingVerify = false;
                vAuth->pendingVerifyFingerprint = 0;
            }
        }

        JsonDocument respDoc;
        respDoc["type"]    = "verify_sent";
        respDoc["success"]    = sent;
        respDoc["canId"]         = cmd.canId;
        char hexId[12];
        snprintf(hexId, sizeof(hexId), "0x%03X", cmd.canId);
        respDoc["canIdHex"] = hexId;
        if (sent) respDoc["verifyToken"] = token;
        if (!sent) respDoc["error"] = errReason;

        String resp;
        serializeJson(respDoc, resp);
        client->text(resp);

    } else if (strcmp(type, "verify_confirm") == 0) {
        // After verify_command, the user manually confirms whether the
        // vehicle reacted correctly. Accepted ONLY when it matches the
        // verification transaction verify_command opened for THIS
        // client: same profile, same label, matching token, and within
        // VERIFY_PENDING_TIMEOUT_MS. Consumed on use, so it cannot be
        // replayed, and an arbitrary confirmation is rejected.
        uint8_t profileId;
        if (!parseJsonProfileIndex(doc["vehicleId"], profileId)) {
            client->printf("{\"type\":\"verify_confirmed\",\"success\":false,\"message\":\"Command profile is invalid\"}");
            return;
        }
        const char* label   = doc["label"] | "";
        bool        success = doc["success"] | false;
        uint32_t    token   = doc["verifyToken"] | 0;

        WsClientAuth* vAuth = _findClientAuth(client->id());
        if (!vAuth) {
            client->printf("{\"type\":\"verify_confirmed\",\"success\":false,"
                           "\"message\":\"\u0646\u0634\u0633\u062a \u0646\u0627\u0645\u0639\u062a\u0628\u0631 \u0627\u0633\u062a\"}");
            return;
        }

        if (!ctVerifyTransactionValid(
                vAuth->hasPendingVerify, vAuth->pendingVerifyProfileId,
                vAuth->pendingVerifyLabel, vAuth->pendingVerifyToken,
                vAuth->pendingVerifyAt, profileId, label, token, millis(),
                VERIFY_PENDING_TIMEOUT_MS)) {
            vAuth->hasPendingVerify = false;
            vAuth->pendingVerifyFingerprint = 0;
            client->printf("{\"type\":\"verify_confirmed\",\"success\":false,\"message\":\"Verification is invalid or expired\"}");
            return;
        }

        // Re-read the command at confirmation time. This closes the residual
        // race where a command could be edited/relearned/deleted after the
        // test-send but before confirmation. A confirmation is valid only
        // for the exact payload that was actually sent and only while the
        // command remains UNVERIFIED in the same profile.
        LearnedCommand currentCmd;
        CustomVehicleProfile currentProfile;
        if (!_customStore->findCommand(profileId, label, currentCmd) ||
            !_customStore->getProfileSummary(profileId, currentProfile) ||
            currentCmd.status != CMD_UNVERIFIED ||
            ctVerifyFingerprint(profileId, currentProfile.revision, currentCmd.label, currentCmd.canId,
                                currentCmd.isExtended, currentCmd.length, currentCmd.data) !=
                vAuth->pendingVerifyFingerprint) {
            vAuth->hasPendingVerify = false;
            vAuth->pendingVerifyFingerprint = 0;
            client->printf("{\"type\":\"verify_confirmed\",\"success\":false,\"message\":\"Command or profile changed since the test\"}");
            return;
        }

        // Consume the transaction before writing, so a replayed message
        // cannot apply twice.
        vAuth->hasPendingVerify = false;
        vAuth->pendingVerifyFingerprint = 0;

        bool ok = _customStore->setCommandStatus(profileId, label,
                     success ? CMD_VERIFIED : CMD_UNVERIFIED, !success);

        client->printf("{\"type\":\"verify_confirmed\",\"success\":%s}", ok ? "true" : "false");
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ WebSocket events
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WebServerManager::_handleWebSocketEvent(AsyncWebSocket* server,
                                              AsyncWebSocketClient* client,
                                              AwsEventType type,
                                              void* arg,
                                              uint8_t* data,
                                              size_t len) {
    switch (type) {
        case WS_EVT_CONNECT: {
            Serial.printf("[WEB] Client %d connected (awaiting auth)\n", client->id());
            _findOrCreateClientAuth(client->id());
            client->printf("{\"type\":\"need_auth\"}");
            _activity = true;
            break;
        }

        case WS_EVT_DISCONNECT:
            Serial.printf("[WEB] Client %d disconnected\n", client->id());
            _removeClientAuth(client->id());
            break;

        case WS_EVT_DATA: {
            AwsFrameInfo* info = (AwsFrameInfo*)arg;

            // Bound the memory a single peer can make the device buffer
            // before any JSON parsing or authentication runs. 1009 is the
            // WebSocket "message too big" close code.
            if (info->len > WS_MAX_MESSAGE_LEN) {
                Serial.printf("[WEB] Oversized WS message from client %d (%u bytes) - closing\n",
                              client->id(), (unsigned)info->len);
                client->close(1009, "message too large");
                break;
            }

            if (info->final && info->index == 0 && info->len == len) {
                String msg;
                msg.reserve(len + 1);
                msg.concat(reinterpret_cast<const char*>(data), len);

                JsonDocument doc;
                DeserializationError error = deserializeJson(doc, msg);
                if (error) break;

                const char* msgType = doc["type"];
                if (!msgType) break;

                WsClientAuth* auth = _findOrCreateClientAuth(client->id());

                if (strcmp(msgType, "auth") == 0) {
                    const char* token = doc["token"];
                    if (_isValidSessionToken(token)) {
                        auth->authenticated = true;
                        auth->sessionToken = token;
                        client->printf("{\"type\":\"welcome\",\"message\":\"Welcome to CarTouch\"}");
                        Serial.printf("[WEB] Client %d authenticated\n", client->id());
                    } else {
                        client->printf("{\"type\":\"auth_failed\"}");
                        Serial.printf("[WEB] Failed auth attempt from client %d\n", client->id());
                        client->close(1008, "auth failed");
                    }
                    break;
                }

                if (!auth->authenticated) {
                    Serial.printf("[WEB] Unauthenticated message from client %d rejected\n", client->id());
                    client->printf("{\"type\":\"need_auth\"}");
                    break;
                }

                // WebSocket authentication is bound to the same finite
                // lifetime as the HTTP session token. Re-check it for every
                // post-auth message so an already-connected client cannot
                // continue issuing commands after the session expires or
                // after a password change invalidates the token.
                if (!_isValidSessionToken(auth->sessionToken.c_str())) {
                    auth->authenticated = false;
                    auth->canMonitor = false;
                    auth->sessionToken = "";
                    _syncCanMonitorSubscription();
                    client->printf("{\"type\":\"session_expired\"}");
                    Serial.printf("[WEB] Session expired for client %d\n", client->id());
                    client->close(1008, "session expired");
                    break;
                }

                if (strcmp(msgType, "ping") != 0) _activity = true;    // keep-alive pings do not count

                if (strcmp(msgType, "command") == 0) {
                    uint32_t now = millis();
                    if (now - auth->lastCommandTime < COMMAND_RATE_LIMIT_MS) {
                        client->printf("{\"type\":\"rate_limited\"}");
                        break;
                    }
                    auth->lastCommandTime = now;

                    const char* command = doc["command"];
                    if (command && _commandCallback) {
                        _commandCallback(command);
                        JsonDocument ack;
                        ack["type"] = "ack";
                        ack["command"] = command;
                        String ackJson;
                        serializeJson(ack, ackJson);
                        client->text(ackJson);
                    }
                } else if (strcmp(msgType, "ping") == 0) {
                    client->printf("{\"type\":\"pong\"}");
                } else if (strcmp(msgType, "get_logs") == 0) {
                    // Same data as GET /api/logs, available over the
                    // existing authenticated WebSocket connection too
                    // (checklist item 16 - error logging/telemetry).
                    client->text(getErrorLog()->toJSON());
                } else if (strcmp(msgType, "can_monitor_start") == 0) {
                    uint8_t bus;
                    if (!_canService || !parseJsonBoundedIndex(doc["bus"], 2, bus) ||
                        !_canService->isActive((CanBusId)bus)) {
                        client->printf("{\"type\":\"can_monitor_error\",\"message\":\"Selected CAN interface is unavailable\"}");
                        break;
                    }
                    auth->monitorBus = bus;
                    auth->canMonitor = true;
                    _syncCanMonitorSubscription();
                    JsonDocument response;
                    response["type"] = "can_monitor_state";
                    response["active"] = true;
                    response["bus"] = bus;
                    String json;
                    serializeJson(response, json);
                    client->text(json);
                } else if (strcmp(msgType, "can_monitor_stop") == 0) {
                    auth->canMonitor = false;
                    _syncCanMonitorSubscription();
                    client->printf("{\"type\":\"can_monitor_state\",\"active\":false}");
                } else if (strcmp(msgType, "can_record_start") == 0) {
                    uint8_t bus;
                    if (!_canService || !_commandCallback ||
                        !parseJsonBoundedIndex(doc["bus"], 3, bus)) {
                        client->printf("{\"type\":\"can_record_error\",\"message\":\"Invalid recording request\"}");
                        break;
                    }
                    const uint8_t mask = bus == 2 ? 3 : (uint8_t)(1u << bus);
                    if (((mask & 1u) && !_canService->isActive(CAN_BUS_1)) ||
                        ((mask & 2u) && !_canService->isActive(CAN_BUS_2))) {
                        client->printf("{\"type\":\"can_record_error\",\"message\":\"Selected CAN interface is unavailable\"}");
                        break;
                    }
                    const uint32_t now = millis();
                    if (now - auth->lastCommandTime < COMMAND_RATE_LIMIT_MS) {
                        client->printf("{\"type\":\"rate_limited\"}");
                        break;
                    }
                    auth->lastCommandTime = now;
                    char command[24];
                    snprintf(command, sizeof(command), "record_start:%u", mask);
                    _commandCallback(command);
                    client->printf("{\"type\":\"can_record_queued\"}");
                } else if (strcmp(msgType, "can_record_stop") == 0 ||
                           strcmp(msgType, "can_record_status") == 0) {
                    if (!_commandCallback) {
                        client->printf("{\"type\":\"can_record_error\",\"message\":\"Recording control is unavailable\"}");
                        break;
                    }
                    const uint32_t now = millis();
                    if (now - auth->lastCommandTime < COMMAND_RATE_LIMIT_MS) {
                        client->printf("{\"type\":\"rate_limited\"}");
                        break;
                    }
                    auth->lastCommandTime = now;
                    _commandCallback(strcmp(msgType, "can_record_stop") == 0
                        ? "record_stop" : "record_status");
                    client->printf("{\"type\":\"can_record_queued\"}");
                } else if (strcmp(msgType, "can_record_delete") == 0) {
                    const char* fileName = doc["name"];
                    if (!_commandCallback || !ctCanRecordFilenameValid(fileName)) {
                        client->printf("{\"type\":\"can_record_error\",\"message\":\"Invalid recording filename\"}");
                        break;
                    }
                    const uint32_t now = millis();
                    if (now - auth->lastCommandTime < COMMAND_RATE_LIMIT_MS) {
                        client->printf("{\"type\":\"rate_limited\"}");
                        break;
                    }
                    auth->lastCommandTime = now;
                    char command[32];
                    snprintf(command, sizeof(command), "record_delete:%s", fileName);
                    _commandCallback(command);
                    client->printf("{\"type\":\"can_record_queued\"}");
                } else if (strcmp(msgType, "obd_dtc_read") == 0 ||
                           strcmp(msgType, "obd_dtc_clear") == 0 ||
                           strcmp(msgType, "obd_dtc_status") == 0) {
                    if (!_commandCallback) {
                        client->printf("{\"type\":\"obd_dtc_error\",\"message\":\"OBD control is unavailable\"}");
                        break;
                    }
                    if (strcmp(msgType, "obd_dtc_status") != 0) {
                        const uint32_t now = millis();
                        if (now - auth->lastCommandTime < COMMAND_RATE_LIMIT_MS) {
                            client->printf("{\"type\":\"rate_limited\"}");
                            break;
                        }
                        auth->lastCommandTime = now;
                    }
                    _commandCallback(strcmp(msgType, "obd_dtc_read") == 0 ? "dtc_read" :
                        strcmp(msgType, "obd_dtc_clear") == 0 ? "dtc_clear" : "dtc_status");
                    client->printf("{\"type\":\"obd_dtc_queued\"}");
                } else if (strncmp(msgType, "learn_", 6) == 0 || strncmp(msgType, "verify_", 7) == 0) {
                    // The same rate limit as regular commands also
                    // applies to verify_command, since that message can
                    // trigger a real send on the bus.
                    if (strcmp(msgType, "verify_command") == 0) {
                        uint32_t now = millis();
                        if (now - auth->lastCommandTime < COMMAND_RATE_LIMIT_MS) {
                            client->printf("{\"type\":\"rate_limited\"}");
                            break;
                        }
                        auth->lastCommandTime = now;
                    }
                    _handleLearnModeMessage(client, doc, msgType);
                }
            }
            break;
        }

        case WS_EVT_PONG:
            break;

        case WS_EVT_ERROR:
            break;
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ HTTP authentication
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

//
// Brute-force protection:
// _isLoginLocked(ip) must be checked by every entry point that directly
// compares a submitted password against cfg->webPass, BEFORE that
// comparison happens - never after. Checking after the comparison would
// still let an attacker distinguish "right password, locked" from
// "wrong password" via timing/behavior, and would do the expensive
// comparison work on every locked-out attempt for no reason.
//
// _registerLoginFailure() must be called on every failed comparison,
// and _registerLoginSuccess() on every successful one, at all three
// call sites ("/", "/login", _authenticate()) - they all share the same
// counter/lockout state, so a lockout triggered via one path (e.g.
// repeated bad Basic-Auth on "/") also blocks the others (e.g. "/login")
// until it expires.

WebServerManager::LoginTrack* WebServerManager::_loginSlot(uint32_t ip, bool create) {
    LoginTrack* freeSlot = nullptr;
    LoginTrack* oldest   = nullptr;
    for (int i = 0; i < LOGIN_TRACK_SLOTS; ++i) {
        LoginTrack& t = _loginTrack[i];
        if (t.used && t.ip == ip) { t.lastSeen = millis(); return &t; }
        if (!t.used) { if (!freeSlot) freeSlot = &t; }
        else if (!oldest || (int32_t)(t.lastSeen - oldest->lastSeen) < 0) oldest = &t;
    }
    if (!create) return nullptr;
    LoginTrack* s = freeSlot ? freeSlot : oldest;
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->ip = ip;
    s->lastSeen = millis();
    return s;
}

bool WebServerManager::_isLoginLocked(uint32_t ip, uint32_t& remainingMs) {
    LoginTrack* t = _loginSlot(ip, false);
    if (!t || t->lockUntil == 0) return false;
    uint32_t now = millis();
    // Wrap-safe: the lockout (30 s) is far shorter than the millis() period.
    if ((int32_t)(now - t->lockUntil) >= 0) {
        t->lockUntil = 0;
        t->fails = 0;
        return false;
    }
    remainingMs = t->lockUntil - now;
    return true;
}

void WebServerManager::_registerLoginFailure(uint32_t ip) {
    LoginTrack* t = _loginSlot(ip, true);
    if (t->fails < 255) t->fails++;
    if (t->fails >= LOGIN_MAX_ATTEMPTS) {
        uint32_t until = millis() + LOGIN_LOCKOUT_MS;
        t->lockUntil = (until == 0) ? 1 : until;  // 0 is the "not locked" sentinel
        getErrorLog()->log(LOG_CAT_WEB, LOG_WARN,
            "Login lockout triggered after %u failed attempts", t->fails);
    }
}

void WebServerManager::_registerLoginSuccess(uint32_t ip) {
    LoginTrack* t = _loginSlot(ip, false);
    if (t) { t->fails = 0; t->lockUntil = 0; }
}

// Contract: on failure (locked out or bad credentials), _authenticate()
// itself sends the appropriate response to `request` (429+Retry-After,
// or 401+WWW-Authenticate via requestAuthentication()) and returns
// false. Callers must NOT send another response in that case - do
// exactly `if (!_authenticate(request)) return;` with nothing else.
// Sending twice on the same AsyncWebServerRequest is undefined
// behavior in ESPAsyncWebServer.
bool WebServerManager::_authenticate(AsyncWebServerRequest* request) {
    AppConfig* cfg = getConfig();

    const uint32_t ip = ctClientIp(request);
    uint32_t remainingMs;
    if (_isLoginLocked(ip, remainingMs)) {
        AsyncWebServerResponse* response = request->beginResponse(429, "application/json",
            "{\"error\":\"Too many failed login attempts. Try again later.\"}");
        response->addHeader("Retry-After", String(remainingMs / 1000 + 1));
        request->send(response);
        return false;
    }

    if (!request->authenticate(cfg->webUser, cfg->webPass)) {
        getErrorLog()->log(LOG_CAT_WEB, LOG_WARN, "Failed basic-auth attempt (%s)", request->url().c_str());
        _registerLoginFailure(ip);
        request->requestAuthentication("CarTouch");
        return false;
    }
    _registerLoginSuccess(ip);
    _activity = true;
    return true;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Control API
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WebServerManager::_handleAPIControl(AsyncWebServerRequest* request) {
    String command = request->arg("command");

    if (command.length() == 0) {
        request->send(400, "application/json", "{\"error\":\"command parameter required\"}");
        return;
    }

    if (_commandCallback) {
        _commandCallback(command.c_str());
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["command"] = command;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

void WebServerManager::_handleAPICanConfig(AsyncWebServerRequest* request) {
    if (!request->hasArg("txPin") || !request->hasArg("rxPin") ||
        !request->hasArg("speed") || !request->hasArg("listenOnly")) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"txPin, rxPin, speed and listenOnly are required\"}");
        return;
    }

    const bool hasCan1Cs = request->hasArg("can1CsPin");
    const bool hasCan1Int = request->hasArg("can1IntPin");
    const bool hasCan1Speed = request->hasArg("can1Speed");
    const bool hasCan1ListenOnly = request->hasArg("can1ListenOnly");
    const bool hasObdCanBus = request->hasArg("obdCanBus");
    const bool hasLearnCanBus = request->hasArg("learnCanBus");
    const bool hasVehicleCanBus = request->hasArg("vehicleCanBus");
    const bool hasAnyCan1 = hasCan1Cs || hasCan1Int || hasCan1Speed || hasCan1ListenOnly;
    const bool hasAllCan1 = hasCan1Cs && hasCan1Int && hasCan1Speed && hasCan1ListenOnly;
    if (hasAnyCan1 && !hasAllCan1) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"All CAN1 settings are required together\"}");
        return;
    }

    const String txArg          = request->arg("txPin");
    const String rxArg          = request->arg("rxPin");
    const String speedArg       = request->arg("speed");
    const String listenOnlyArg  = request->arg("listenOnly");
    AppConfig*   cfg            = getConfig();
    uint8_t      tx             = 0;
    uint8_t      rx             = 0;
    uint8_t      can1CsPin      = cfg->can1CsPin;
    uint8_t      can1IntPin     = cfg->can1IntPin;
    uint32_t     speed          = 0;
    uint32_t     can1Speed      = cfg->can1Speed;
    bool         listenOnly     = false;
    bool         can1ListenOnly = cfg->can1ListenOnly;
    uint8_t      obdCanBus      = cfg->obdCanBus;
    uint8_t      learnCanBus    = cfg->learnCanBus;
    uint8_t      vehicleCanBus  = cfg->vehicleCanBus;

    const bool can0Valid = ctParseBoundedIndex(txArg.c_str(), 49, tx) &&
                           ctParseBoundedIndex(rxArg.c_str(), 49, rx) &&
                           ctParseUnsignedDecimal(speedArg.c_str(), 1000000u, speed) &&
                           ctParseBoolean(listenOnlyArg.c_str(), listenOnly) &&
                           isValidCanSpeed(speed);
    bool can1Valid = true;
    if (hasAllCan1) {
        const String can1CsArg = request->arg("can1CsPin");
        const String can1IntArg = request->arg("can1IntPin");
        const String can1SpeedArg = request->arg("can1Speed");
        const String can1ListenOnlyArg = request->arg("can1ListenOnly");
        can1Valid = ctParseBoundedIndex(can1CsArg.c_str(), 49, can1CsPin) &&
                    ctParseBoundedIndex(can1IntArg.c_str(), 49, can1IntPin) &&
                    ctParseUnsignedDecimal(can1SpeedArg.c_str(), 1000000u, can1Speed) &&
                    ctParseBoolean(can1ListenOnlyArg.c_str(), can1ListenOnly) &&
                    isValidCan1Speed(can1Speed);
    }
    const bool obdCanBusValid = !hasObdCanBus ||
        ctParseBoundedIndex(request->arg("obdCanBus").c_str(), 2, obdCanBus);
    const bool learnCanBusValid = !hasLearnCanBus ||
        ctParseBoundedIndex(request->arg("learnCanBus").c_str(), 2, learnCanBus);

    const bool vehicleCanBusValid = !hasVehicleCanBus ||
        ctParseBoundedIndex(request->arg("vehicleCanBus").c_str(), 2, vehicleCanBus);

    if (!can0Valid || !can1Valid || !obdCanBusValid || !learnCanBusValid || !vehicleCanBusValid ||
        !validateCanPinAssignment(tx, rx, can1CsPin, can1IntPin)) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid or conflicting CAN configuration\"}");
        return;
    }

    const uint8_t oldTxPin = cfg->canTxPin;
    const uint8_t oldRxPin = cfg->canRxPin;
    const uint32_t oldSpeed = cfg->canSpeed;
    const bool oldListenOnly = cfg->listenOnlyMode;
    const uint8_t oldCan1CsPin = cfg->can1CsPin;
    const uint8_t oldCan1IntPin = cfg->can1IntPin;
    const uint32_t oldCan1Speed = cfg->can1Speed;
    const bool oldCan1ListenOnly = cfg->can1ListenOnly;
    const uint8_t oldObdCanBus = cfg->obdCanBus;
    const uint8_t oldLearnCanBus = cfg->learnCanBus;
    const uint8_t oldVehicleCanBus = cfg->vehicleCanBus;
    cfg->canTxPin = tx;
    cfg->canRxPin = rx;
    cfg->canSpeed = speed;
    cfg->listenOnlyMode = listenOnly;
    cfg->can1CsPin = can1CsPin;
    cfg->can1IntPin = can1IntPin;
    cfg->can1Speed = can1Speed;
    cfg->can1ListenOnly = can1ListenOnly;
    cfg->obdCanBus = obdCanBus;
    cfg->learnCanBus = learnCanBus;
    cfg->vehicleCanBus = vehicleCanBus;

    if (!saveConfig()) {
        cfg->canTxPin = oldTxPin;
        cfg->canRxPin = oldRxPin;
        cfg->canSpeed = oldSpeed;
        cfg->listenOnlyMode = oldListenOnly;
        cfg->can1CsPin = oldCan1CsPin;
        cfg->can1IntPin = oldCan1IntPin;
        cfg->can1Speed = oldCan1Speed;
        cfg->can1ListenOnly = oldCan1ListenOnly;
        cfg->obdCanBus = oldObdCanBus;
        cfg->learnCanBus = oldLearnCanBus;
        cfg->vehicleCanBus = oldVehicleCanBus;
        request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to save CAN configuration\"}");
        return;
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["rebootRequired"] = true;
    doc["message"] = "CAN configuration saved. The device will reboot to apply it.";
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
    _rebootPending = true;
    _rebootAt = millis() + 1500;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Status API
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

void WebServerManager::_handleAPIStatus(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["status"]      = "ok";
    doc["message"]     = "CarTouch active";
    doc["usingDefaultPassword"] = isUsingDefaultPassword();
    doc["firmwareVersion"] = CAR_TOUCH_FIRMWARE_VERSION;
    const bool staUp = WiFi.status() == WL_CONNECTED;
    const bool apUp  = (WiFi.getMode() & WIFI_MODE_AP) != 0;
    doc["wifiConnected"] = staUp || apUp;
    doc["wifiMode"] = staUp ? (apUp ? "STA+AP" : "STA") : (apUp ? "AP" : "OFFLINE");
    doc["ip"] = staUp ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    doc["apIp"] = apUp ? WiFi.softAPIP().toString() : String("");
    doc["wifiSsid"] = getConfig()->wifiSSID;
    doc["bleEnabled"] = bleManager.isEnabled();
    doc["bleConnected"] = bleManager.isConnected();
    doc["bleOtaInProgress"] = bleManager.isOtaInProgress();
    AppConfig* cfg = getConfig();
    doc["canTxPin"] = cfg->canTxPin;
    doc["canRxPin"] = cfg->canRxPin;
    doc["canSpeed"] = cfg->canSpeed;
    doc["listenOnlyMode"] = cfg->listenOnlyMode;
    doc["can1CsPin"] = cfg->can1CsPin;
    doc["can1IntPin"] = cfg->can1IntPin;
    doc["can1Speed"] = cfg->can1Speed;
    doc["can1ListenOnly"] = cfg->can1ListenOnly;
    doc["obdCanBus"] = cfg->obdCanBus;
    doc["learnCanBus"] = cfg->learnCanBus;
    doc["vehicleCanBus"] = cfg->vehicleCanBus;

    {
        JsonObject sd = doc["sd"].to<JsonObject>();
        sd["state"] = sdStorage.stateText();
        sd["csPin"] = sdStorage.csPin();
        sd["totalBytes"] = (uint64_t)sdStorage.totalBytes();
        sd["freeBytes"] = (uint64_t)sdStorage.freeBytes();
        JsonObject internal = doc["internalStorage"].to<JsonObject>();
        const uint64_t internalTotal = SPIFFS.totalBytes();
        const uint64_t internalUsed = SPIFFS.usedBytes();
        internal["state"] = internalTotal > 0 ? "ready" : "unavailable";
        internal["totalBytes"] = internalTotal;
        internal["freeBytes"] = internalTotal > internalUsed ? internalTotal - internalUsed : 0;
        JsonObject st = doc["storageChoice"].to<JsonObject>();
        static const char* cats[] = { "db", "rec", "prof", "bak" };
        for (const char* c : cats) {
            const CtStorageChoice ch = getStorageChoice(c);
            st[c] = ch == CT_STORE_SD ? "sd" : (ch == CT_STORE_INTERNAL ? "internal" : "auto");
        }
    }

    if (_moduleStatus) {
        JsonArray modules = doc["modules"].to<JsonArray>();
        for (uint8_t i = 0; i < MODULE_COUNT; ++i) {
            JsonObject item = modules.add<JsonObject>();
            item["id"] = i;
            item["name"] = _moduleStatus->name((ModuleId)i);
            item["state"] = _moduleStatus->stateText(_moduleStatus->getState((ModuleId)i));
            item["ready"] = (_moduleStatus->getState((ModuleId)i) == MODULE_READY);
        }
    }

    if (_profileManager) {
        char vehicleName[48];
        _profileManager->getActiveVehicleName(vehicleName, sizeof(vehicleName));
        doc["activeVehicle"] = vehicleName;
    }

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ 404
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WebServerManager::_handleNotFound(AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "404 - Not Found");
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Broadcasts
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WebServerManager::broadcastVehicleData(const VehicleData& data) {
    if (!_started) return;

    String json = _vehicleDataToJSON(data);

    for (AsyncWebSocketClient& client : _ws.getClients()) {
        WsClientAuth* auth = _findClientAuth(client.id());
        if (auth && auth->authenticated) {
            client.text(json);
        }
    }
}

void WebServerManager::broadcastStatus(const char* status) {
    if (!_started) return;

    JsonDocument doc;
    doc["type"] = "status";
    doc["message"] = status ? status : "";
    String msg;
    serializeJson(doc, msg);

    for (AsyncWebSocketClient& client : _ws.getClients()) {
        WsClientAuth* auth = _findClientAuth(client.id());
        if (auth && auth->authenticated) {
            client.text(msg);
        }
    }
}

void WebServerManager::broadcastCanDiagnostics(const CanDiagnostics& diagnostics,
                                                const char* interfaceName) {
    if (!_started) return;

    JsonDocument doc;
    doc["type"] = "can_diagnostics";
    doc["interface"] = interfaceName ? interfaceName : "CAN1";
    doc["driverReady"] = diagnostics.driverReady;
    doc["listenOnly"] = diagnostics.listenOnly;
    doc["busActive"] = diagnostics.busActive;
    doc["busOff"] = diagnostics.busOff;
    doc["rxFrames"] = diagnostics.rxFrames;
    doc["txFrames"] = diagnostics.txFrames;
    doc["errorCount"] = diagnostics.errorCount;
    doc["txFailedCount"] = diagnostics.txFailedCount;
    doc["rxMissedCount"] = diagnostics.rxMissedCount;
    doc["rxOverrunCount"] = diagnostics.rxOverrunCount;
    doc["arbitrationLostCount"] = diagnostics.arbitrationLostCount;
    doc["busErrorCount"] = diagnostics.busErrorCount;
    doc["txErrorCounter"] = diagnostics.txErrorCounter;
    doc["rxErrorCounter"] = diagnostics.rxErrorCounter;
    doc["msgsWaiting"] = diagnostics.msgsWaiting;

    String json;
    serializeJson(doc, json);
    for (AsyncWebSocketClient& client : _ws.getClients()) {
        WsClientAuth* auth = _findClientAuth(client.id());
        if (auth && auth->authenticated) client.text(json);
    }
}

void WebServerManager::broadcastCanRecordingStatus(bool active, uint8_t busMask,
                                                    uint32_t frames,
                                                    uint32_t droppedFrames,
                                                    const char* fileName,
                                                    const char* error) {
    if (!_started) return;

    JsonDocument doc;
    doc["type"] = "can_record_state";
    doc["active"] = active;
    doc["busMask"] = busMask;
    doc["frames"] = frames;
    doc["dropped"] = droppedFrames;
    doc["file"] = fileName ? fileName : "";
    doc["error"] = error ? error : "";

    String json;
    serializeJson(doc, json);
    for (AsyncWebSocketClient& client : _ws.getClients()) {
        WsClientAuth* auth = _findClientAuth(client.id());
        if (auth && auth->authenticated) client.text(json);
    }
}

void WebServerManager::broadcastCanRecordingFilesChanged() {
    if (!_started) return;
    for (AsyncWebSocketClient& client : _ws.getClients()) {
        WsClientAuth* auth = _findClientAuth(client.id());
        if (auth && auth->authenticated) {
            client.printf("{\"type\":\"can_record_files_changed\"}");
        }
    }
}

void WebServerManager::broadcastObdDiagnosticStatus(uint8_t state,
                                                     uint8_t operation,
                                                     uint8_t error,
                                                     uint8_t responseCode,
                                                     const uint16_t* dtcList,
                                                     uint8_t dtcCount) {
    if (!_started) return;

    JsonDocument doc;
    doc["type"] = "obd_dtc_state";
    doc["state"] = state;
    doc["operation"] = operation;
    doc["error"] = error;
    doc["responseCode"] = responseCode;
    JsonArray dtcs = doc["dtcs"].to<JsonArray>();
    for (uint8_t i = 0; dtcList && i < dtcCount; ++i) dtcs.add(dtcList[i]);

    String json;
    serializeJson(doc, json);
    for (AsyncWebSocketClient& client : _ws.getClients()) {
        WsClientAuth* auth = _findClientAuth(client.id());
        if (auth && auth->authenticated) client.text(json);
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ VehicleData -> JSON
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WebServerManager::broadcastModuleStatus() {
    if (!_started || !_moduleStatus) return;

    JsonDocument doc;
    doc["type"] = "module_status";
    JsonArray modules = doc["modules"].to<JsonArray>();
    for (uint8_t i = 0; i < MODULE_COUNT; ++i) {
        JsonObject item = modules.add<JsonObject>();
        item["id"] = i;
        item["name"] = _moduleStatus->name((ModuleId)i);
        item["state"] = _moduleStatus->stateText(_moduleStatus->getState((ModuleId)i));
        item["ready"] = (_moduleStatus->getState((ModuleId)i) == MODULE_READY);
    }
    String json;
    serializeJson(doc, json);
    for (AsyncWebSocketClient& client : _ws.getClients()) {
        WsClientAuth* auth = _findClientAuth(client.id());
        if (auth && auth->authenticated) client.text(json);
    }
}

String WebServerManager::_vehicleDataToJSON(const VehicleData& data) {
    JsonDocument doc;

    doc["type"]           = "vehicle_data";
    // A value the ECU did not answer (or answered long ago) is sent as null,
    // never as a made-up number. Field names are unchanged.
    if (ctVdValid(data.validMask, CT_VD_SPEED))    doc["speed"] = data.vehicleSpeed;    else doc["speed"] = nullptr;
    if (ctVdValid(data.validMask, CT_VD_RPM))      doc["rpm"] = data.engineRPM;         else doc["rpm"] = nullptr;
    if (ctVdValid(data.validMask, CT_VD_COOLANT))  doc["coolantTemp"] = data.coolantTemp; else doc["coolantTemp"] = nullptr;
    if (ctVdValid(data.validMask, CT_VD_BATTERY) && ctBatteryVoltageAvailable(data.batteryVoltage)) {
        doc["battery"] = data.batteryVoltage;
    } else {
        doc["battery"] = nullptr;
    }
    if (ctVdValid(data.validMask, CT_VD_FUEL))     doc["fuel"] = data.fuelLevel;        else doc["fuel"] = nullptr;
    if (ctVdValid(data.validMask, CT_VD_THROTTLE)) doc["throttle"] = data.throttlePos;  else doc["throttle"] = nullptr;

    doc["doorFL"]   = (int)data.doorFL;
    doc["doorFR"]      = (int)data.doorFR;
    doc["doorRL"]         = (int)data.doorRL;
    doc["doorRR"]            = (int)data.doorRR;
    doc["trunk"]                 = (int)data.trunkState;
    doc["alarm"]                     = (int)data.alarmState;

    String output;
    serializeJson(doc, output);
    return output;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Client status
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool WebServerManager::isClientConnected() {
    return _ws.count() > 0;
}

uint8_t WebServerManager::getClientCount() {
    return _ws.count();
}
