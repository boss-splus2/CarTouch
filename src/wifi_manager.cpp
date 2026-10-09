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
    // Avoid persisting transient radio settings in NVS and disable modem
    // sleep: both improve AP discovery/reliability on some ESP32-S3 boards.
    WiFi.persistent(false);
    WiFi.setSleep(false);

    switch (mode) {
        case 0:
            _apStartFailed = false; // explicit OFF must not be auto-restarted
            _apUp = false;
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
// network, so the key falls back to the factory key.
// Copied under the credentials lock: in personal mode the key IS the web
// password, which the async web task or BLE can change at any time.
static void apPassword(char* out, size_t outSize) {
    getWifiApKeySnapshot(out, outSize);
}

void WiFiManager::_startAP() {
    const bool staWasConnected = WiFi.status() == WL_CONNECTED;
    const bool keepStaMode = staWasConnected || _connecting;
    _apUp = false;
    _apStartFailed = true;
    WiFi.mode(keepStaMode ? WIFI_AP_STA : WIFI_AP);
    delay(150); // let the ESP-IDF driver settle before configuring the AP

    // Configure the AP gateway explicitly. Relying on inherited netif state
    // can leave DHCP/IP inconsistent after AP/STA mode transitions or scans.
    const IPAddress apIp(192, 168, 4, 1);
    const IPAddress apMask(255, 255, 255, 0);
    if (!WiFi.softAPConfig(apIp, apIp, apMask)) {
        Serial.println("[WiFi] Warning: softAPConfig failed; continuing with driver defaults");
    }

    // Copy the key under the credentials lock, then release it before Wi-Fi
    // APIs. WPA2 keys remain 8..15 characters; never start an open AP.
    char apKey[16] = {};
    apPassword(apKey, sizeof(apKey));

    bool started = false;
    IPAddress apIpNow(0, 0, 0, 0);
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
        if (attempt != 0) {
            Serial.println("[WiFi] AP retry: resetting Wi-Fi driver mode");
            WiFi.softAPdisconnect(true);
            if (!keepStaMode) WiFi.mode(WIFI_OFF);
            delay(200);
            WiFi.mode(keepStaMode ? WIFI_AP_STA : WIFI_AP);
            delay(200);
            if (!WiFi.softAPConfig(apIp, apIp, apMask)) {
                Serial.println("[WiFi] Warning: softAPConfig retry failed");
            }
        }

        started = WiFi.softAP(WIFI_AP_NAME, apKey, WIFI_AP_CHANNEL, 0,
                              WIFI_AP_MAX_CLIENTS);
        // softAP() may return before the AP interface has a usable IP. Wait
        // boundedly before treating that as a failed startup.
        if (started) {
            const uint32_t waitStart = millis();
            do {
                apIpNow = WiFi.softAPIP();
                if (apIpNow != IPAddress(0, 0, 0, 0)) break;
                delay(50);
            } while ((uint32_t)(millis() - waitStart) < 1500);
        }
        if (started && apIpNow != IPAddress(0, 0, 0, 0)) break;
        started = false;
    }
    memset(apKey, 0, sizeof(apKey));

    if (started && apIpNow != IPAddress(0, 0, 0, 0)) {
        _apUp = true;
        _apStartFailed = false;
        _apRetryAt = 0;
        _state = staWasConnected ? CT_WIFI_STA : CT_WIFI_AP;
        Serial.printf("[WiFi] Access Point: %s | IP: %s | channel: %u\n",
                      WIFI_AP_NAME, apIpNow.toString().c_str(), WIFI_AP_CHANNEL);
        Serial.println("[WiFi] AP password = device password (not printed)");
    } else {
        _apUp = false;
        _apStartFailed = true;
        _apRetryAt = millis() + 10000UL;
        _state = staWasConnected ? CT_WIFI_STA :
                 (_connecting ? _state : CT_WIFI_DISABLED);
        Serial.printf("[WiFi] ERROR: AP '%s' failed after retries; will retry in 10 seconds\n",
                      WIFI_AP_NAME);
    }
}

void WiFiManager::_beginSta(const char* ssid, const char* pass) {
    // Establish the local AP first. Do not assume a previous AP survived a
    // failed startup or mode transition; this is also the recovery path.
    if (!_apUp) _startAP();
    WiFi.mode(WIFI_AP_STA);
    delay(100);
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
    if (!_enabled) return;
    const uint32_t now = millis();

    // Previously a transient AP startup failure became permanent: the
    // disabled-state early return prevented all later recovery attempts.
    // Retry only after a real startup failure; explicit mode=OFF stays off.
    if (_apStartFailed && (int32_t)(now - _apRetryAt) >= 0) {
        // If STA is already connected, _startAP preserves AP+STA mode so the
        // recovery attempt does not deliberately tear down the router link.
        _startAP();
    }
    // If a saved router connection is in progress, continue supervising it
    // even while the AP is temporarily down. Otherwise remain idle until the
    // scheduled AP retry (explicit OFF never sets _apStartFailed).
    if (_state == CT_WIFI_DISABLED && !_connecting) return;

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
            _state = _apUp ? CT_WIFI_AP : CT_WIFI_DISABLED;
            if (!_apUp && _apStartFailed) _apRetryAt = now + 10000UL;
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
    _apStartFailed = false;
    _apRetryAt = 0;
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
