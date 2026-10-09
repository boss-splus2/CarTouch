/**
 * wifi_manager.cpp - WiFi manager implementation
 */

#include "wifi_manager.h"
#include "error_log.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Constructor
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

WiFiManager::WiFiManager() {
    _state    = CT_WIFI_DISABLED;
    _enabled  = true;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ begin()
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WiFiManager::begin(uint8_t mode) {
    if (!_enabled) {
        Serial.println("[WiFi] WiFi is disabled");
        _state = CT_WIFI_DISABLED;
        return;
    }

    Serial.println("[WiFi] Starting WiFi...");

    switch (mode) {
        case 0:
            WiFi.mode(WIFI_OFF);
            _state = CT_WIFI_DISABLED;
            Serial.println("[WiFi] WiFi turned off");
            break;

        case 1:
            _startAP();
            break;

        case 2:
            _startSTA();
            break;

        default:
            _startAP();
            break;
    }
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Access Point mode
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// The access point key comes from getWifiApKey(): the device password in
// personal mode (unchanged behaviour), a separate stored key in commercial
// mode. WPA2 needs 8 to 63 characters; an empty key would start an OPEN
// network, so getWifiApKey() falls back to the factory key.
static const char* apPassword() {
    return getWifiApKey();
}

void WiFiManager::_startAP() {
    WiFi.mode(WIFI_AP);

    // apPassword() is always >= 8 characters, so the Arduino default auth mode
    // is WPA2-PSK (never an open network). Client count is capped.
    bool result = WiFi.softAP(WIFI_AP_NAME, apPassword(), WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CLIENTS);

    if (result) {
        _apUp = true;
        _state = CT_WIFI_AP;
        Serial.printf("[WiFi] Access Point: %s | IP: %s\n",
                      WIFI_AP_NAME, WiFi.softAPIP().toString().c_str());
        Serial.println("[WiFi] AP password = device password (not printed)");
    } else {
        _state = CT_WIFI_DISABLED;
        Serial.println("[WiFi] Failed to create Access Point");
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Station mode
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WiFiManager::_beginSta(const char* ssid, const char* pass) {
    WiFi.mode(WIFI_AP_STA);
    if (!_apUp) _apUp = WiFi.softAP(WIFI_AP_NAME, apPassword(), WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CLIENTS);
    WiFi.begin(ssid, pass);
    _connecting = true;
    _connectStart = millis();
}

// Boot-time connect: NON-BLOCKING. The access point comes up immediately and
// the router join is started in the background; update() (called every loop)
// finishes it, applies the WIFI_TIMEOUT_MS limit and keeps retrying later.
// The old version waited here for up to 10 s, which delayed everything that
// started after Wi-Fi (BLE, web server). The access point stays up beside the
// router connection, so the device is always reachable at its own network.
void WiFiManager::_startSTA() {
    AppConfig* cfg = getConfig();

    if (strlen(cfg->wifiSSID) == 0) {
        Serial.println("[WiFi] No saved SSID - AP mode");
        _startAP();
        return;
    }

    _startAP();
    _beginSta(cfg->wifiSSID, cfg->wifiPassword);
    _lastAttempt = millis();
    Serial.printf("[WiFi] Connecting to %s in the background (AP stays up)\n", cfg->wifiSSID);
}

void WiFiManager::requestConnect(const char* ssid, const char* password) {
    if (!ssid || !ssid[0]) return;
    strncpy(_pSsid, ssid, sizeof(_pSsid) - 1);
    _pSsid[sizeof(_pSsid) - 1] = '\0';
    strncpy(_pPass, password ? password : "", sizeof(_pPass) - 1);
    _pPass[sizeof(_pPass) - 1] = '\0';
    _pending = true;
}

void WiFiManager::forgetNetwork() {
    AppConfig* cfg = getConfig();
    cfg->wifiSSID[0] = '\0';
    cfg->wifiPassword[0] = '\0';
    saveConfig();
    _pending = false;
    _connecting = false;
    if (_state != CT_WIFI_DISABLED) {
        WiFi.disconnect(false);
        WiFi.mode(WIFI_AP);
        _state = CT_WIFI_AP;
    }
}

void WiFiManager::update() {
    if (!_enabled || _state == CT_WIFI_DISABLED) return;
    const uint32_t now = millis();

    if (_pending) {
        _pending = false;
        _saveOnConnect = true;
        Serial.printf("[WiFi] Connecting to %s...\n", _pSsid);
        _beginSta(_pSsid, _pPass);
        return;
    }

    if (_connecting) {
        if (WiFi.status() == WL_CONNECTED) {
            _connecting = false;
            _state = CT_WIFI_STA;
            if (_saveOnConnect) {
                _saveOnConnect = false;
                AppConfig* cfg = getConfig();
                strncpy(cfg->wifiSSID, _pSsid, sizeof(cfg->wifiSSID) - 1);
                cfg->wifiSSID[sizeof(cfg->wifiSSID) - 1] = '\0';
                strncpy(cfg->wifiPassword, _pPass, sizeof(cfg->wifiPassword) - 1);
                cfg->wifiPassword[sizeof(cfg->wifiPassword) - 1] = '\0';
                saveConfig();
            }
            Serial.printf("[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
        } else if ((uint32_t)(now - _connectStart) > WIFI_TIMEOUT_MS) {
            _connecting = false;
            _saveOnConnect = false;
            _lastAttempt = now;
            getErrorLog()->log(LOG_CAT_WIFI, LOG_WARN, "Router connect timed out - AP only");
            WiFi.disconnect(false);
            WiFi.mode(WIFI_AP);
            _state = CT_WIFI_AP;
        }
        return;
    }

    // Router link lost: fall back to the AP-only state and retry later.
    if (_state == CT_WIFI_STA && WiFi.status() != WL_CONNECTED) {
        _state = CT_WIFI_AP;
        _lastAttempt = now;
        WiFi.disconnect(false);
        WiFi.mode(WIFI_AP);
        return;
    }

    // Saved router not connected: retry every 60 s, but not while someone is
    // using the AP (the radio would leave the AP channel during a scan).
    AppConfig* cfg = getConfig();
    if (_state == CT_WIFI_AP && cfg->wifiSSID[0] &&
        (uint32_t)(now - _lastAttempt) > 60000 && WiFi.softAPgetStationNum() == 0) {
        _lastAttempt = now;
        _beginSta(cfg->wifiSSID, cfg->wifiPassword);
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Disconnect
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

void WiFiManager::disconnect() {
    _apUp = false;
    _pending = false;
    _connecting = false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    _state = CT_WIFI_DISABLED;
    Serial.println("[WiFi] WiFi disconnected");
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Network scan
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■


// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Status accessors
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

WiFiState WiFiManager::getState() {
    return _state;
}

IPAddress WiFiManager::getIP() {
    if (_state == CT_WIFI_STA && WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP();
    }
    if (_apUp) {
        return WiFi.softAPIP();
    }
    return IPAddress(0, 0, 0, 0);
}

bool WiFiManager::isConnected() {
    return _state != CT_WIFI_DISABLED &&
           (_apUp || WiFi.status() == WL_CONNECTED);
}

bool WiFiManager::isEnabled() {
    return _enabled;
}

void WiFiManager::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!enabled) {
        disconnect();
    }
}
