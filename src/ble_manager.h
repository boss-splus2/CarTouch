#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <Arduino.h>
#include "ct_login_lock.h"

typedef void (*BLECommandCallback)(const char* command);

/**
 * BLE control/status and firmware OTA manager.
 *
 * The BLE OTA protocol is intentionally simple so it can be used by a
 * generic GATT client (for example nRF Connect or a dedicated mobile app):
 *   1) write "START:<password>:<firmware_size>:<sha256>" to the command characteristic
 *   2) write raw firmware bytes to the data characteristic
 *   3) write "END" to the command characteristic
 *   4) device validates the Update object and reboots after success
 *
 * The same Web password is required before a BLE OTA session can start.
 */
class BLEManager {
public:
    BLEManager();

    bool begin();
    void update();
    void setCommandCallback(BLECommandCallback callback);
    void publishStatus(const char* status);
    bool isEnabled() const;
    bool isConnected() const;
    bool isOtaInProgress() const;
    /** True once if a BLE client connected or sent a command since the last call (keeps auto-sleep away). */
    bool consumeActivity() { const bool a = _activity; _activity = false; return a; }
    uint32_t otaBytesReceived() const;
    uint32_t otaExpectedBytes() const;
    const char* deviceName() const;

private:
    bool _started;
    bool _connected;
    bool _otaInProgress;
    bool _otaAuthenticated;
    bool _otaError;
    bool _commandAuthenticated;
    volatile bool _activity = false;
    CtLoginLock _authLock;    // shared by command login and OTA start
    uint16_t _otaConnHandle;
    uint16_t _commandConnHandle;
    bool _hasDeviceCommand;
    uint32_t _lastDeviceCommandMs;
    uint32_t _otaExpected;
    uint32_t _otaReceived;
    uint32_t _rebootAt;
    String _deviceName;
    BLECommandCallback _commandCallback;

    void _sendStatus(const char* status, uint16_t connHandle);
    void _handleCommand(const String& command, uint16_t connHandle);
    bool _startOta(uint32_t size, const String& password,
                   const String& expectedSha256, uint16_t connHandle);
    void _abortOta();
    bool _finishOta();
    bool _authenticateCommand(const String& password, uint16_t connHandle);

    class ServerCallbacks;
    class CommandCallbacks;
    class DataCallbacks;

    friend class ServerCallbacks;
    friend class CommandCallbacks;
    friend class DataCallbacks;
};

extern BLEManager bleManager;

#endif
