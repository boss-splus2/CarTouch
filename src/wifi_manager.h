/**
 * wifi_manager.h - WiFi connectivity management
 *
 * Handles connecting the ESP32 to a WiFi network, or creating its own
 * Access Point. In AP mode the device is reachable directly; in STA
 * mode it joins an existing home/office network.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"

// WiFi state. The CT_ prefix avoids collisions with ESP32 Arduino
// macros such as WIFI_AP and WIFI_STA.

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ WiFi state
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

enum WiFiState : uint8_t {
    CT_WIFI_DISABLED = 0,
    CT_WIFI_AP       = 1,  // Access Point mode
    CT_WIFI_STA      = 2,  // Station mode (connected to a router)
    CT_WIFI_STA_FAIL = 3   // Station mode failed to connect
};

class WiFiManager {

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Public API
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

public:
    WiFiManager();

    /**
     * Starts WiFi.
     * @param mode 0 = off, 1 = AP, 2 = STA (attempts to connect)
     */
    void begin(uint8_t mode = 2);

    void disconnect();


    /**
     * Asks the device to join a router. Non-blocking: the attempt runs from
     * update(). The CarTouch access point stays on, and the network is saved
     * only after it connects.
     */
    void requestConnect(const char* ssid, const char* password);

    /** Forgets the saved router and goes back to AP only. */
    void forgetNetwork();

    /** Call every loop(): finishes connect attempts and retries a saved router. */
    void update();

    WiFiState getState();

    /** Returns the IP address (AP or STA mode). */
    IPAddress getIP();

    bool isConnected();
    bool isEnabled();
    void setEnabled(bool enabled);

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Connection internals
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

private:
    WiFiState _state;
    bool       _enabled;
    bool       _apUp = false;
    bool       _pending = false;     // requestConnect() waiting for update()
    bool       _connecting = false;  // a STA attempt is in progress
    bool       _saveOnConnect = false;
    uint32_t   _connectStart = 0;
    uint32_t   _lastAttempt = 0;
    char       _pSsid[32] = {};
    char       _pPass[64] = {};

    void _startAP();
    void _startSTA();
    void _beginSta(const char* ssid, const char* pass);
};

#endif    // WIFI_MANAGER_H
