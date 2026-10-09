#include "ble_manager.h"

#include <NimBLEDevice.h>
#include <Update.h>
#include "ct_password.h"
#include "ct_ota_header.h"
#include "ct_ota_lock.h"
#include "ct_ota_authenticity.h"
#include "ct_credentials.h"
#include "ct_sha256.h"
#include "ct_index_parser.h"
#include "sd_storage.h"
#include "config.h"
#include "ct_can_record.h"
#include "ct_time.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ BLE globals
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

namespace {
static const char* BLE_SERVICE_UUID  = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* BLE_STATUS_UUID   = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* BLE_COMMAND_UUID  = "6E400004-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* BLE_DATA_UUID     = "6E400005-B5A3-F393-E0A9-E50E24DCCA9E";

BLECharacteristic* gStatus = nullptr;
BLECharacteristic* gCommand = nullptr;
BLECharacteristic* gData = nullptr;
BLEServer* gServer = nullptr;
CtOtaHeaderCheck gOtaHeader;  // header collected across BLE writes
CtSha256 gOtaSha256;
char gOtaExpectedSha256[65] = {};
bool gOtaHashMismatch = false;
BLEManager* gManager = nullptr;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ GATT callbacks
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

class BLEManager::ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
        if (gManager) {
            gManager->_connected = true;
            gManager->_activity = true;
            gManager->_commandAuthenticated = false;
            gManager->_commandConnHandle = info.getConnHandle();
        }
        if (gStatus) gStatus->setValue("CONNECTED");
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int) override {
        if (gManager) {
            if (gManager->_commandConnHandle == info.getConnHandle()) {
                gManager->_commandAuthenticated = false;
                gManager->_commandConnHandle = BLE_HS_CONN_HANDLE_NONE;
            }
            if (gManager->_otaInProgress &&
                gManager->_otaConnHandle == info.getConnHandle()) {
                gManager->_abortOta();
            }
            gManager->_connected = gServer && gServer->getConnectedCount() > 0;
        }
        if (gServer) gServer->startAdvertising();
    }
};

class BLEManager::CommandCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
        if (!gManager) return;
        const std::string value = characteristic->getValue();
        if (value.size() > 128) {
            gManager->_sendStatus("COMMAND_TOO_LONG", info.getConnHandle());
            return;
        }
        gManager->_handleCommand(String(value.c_str()), info.getConnHandle());
    }
};

class BLEManager::DataCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
        if (!gManager || !gManager->_otaInProgress || !gManager->_otaAuthenticated ||
            gManager->_otaConnHandle != info.getConnHandle()) return;

        std::string value = characteristic->getValue();
        if (value.empty()) return;

        if (!gOtaHeader.done) {
            const CtOtaHdrResult hr = ctOtaHeaderFeed(
                gOtaHeader, reinterpret_cast<const uint8_t*>(value.data()), value.size(),
                ESP.getFlashChipSize());
            if (hr != CT_OTA_HDR_OK && hr != CT_OTA_HDR_NEED_MORE) {
                gManager->_otaError = true;
                gManager->_abortOta();
                gManager->_sendStatus(hr == CT_OTA_HDR_WRONG_CHIP    ? "OTA_WRONG_CHIP"
                                      : hr == CT_OTA_HDR_FLASH_TOO_BIG ? "OTA_WRONG_FLASH_SIZE"
                                                                       : "OTA_BAD_HEADER",
                                      info.getConnHandle());
                return;
            }
        }
        if (gManager->_otaReceived + value.size() > gManager->_otaExpected) {
            gManager->_otaError = true;
            gManager->_abortOta();
            gManager->_sendStatus("OTA_TOO_MUCH_DATA", info.getConnHandle());
            return;
        }

        size_t written = Update.write(reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())), value.size());
        if (written != value.size()) {
            gManager->_otaError = true;
            gManager->_abortOta();
            gManager->_sendStatus("OTA_WRITE_ERROR", info.getConnHandle());
            return;
        }

        gManager->_otaReceived += static_cast<uint32_t>(written);
        ctSha256Update(gOtaSha256,
                       reinterpret_cast<const uint8_t*>(value.data()), written);
        if (gStatus) {
            String status = "OTA_PROGRESS:" + String(gManager->_otaReceived) + ":" + String(gManager->_otaExpected);
            gStatus->notify(reinterpret_cast<const uint8_t*>(status.c_str()),
                            status.length(), info.getConnHandle());
        }
    }
};

BLEManager bleManager;

BLEManager::BLEManager()
    : _started(false), _connected(false), _otaInProgress(false),
      _otaAuthenticated(false), _otaError(false), _commandAuthenticated(false),
    _otaConnHandle(BLE_HS_CONN_HANDLE_NONE),
    _commandConnHandle(BLE_HS_CONN_HANDLE_NONE), _hasDeviceCommand(false),
    _lastDeviceCommandMs(0), _otaExpected(0),
      _otaReceived(0), _rebootAt(0), _deviceName("CarTouch"),
      _commandCallback(nullptr) {}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Lifecycle and status
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

void BLEManager::setCommandCallback(BLECommandCallback callback) {
    _commandCallback = callback;
}

void BLEManager::publishStatus(const char* status) {
    if (_commandAuthenticated && _connected) {
        _sendStatus(status ? status : "STATUS_UNAVAILABLE", _commandConnHandle);
    }
}

bool BLEManager::begin() {
    if (_started) return true;

    uint64_t chipId = ESP.getEfuseMac();
    char name[32];
    snprintf(name, sizeof(name), "CarTouch-%04X", static_cast<unsigned>(chipId & 0xFFFF));
    _deviceName = name;

    NimBLEDevice::init(_deviceName.c_str());
    NimBLEDevice::setPower(9);
    // Encrypt BLE links. Pairing uses Just Works because the device has no
    // dedicated keyboard/display for a pairing PIN; the OTA command still
    // requires the current Web password at the application layer.
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM (Just Works), LE Secure Connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    gManager = this;
    gServer = NimBLEDevice::createServer();
    gServer->setCallbacks(new ServerCallbacks());

    NimBLEService* service = gServer->createService(BLE_SERVICE_UUID);
    gStatus = service->createCharacteristic(BLE_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    gCommand = service->createCharacteristic(BLE_COMMAND_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
    gData = service->createCharacteristic(BLE_DATA_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);

    gCommand->setCallbacks(new CommandCallbacks());
    gData->setCallbacks(new DataCallbacks());
    gStatus->setValue("READY");

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    // A BLE advertising packet holds only 31 bytes. Flags (3) + a 128-bit
    // service UUID (18) + the name "CarTouch-XXXX" (15) = 36 bytes, which is
    // too big: the radio can refuse to advertise while begin() still looks
    // successful (the result was never checked), so the phone never sees the
    // device. Only the name goes in the packet; the service is still found
    // after connecting.
    advertising->setName(_deviceName.c_str());
    advertising->start();

    if (!advertising->isAdvertising()) {
        Serial.println("[BLE] ERROR: advertising did not start - device is not visible");
        return false;
    }

    _started = true;
    Serial.printf("[BLE] Started: %s\n", _deviceName.c_str());
    return true;
}

void BLEManager::update() {
    if (_rebootAt != 0 && static_cast<int32_t>(millis() - _rebootAt) >= 0) {
        Serial.println("[BLE OTA] Rebooting to apply firmware update...");
        delay(100);
        ESP.restart();
    }
}

void BLEManager::_sendStatus(const char* status, uint16_t connHandle) {
    if (!gStatus || !_connected || connHandle == BLE_HS_CONN_HANDLE_NONE) return;
    gStatus->notify(reinterpret_cast<const uint8_t*>(status), strlen(status), connHandle);
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Command handling
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool BLEManager::_authenticateCommand(const String& password, uint16_t connHandle) {
    if (isUsingDefaultPassword()) return false;
    if (ctLoginLocked(_authLock, millis())) return false;

    const char* expected = getConfig()->webPass;
    const size_t expectedLength = strlen(expected);
    const size_t passwordLength = password.length();
    uint8_t diff = (uint8_t)(expectedLength != passwordLength);
    for (size_t i = 0; i < expectedLength; ++i) {
        diff |= (uint8_t)(expected[i] ^ (i < passwordLength ? password[i] : 0));
    }
    if (diff == 0) {
        _commandAuthenticated = true;
        _commandConnHandle = connHandle;
        ctLoginSucceeded(_authLock);
        return true;
    }

    ctLoginFailed(_authLock, millis());
    return false;
}

void BLEManager::_handleCommand(const String& command, uint16_t connHandle) {
    String cmd = command;
    cmd.trim();
    _activity = true;

    if (cmd.equalsIgnoreCase("STATUS")) {
        String status = "READY:" + String(_otaInProgress ? "OTA" : "IDLE");
        _sendStatus(status.c_str(), connHandle);
        return;
    }

    if (cmd.length() >= 5 && cmd.substring(0, 5).equalsIgnoreCase("AUTH:")) {   // keyword is case-insensitive; the password is not
        if (_authenticateCommand(cmd.substring(5), connHandle)) _sendStatus("COMMAND_AUTHENTICATED", connHandle);
        else _sendStatus(ctLoginLocked(_authLock, millis()) ? "COMMAND_LOCKED" : "COMMAND_AUTH_FAILED", connHandle);
        return;
    }

    if (cmd.equalsIgnoreCase("LOGOUT")) {
        if (_commandConnHandle == connHandle) _commandAuthenticated = false;
        _sendStatus("COMMAND_LOGGED_OUT", connHandle);
        return;
    }

    if (cmd.equalsIgnoreCase("DTC:READ") || cmd.equalsIgnoreCase("DTC:CLEAR") ||
        cmd.equalsIgnoreCase("DTC:STATUS")) {
        if (!_commandAuthenticated || _commandConnHandle != connHandle) {
            _sendStatus("COMMAND_AUTH_REQUIRED", connHandle);
            return;
        }
        if (!_commandCallback) {
            _sendStatus("COMMAND_UNAVAILABLE", connHandle);
            return;
        }
        if (_hasDeviceCommand &&
            !ctElapsedAtLeast(millis(), _lastDeviceCommandMs, 300)) {
            _sendStatus("COMMAND_RATE_LIMITED", connHandle);
            return;
        }
        _hasDeviceCommand = true;
        _lastDeviceCommandMs = millis();
        _commandCallback(cmd.equalsIgnoreCase("DTC:READ") ? "dtc_read" :
            cmd.equalsIgnoreCase("DTC:CLEAR") ? "dtc_clear" : "dtc_status");
        _sendStatus("COMMAND_QUEUED", connHandle);
        return;
    }

    if (cmd.startsWith("RECORD:")) {
        if (!_commandAuthenticated || _commandConnHandle != connHandle) {
            _sendStatus("COMMAND_AUTH_REQUIRED", connHandle);
            return;
        }
        if (!_commandCallback) {
            _sendStatus("COMMAND_UNAVAILABLE", connHandle);
            return;
        }
        if (_hasDeviceCommand &&
            !ctElapsedAtLeast(millis(), _lastDeviceCommandMs, 300)) {
            _sendStatus("COMMAND_RATE_LIMITED", connHandle);
            return;
        }
        _hasDeviceCommand = true;
        _lastDeviceCommandMs = millis();
        String action = cmd.substring(7);
        if (action.equalsIgnoreCase("START:1")) _commandCallback("record_start:1");
        else if (action.equalsIgnoreCase("START:2")) _commandCallback("record_start:2");
        else if (action.equalsIgnoreCase("START:BOTH")) _commandCallback("record_start:3");
        else if (action.equalsIgnoreCase("STOP")) _commandCallback("record_stop");
        else if (action.equalsIgnoreCase("STATUS")) _commandCallback("record_status");
        else if (action.startsWith("DELETE:") &&
                 ctCanRecordFilenameValid(action.substring(7).c_str())) {
            const String fileName = action.substring(7);
            const String queued = "record_delete:" + fileName;
            _commandCallback(queued.c_str());
        } else {
            _sendStatus("COMMAND_BAD_REQUEST", connHandle);
            return;
        }
        _sendStatus("COMMAND_QUEUED", connHandle);
        return;
    }

    if (cmd.startsWith("SD:") || cmd.startsWith("sd:")) {
        if (!_commandAuthenticated || _commandConnHandle != connHandle) {
            _sendStatus("COMMAND_AUTH_REQUIRED", connHandle);
            return;
        }
        // SD:STATUS | SD:CS:<gpio|-1> | SD:STORE:<db|rec|prof|bak>:<auto|internal|sd> | SD:RESET
        String arg = cmd.substring(3);
        if (arg.equalsIgnoreCase("STATUS")) {
            String r = "SD:" + String(sdStorage.stateText()) + ":CS=" + String(sdStorage.csPin()) +
                       ":FREE=" + String((uint32_t)(sdStorage.freeBytes() / 1024)) + "K";
            _sendStatus(r.c_str(), connHandle);
        } else if (arg.equalsIgnoreCase("RESET")) {
            resetStorageChoices();
            _sendStatus("SD_OK", connHandle);
        } else if (arg.startsWith("CS:") || arg.startsWith("cs:")) {
            const String v = arg.substring(3);
            char* end = nullptr;
            const long pin = strtol(v.c_str(), &end, 10);
            const bool ok = end != v.c_str() && *end == '\0' && pin >= -1 && pin <= 48 &&
                            sdStorage.setCsPin((int)pin);
            _sendStatus(ok ? "SD_OK" : "SD_REJECTED", connHandle);
        } else if (arg.startsWith("STORE:") || arg.startsWith("store:")) {
            const String rest = arg.substring(6);
            const int sep = rest.indexOf(':');
            bool ok = false;
            if (sep > 0) {
                const String ch = rest.substring(sep + 1);
                const uint8_t v = ch.equalsIgnoreCase("auto") ? CT_STORE_AUTO :
                                  ch.equalsIgnoreCase("internal") ? CT_STORE_INTERNAL :
                                  ch.equalsIgnoreCase("sd") ? CT_STORE_SD : 255;
                String cat = rest.substring(0, sep);
                cat.toLowerCase();
                ok = setStorageChoice(cat.c_str(), v);
            }
            _sendStatus(ok ? "SD_OK" : "SD_REJECTED", connHandle);
        } else {
            _sendStatus("COMMAND_BAD_REQUEST", connHandle);
        }
        return;
    }

    if (cmd.equalsIgnoreCase("ABORT")) {
        const bool ownsOta = _otaInProgress && _otaAuthenticated &&
                             _otaConnHandle == connHandle;
        const bool ownsCommandSession = _commandAuthenticated &&
                                        _commandConnHandle == connHandle;
        if ((_otaInProgress && !ownsOta) || (!_otaInProgress && !ownsCommandSession)) {
            _sendStatus("COMMAND_AUTH_REQUIRED", connHandle);
            return;
        }
        _abortOta();
        _sendStatus("OTA_ABORTED", connHandle);
        return;
    }

    if (cmd.equalsIgnoreCase("END")) {
        if (!_otaInProgress || !_otaAuthenticated || _otaConnHandle != connHandle) {
            _sendStatus("OTA_NOT_STARTED", connHandle);
            return;
        }
        if (_otaReceived != _otaExpected) {
            _sendStatus("OTA_SIZE_MISMATCH", connHandle);
            _abortOta();
            return;
        }
        if (!_finishOta()) {
            _sendStatus(gOtaHashMismatch ? "OTA_SHA256_MISMATCH" : "OTA_FINALIZE_ERROR",
                        connHandle);
            gOtaHashMismatch = false;
            return;
        }
        _sendStatus("OTA_OK_REBOOTING", connHandle);
        _rebootAt = millis() + 1500;
        return;
    }

    if (cmd.length() >= 6 && cmd.substring(0, 6).equalsIgnoreCase("START:")) {   // keyword is case-insensitive; the password is not
        int first = cmd.indexOf(':');
        const int hashSeparator = cmd.lastIndexOf(':');
        const int sizeSeparator = hashSeparator > 0
            ? cmd.lastIndexOf(':', hashSeparator - 1) : -1;
        if (first < 0 || sizeSeparator <= first || hashSeparator <= sizeSeparator) {
            _sendStatus("OTA_BAD_COMMAND", connHandle);
            return;
        }

        const String expectedHash = cmd.substring(hashSeparator + 1);
        if (!ctSha256HexValid(expectedHash.c_str())) {
            _sendStatus("OTA_BAD_SHA256", connHandle);
            return;
        }
        if (isUsingDefaultPassword()) {
            _sendStatus("OTA_CHANGE_DEFAULT_PASSWORD", connHandle);
            return;
        }
        String password = cmd.substring(first + 1, sizeSeparator);
        const String sizeText = cmd.substring(sizeSeparator + 1, hashSeparator);
        uint32_t size = 0;
        if (!ctParseUnsignedDecimal(sizeText.c_str(), UINT32_MAX, size) || size == 0) {
            _sendStatus("OTA_BAD_SIZE", connHandle);
            return;
        }
        if (!_startOta(size, password, expectedHash, connHandle)) {
            _sendStatus(_otaError ? "OTA_AUTH_OR_START_ERROR" : "OTA_START_ERROR", connHandle);
        } else {
            _sendStatus("OTA_STARTED", connHandle);
        }
        return;
    }

    _sendStatus("UNKNOWN_COMMAND", connHandle);
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ OTA over BLE
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool BLEManager::_startOta(uint32_t size, const String& password,
                           const String& expectedSha256,
                           uint16_t connHandle) {
    if (!ctOtaLock().tryAcquire(CT_OTA_OWNER_BLE)) {
        _otaError = true;
        return false;
    }
    _otaError = false;
    _otaAuthenticated = false;
    _otaExpected = 0;
    _otaReceived = 0;
    gOtaHashMismatch = false;

    const AppConfig* cfg = getConfig();
    if (isUsingDefaultPassword()) {  // never allow OTA with the public default
        _otaError = true;
        ctOtaLock().release(CT_OTA_OWNER_BLE);
        return false;
    }
    if (ctLoginLocked(_authLock, millis())) {
        _otaError = true;  // locked out after repeated failures
        ctOtaLock().release(CT_OTA_OWNER_BLE);
        return false;
    }
    {
        // constant-time comparison
        const char* expected = cfg->webPass;
        size_t el = strlen(expected), pl = password.length();
        uint8_t diff = (uint8_t)(el != pl);
        for (size_t i = 0; i < el; ++i) diff |= (uint8_t)(expected[i] ^ (i < pl ? password[i] : 0));
        if (diff != 0) {
            _otaError = true;
            ctLoginFailed(_authLock, millis());
            ctOtaLock().release(CT_OTA_OWNER_BLE);
            return false;
        }
        ctLoginSucceeded(_authLock);
    }

    if (_otaInProgress || Update.isRunning()) {
        _otaError = true;
        ctOtaLock().release(CT_OTA_OWNER_BLE);
        return false;
    }

    gOtaHeader.reset();
    if (!Update.begin(size, U_FLASH)) {
        _otaError = true;
        Serial.printf("[BLE OTA] Update.begin failed: %s\n", Update.errorString());
        ctOtaLock().release(CT_OTA_OWNER_BLE);
        return false;
    }

    strncpy(gOtaExpectedSha256, expectedSha256.c_str(), sizeof(gOtaExpectedSha256) - 1);
    gOtaExpectedSha256[sizeof(gOtaExpectedSha256) - 1] = '\0';
    ctSha256Init(gOtaSha256);
    _otaExpected = size;
    _otaReceived = 0;
    _otaAuthenticated = true;
    _otaInProgress = true;
    _otaConnHandle = connHandle;
    Serial.printf("[BLE OTA] Started: %u bytes\n", (unsigned)size);
    return true;
}

void BLEManager::_abortOta() {
    if (Update.isRunning()) Update.abort();
    gOtaHeader.reset();
    gOtaExpectedSha256[0] = '\0';
    ctSha256Init(gOtaSha256);
    _otaInProgress = false;
    _otaAuthenticated = false;
    _otaExpected = 0;
    _otaReceived = 0;
    _otaConnHandle = BLE_HS_CONN_HANDLE_NONE;
    ctOtaLock().release(CT_OTA_OWNER_BLE);
}

bool BLEManager::_finishOta() {
    if (!Update.isRunning()) return false;
    if (_otaReceived != _otaExpected) {
        Serial.printf("[BLE OTA] Size mismatch: got %u of %u bytes\n",
                      (unsigned)_otaReceived, (unsigned)_otaExpected);
        _otaError = true;
        _abortOta();
        return false;
    }
    if (!gOtaHeader.done) {    // image shorter than its own header
        Serial.println("[BLE OTA] Image too short");
        _otaError = true;
        _abortOta();
        return false;
    }
    char actualHash[65];
    ctSha256FinishHex(gOtaSha256, actualHash);
    if (!ctSha256HexEqual(actualHash, gOtaExpectedSha256)) {
        Serial.println("[BLE OTA] SHA-256 mismatch");
        _otaError = true;
        gOtaHashMismatch = true;
        _abortOta();
        return false;
    }
    // BLE has no signature channel yet: personal mode passes, product mode
    // fails closed (no signature supplied) until one is added.
    if (!ctOtaVerifyAuthenticity(CT_PRODUCT_MODE != 0, actualHash, nullptr,
                                 ctOtaVerifySignature)) {
        Serial.println("[BLE OTA] Signature required");
        _otaError = true;
        _abortOta();
        return false;
    }
    if (!Update.end(true)) {
        Serial.printf("[BLE OTA] Finalize failed: %s\n", Update.errorString());
        _otaError = true;
        _abortOta();
        return false;
    }

    _otaInProgress = false;
    _otaAuthenticated = false;
    _otaConnHandle = BLE_HS_CONN_HANDLE_NONE;
    gOtaExpectedSha256[0] = '\0';
    Serial.printf("[BLE OTA] Finished successfully: %u bytes\n", (unsigned)_otaReceived);
    ctOtaLock().release(CT_OTA_OWNER_BLE);
    return true;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Accessors
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool BLEManager::isEnabled() const { return _started; }
bool BLEManager::isConnected() const { return _connected; }
bool BLEManager::isOtaInProgress() const { return _otaInProgress; }
uint32_t BLEManager::otaBytesReceived() const { return _otaReceived; }
uint32_t BLEManager::otaExpectedBytes() const { return _otaExpected; }
const char* BLEManager::deviceName() const { return _deviceName.c_str(); }
