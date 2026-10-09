#include "dbc_store.h"

#include <SD.h>
#include <SPIFFS.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ct_dbc_manifest.h"
#include "ct_dbc_validation.h"
#include "ct_sha256.h"
#include "sd_storage.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Local helpers
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

namespace {
static const char USER_MANIFEST_PATH[] = "/dbc_user.json";
static const char BUILTIN_MANIFEST_PATH[] = "/dbc/manifest.json";
static const char MANIFEST_HEADER[] = CT_DBC_MANIFEST_HEADER "\n";
static const char MANIFEST_FOOTER[] = CT_DBC_MANIFEST_FOOTER "\n";
static const size_t MANIFEST_LINE_CAPACITY = 256;

bool writeAll(File& file, const char* data, size_t length) {
    return file.write(reinterpret_cast<const uint8_t*>(data), length) == length;
}

bool readLine(File& file, char* line, size_t capacity, bool& overflow) {
    if (!line || capacity < 2) return false;
    size_t length = 0;
    overflow = false;
    bool readAny = false;
    while (file.available()) {
        int value = file.read();
        if (value < 0) break;
        readAny = true;
        if (value == '\n') break;
        if (length + 1 < capacity) line[length++] = static_cast<char>(value);
        else overflow = true;
    }
    line[length] = '\0';
    if (length && line[length - 1] == '\r') line[--length] = '\0';
    return readAny;
}

bool dbcSignalFitsLine(const char* line, uint8_t messageDlc) {
    if (!line || strncmp(line, "SG_ ", 4) != 0) return true;
    const char* colon = strchr(line, ':');
    if (!colon) return false;
    const char* p = colon + 1;
    while (*p == ' ' || *p == '\t') ++p;

    char* end = nullptr;
    unsigned long startBit = strtoul(p, &end, 10);
    if (end == p || *end != '|') return false;
    p = end + 1;
    unsigned long bitLength = strtoul(p, &end, 10);
    if (end == p || *end != '@' || startBit > 255 || bitLength > 255) return false;
    p = end + 1;
    if (*p != '0' && *p != '1') return false;
    const bool bigEndian = *p == '0';
    return ctDbcSignalFitsDlc(static_cast<uint8_t>(startBit),
                              static_cast<uint8_t>(bitLength),
                              bigEndian, messageDlc);
}

bool dbcMessageDlcFromLine(const char* line, uint8_t& dlc) {
    if (!line || strncmp(line, "BO_ ", 4) != 0) return false;
    const char* colon = strchr(line, ':');
    if (!colon) return false;
    const char* p = colon + 1;
    while (*p == ' ' || *p == '\t') ++p;
    char* end = nullptr;
    unsigned long value = strtoul(p, &end, 10);
    if (end == p || value > 8) return false;
    dlc = static_cast<uint8_t>(value);
    return true;
}
}

DbcStore dbcStore;

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Status and errors
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

void DbcStore::_setStatus(DbcStoreStatus status) {
    _status = status;
}

const char* DbcStore::errorText() const {
    switch (_status) {
        case DBC_STORE_OK: return "OK";
        case DBC_STORE_BUSY: return "Another DBC upload is already active";
        case DBC_STORE_INVALID_NAME: return "Invalid DBC file name";
        case DBC_STORE_BUILTIN_FILE: return "Built-in DBC files cannot be replaced";
        case DBC_STORE_INVALID_SIZE: return "DBC size is empty or exceeds the upload limit";
        case DBC_STORE_NO_SPACE: return "Not enough free storage space";
        case DBC_STORE_IO_ERROR: return "Storage read or write failed";
        case DBC_STORE_INVALID_CONTENT: return "DBC text, message, or signal validation failed";
        case DBC_STORE_INVALID_MANIFEST: return "User DBC manifest is invalid";
        case DBC_STORE_INTEGRITY_ERROR: return "DBC size or SHA-256 does not match its manifest";
        case DBC_STORE_NOT_FOUND: return "User DBC file was not found in its manifest";
        case DBC_STORE_CONFIRMATION_REQUIRED: return "Deleting the active DBC requires explicit confirmation";
    }
    return "Unknown DBC storage error";
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Storage lookup and listing
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

fs::FS* DbcStore::_fsFor(CtStorageLoc location) {
    if (location == CT_LOC_INTERNAL) return &SPIFFS;
    if (location == CT_LOC_SD && sdStorage.state() == SdStorage::READY) return &SD;
    return nullptr;
}

bool DbcStore::_findUserEntry(fs::FS& fs, const char* name,
                              CtDbcManifestEntry& foundEntry, bool& found) {
    found = false;
    if (!fs.exists(USER_MANIFEST_PATH)) return true;
    File manifest = fs.open(USER_MANIFEST_PATH, "r");
    if (!manifest) return false;

    char line[MANIFEST_LINE_CAPACITY];
    bool sawHeader = false;
    bool sawFooter = false;
    while (manifest.available()) {
        bool overflow = false;
        if (!readLine(manifest, line, sizeof(line), overflow) || overflow) {
            manifest.close();
            return false;
        }
        if (strcmp(line, CT_DBC_MANIFEST_HEADER) == 0) {
            if (sawHeader) {
                manifest.close();
                return false;
            }
            sawHeader = true;
            continue;
        }
        if (strcmp(line, CT_DBC_MANIFEST_FOOTER) == 0) {
            if (!sawHeader || sawFooter) {
                manifest.close();
                return false;
            }
            sawFooter = true;
            continue;
        }
        CtDbcManifestEntry candidate;
        if (!sawHeader || sawFooter ||
            !ctDbcManifestParseLine(line, candidate)) {
            manifest.close();
            return false;
        }
        if (strcmp(candidate.name, name) == 0) {
            foundEntry = candidate;
            found = true;
        }
    }
    manifest.close();
    return sawHeader && sawFooter;
}

bool DbcStore::_appendManifest(fs::FS& fs, const char* path, CtStorageLoc location,
                               CtDbcManifestEntry* entries, CtStorageLoc* locations,
                               size_t capacity, size_t& count, bool& truncated) {
    if (!fs.exists(path)) return true;
    File manifest = fs.open(path, "r");
    if (!manifest) return false;

    char line[MANIFEST_LINE_CAPACITY];
    bool sawHeader = false;
    bool sawFooter = false;
    while (manifest.available()) {
        bool overflow = false;
        if (!readLine(manifest, line, sizeof(line), overflow) || overflow) {
            manifest.close();
            return false;
        }
        if (strcmp(line, CT_DBC_MANIFEST_HEADER) == 0) {
            if (sawHeader) {
                manifest.close();
                return false;
            }
            sawHeader = true;
            continue;
        }
        if (strcmp(line, CT_DBC_MANIFEST_FOOTER) == 0) {
            if (!sawHeader || sawFooter) {
                manifest.close();
                return false;
            }
            sawFooter = true;
            continue;
        }
        CtDbcManifestEntry entry;
        if (!sawHeader || sawFooter ||
            !ctDbcManifestParseLine(line, entry)) {
            manifest.close();
            return false;
        }
        if (count < capacity) {
            entries[count] = entry;
            locations[count] = location;
            ++count;
        } else {
            truncated = true;
        }
    }
    manifest.close();
    return sawHeader && sawFooter;
}

bool DbcStore::listFiles(CtDbcManifestEntry* entries, CtStorageLoc* locations,
                         size_t capacity, size_t& count, bool& truncated) {
    _setStatus(DBC_STORE_OK);
    count = 0;
    truncated = false;
    if ((!entries || !locations) && capacity != 0) {
        _setStatus(DBC_STORE_IO_ERROR);
        return false;
    }

    if (!_appendManifest(SPIFFS, BUILTIN_MANIFEST_PATH, CT_LOC_INTERNAL,
                         entries, locations, capacity, count, truncated) ||
        !_appendManifest(SPIFFS, USER_MANIFEST_PATH, CT_LOC_INTERNAL,
                         entries, locations, capacity, count, truncated)) {
        _setStatus(DBC_STORE_INVALID_MANIFEST);
        return false;
    }
    if (sdStorage.state() == SdStorage::READY &&
        !_appendManifest(SD, USER_MANIFEST_PATH, CT_LOC_SD,
                         entries, locations, capacity, count, truncated)) {
        _setStatus(DBC_STORE_INVALID_MANIFEST);
        return false;
    }
    return true;
}

bool DbcStore::_isBuiltinName(const char* name, bool& malformed) {
    malformed = false;
    File manifest = SPIFFS.open(BUILTIN_MANIFEST_PATH, "r");
    if (!manifest) return false;

    char line[MANIFEST_LINE_CAPACITY];
    bool sawHeader = false;
    bool sawFooter = false;
    bool found = false;
    while (manifest.available()) {
        bool overflow = false;
        if (!readLine(manifest, line, sizeof(line), overflow) || overflow) {
            malformed = true;
            break;
        }
        if (strcmp(line, CT_DBC_MANIFEST_HEADER) == 0) {
            if (sawHeader) malformed = true;
            sawHeader = true;
            continue;
        }
        if (strcmp(line, CT_DBC_MANIFEST_FOOTER) == 0) {
            sawFooter = true;
            continue;
        }
        CtDbcManifestEntry entry;
        if (!sawHeader || sawFooter ||
            !ctDbcManifestParseLine(line, entry)) {
            malformed = true;
            break;
        }
        if (strcmp(entry.name, name) == 0) found = true;
    }
    manifest.close();
    if (!sawHeader || !sawFooter) malformed = true;
    return found;
}

bool DbcStore::openVerifiedUserFile(const char* name, File& file,
                                    CtStorageLoc& location, bool& isUserFile) {
    _setStatus(DBC_STORE_OK);
    isUserFile = false;
    location = CT_LOC_NONE;
    if (!ctDbcNameValid(name)) {
        _setStatus(DBC_STORE_INVALID_NAME);
        return false;
    }

    bool malformedBuiltinManifest = false;
    if (_isBuiltinName(name, malformedBuiltinManifest)) return true;
    if (malformedBuiltinManifest) return true;

    CtStorageLoc order[2] = { CT_LOC_INTERNAL, CT_LOC_SD };
    const CtStorageChoice choice = getStorageChoice("db");
    if (choice == CT_STORE_SD) {
        order[0] = CT_LOC_SD;
        order[1] = CT_LOC_INTERNAL;
    }

    for (uint8_t i = 0; i < 2; ++i) {
        fs::FS* fs = _fsFor(order[i]);
        if (!fs) continue;
        CtDbcManifestEntry entry = {};
        bool found = false;
        if (!_findUserEntry(*fs, name, entry, found)) {
            _setStatus(DBC_STORE_INVALID_MANIFEST);
            isUserFile = true;
            return false;
        }
        if (!found) continue;

        isUserFile = true;
        if (!verifyFile(name, order[i])) return false;
        char path[sizeof(CT_DBC_DIR) + CT_DBC_NAME_MAX];
        if (!ctDbcBuildPath(name, path, sizeof(path))) {
            _setStatus(DBC_STORE_INVALID_NAME);
            return false;
        }
        File opened = fs->open(path, "r");
        if (!opened) {
            _setStatus(DBC_STORE_IO_ERROR);
            return false;
        }
        file = opened;
        location = order[i];
        return true;
    }
    return true;
}

bool DbcStore::isBuiltinFile(const char* name, bool& isBuiltin) {
    _setStatus(DBC_STORE_OK);
    isBuiltin = false;
    if (!ctDbcNameValid(name)) {
        _setStatus(DBC_STORE_INVALID_NAME);
        return false;
    }
    bool malformed = false;
    isBuiltin = _isBuiltinName(name, malformed);
    if (malformed) {
        _setStatus(DBC_STORE_INVALID_MANIFEST);
        return false;
    }
    return true;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Upload pipeline
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool DbcStore::beginUpload(const char* name, uint32_t expectedBytes) {
    return _beginUpload(name, expectedBytes, expectedBytes, true);
}

bool DbcStore::beginUploadStream(const char* name, uint32_t requestBytes) {
    if (requestBytes == 0) {
        _setStatus(DBC_STORE_INVALID_SIZE);
        return false;
    }
    const uint32_t requiredFreeBytes = requestBytes > CT_DBC_MAX_BYTES
        ? CT_DBC_MAX_BYTES : requestBytes;
    return _beginUpload(name, CT_DBC_MAX_BYTES, requiredFreeBytes, false);
}

bool DbcStore::_beginUpload(const char* name, uint32_t maxUploadBytes,
                            uint32_t requiredFreeBytes, bool sizeKnown) {
    if (_uploadActive) {
        _setStatus(DBC_STORE_BUSY);
        return false;
    }
    _lastLocation = CT_LOC_NONE;
    _setStatus(DBC_STORE_OK);
    if (!ctDbcNameValid(name)) {
        _setStatus(DBC_STORE_INVALID_NAME);
        return false;
    }
    strncpy(_uploadName, name, sizeof(_uploadName) - 1);
    _uploadName[sizeof(_uploadName) - 1] = '\0';
    if (!ctDbcSizeInRange(maxUploadBytes) || !ctDbcSizeInRange(requiredFreeBytes)) {
        _setStatus(DBC_STORE_INVALID_SIZE);
        return false;
    }

    bool malformed = false;
    if (_isBuiltinName(name, malformed)) {
        _setStatus(DBC_STORE_BUILTIN_FILE);
        return false;
    }
    if (malformed) {
        _setStatus(DBC_STORE_INVALID_MANIFEST);
        return false;
    }

    const uint64_t internalTotal = SPIFFS.totalBytes();
    const uint64_t internalUsed = SPIFFS.usedBytes();
    const bool internalOk = internalTotal > 0;
    const uint64_t internalFree64 = internalOk && internalTotal > internalUsed
        ? internalTotal - internalUsed : 0;
    const uint64_t sdFree64 = sdStorage.freeBytes();
    const bool sdOk = sdStorage.state() == SdStorage::READY;
    const uint32_t internalFree = internalFree64 > 0xFFFFFFFFull
        ? 0xFFFFFFFFu : static_cast<uint32_t>(internalFree64);
    const uint32_t sdFreeWithReserve = sdFree64 > CT_DBC_RESERVE_BYTES
        ? (sdFree64 - CT_DBC_RESERVE_BYTES > 0xFFFFFFFFull
            ? 0xFFFFFFFFu : static_cast<uint32_t>(sdFree64 - CT_DBC_RESERVE_BYTES)) : 0;
    const CtStorageDecision decision = ctResolveStorage(
        getStorageChoice("db"), internalOk, internalFree, sdOk,
        sdFreeWithReserve, requiredFreeBytes);
    if (decision.loc == CT_LOC_NONE) {
        _setStatus(DBC_STORE_NO_SPACE);
        return false;
    }

    fs::FS* selectedFs = _fsFor(decision.loc);
    if (!selectedFs) {
        _setStatus(DBC_STORE_IO_ERROR);
        return false;
    }
    char path[sizeof(CT_DBC_DIR) + CT_DBC_NAME_MAX];
    if (!ctDbcBuildPath(name, path, sizeof(path))) {
        _setStatus(DBC_STORE_INVALID_NAME);
        return false;
    }
    if (decision.loc == CT_LOC_SD && !selectedFs->exists("/dbc") &&
        !selectedFs->mkdir("/dbc")) {
        _setStatus(DBC_STORE_IO_ERROR);
        return false;
    }

    char tempPath[sizeof(path) + 5];
    char backupPath[sizeof(path) + 5];
    snprintf(tempPath, sizeof(tempPath), "%s.tmp", path);
    snprintf(backupPath, sizeof(backupPath), "%s.bak", path);
    _location = decision.loc;
    if (!_recoverManifest(*selectedFs) ||
        !_recoverFile(*selectedFs, path, backupPath)) {
        _location = CT_LOC_NONE;
        _setStatus(DBC_STORE_INTEGRITY_ERROR);
        return false;
    }
    selectedFs->remove(tempPath);
    _uploadFile = selectedFs->open(tempPath, "w");
    if (!_uploadFile) {
        _location = CT_LOC_NONE;
        _setStatus(DBC_STORE_IO_ERROR);
        return false;
    }

    _expectedBytes = maxUploadBytes;
    _receivedBytes = 0;
    _lineLength = 0;
    _currentDlc = 0;
    _lineHasMessage = false;
    _lineTooLong = false;
    _sizeKnown = sizeKnown;
    ctDbcScanInit(_scan);
    ctSha256Init(_sha);
    _uploadActive = true;
    return true;
}

bool DbcStore::_scanUploadLine() {
    if (_lineTooLong) return false;
    _line[_lineLength] = '\0';
    char* record = reinterpret_cast<char*>(_line);
    while (*record == ' ' || *record == '\t') ++record;
    ctDbcScanLine(_scan, record);

    if (strncmp(record, "BO_ ", 4) == 0) {
        if (!dbcMessageDlcFromLine(record, _currentDlc)) return false;
        _lineHasMessage = true;
    } else if (strncmp(record, "SG_ ", 4) == 0) {
        if (!_lineHasMessage || !dbcSignalFitsLine(record, _currentDlc)) return false;
    }
    _lineLength = 0;
    return true;
}

size_t DbcStore::writeChunk(const uint8_t* data, size_t length) {
    if (!_uploadActive) {
        _setStatus(DBC_STORE_IO_ERROR);
        return 0;
    }
    if ((!data && length != 0) ||
        length > static_cast<size_t>(_expectedBytes - _receivedBytes)) {
        abortUpload();
        _setStatus(DBC_STORE_INVALID_SIZE);
        return 0;
    }
    if (length == 0) return 0;

    if (_uploadFile.write(data, length) != length) {
        abortUpload();
        _setStatus(DBC_STORE_IO_ERROR);
        return 0;
    }
    ctSha256Update(_sha, data, length);
    _receivedBytes += static_cast<uint32_t>(length);

    for (size_t i = 0; i < length; ++i) {
        const uint8_t byte = data[i];
        if (byte == '\n') {
            if (!_scanUploadLine()) {
                abortUpload();
                _setStatus(DBC_STORE_INVALID_CONTENT);
                return 0;
            }
            continue;
        }
        if (byte < 0x20 && byte != '\t' && byte != '\r') _scan.badBytes = true;
        if (static_cast<size_t>(_lineLength) + 1 >= sizeof(_line)) {
            _lineTooLong = true;
            abortUpload();
            _setStatus(DBC_STORE_INVALID_CONTENT);
            return 0;
        }
        _line[_lineLength++] = byte;
    }
    return length;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Manifest handling
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool DbcStore::_writeManifestEntry(File& file, const CtDbcManifestEntry& entry) {
    char line[MANIFEST_LINE_CAPACITY];
    const size_t length = ctDbcManifestFormatLine(entry, line, sizeof(line));
    return length > 0 && writeAll(file, line, length) && writeAll(file, "\n", 1);
}

bool DbcStore::_updateManifest(const CtDbcManifestEntry& entry) {
    fs::FS* fs = _fsFor(_location);
    if (!fs) return false;
    const char* path = USER_MANIFEST_PATH;
    const char* tmpPath = "/dbc_user.json.tmp";
    const char* bakPath = "/dbc_user.json.bak";
    if (!_recoverManifest(*fs)) return false;
    fs->remove(tmpPath);
    File output = fs->open(tmpPath, "w");
    if (!output || !writeAll(output, MANIFEST_HEADER, sizeof(MANIFEST_HEADER) - 1)) {
        if (output) output.close();
        fs->remove(tmpPath);
        return false;
    }

    bool keepGoing = true;
    if (fs->exists(path)) {
        File input = fs->open(path, "r");
        if (!input) keepGoing = false;
        bool sawHeader = false;
        bool sawFooter = false;
        char line[MANIFEST_LINE_CAPACITY];
        while (keepGoing && input.available()) {
            bool overflow = false;
            if (!readLine(input, line, sizeof(line), overflow) || overflow) {
                keepGoing = false;
                break;
            }
            if (strcmp(line, CT_DBC_MANIFEST_HEADER) == 0) {
                if (sawHeader) keepGoing = false;
                sawHeader = true;
                continue;
            }
            if (strcmp(line, CT_DBC_MANIFEST_FOOTER) == 0) {
                if (!sawHeader || sawFooter) keepGoing = false;
                sawFooter = true;
                continue;
            }
            CtDbcManifestEntry oldEntry;
            if (!sawHeader || sawFooter ||
                !ctDbcManifestParseLine(line, oldEntry)) {
                keepGoing = false;
                break;
            }
            if (strcmp(oldEntry.name, entry.name) != 0 &&
                !_writeManifestEntry(output, oldEntry)) keepGoing = false;
        }
        if (input) input.close();
        if (!sawHeader || !sawFooter) keepGoing = false;
    }
    if (keepGoing) keepGoing = _writeManifestEntry(output, entry);
    if (keepGoing) keepGoing = writeAll(output, MANIFEST_FOOTER, sizeof(MANIFEST_FOOTER) - 1);
    output.flush();
    output.close();
    if (!keepGoing) {
        fs->remove(tmpPath);
        return false;
    }

    fs->remove(bakPath);
    const bool hadManifest = fs->exists(path);
    if (hadManifest && !fs->rename(path, bakPath)) {
        fs->remove(tmpPath);
        return false;
    }
    if (!fs->rename(tmpPath, path)) {
        if (hadManifest) fs->rename(bakPath, path);
        fs->remove(tmpPath);
        return false;
    }
    fs->remove(bakPath);
    return true;
}

bool DbcStore::_removeManifestEntry(fs::FS& fs, const char* name) {
    const char* path = USER_MANIFEST_PATH;
    const char* tmpPath = "/dbc_user.json.tmp";
    const char* bakPath = "/dbc_user.json.bak";
    if (!_recoverManifest(fs)) return false;
    File input = fs.open(path, "r");
    if (!input) return false;
    fs.remove(tmpPath);
    File output = fs.open(tmpPath, "w");
    if (!output || !writeAll(output, MANIFEST_HEADER, sizeof(MANIFEST_HEADER) - 1)) {
        input.close();
        if (output) output.close();
        fs.remove(tmpPath);
        return false;
    }

    bool valid = true;
    bool found = false;
    bool sawHeader = false;
    bool sawFooter = false;
    char line[MANIFEST_LINE_CAPACITY];
    while (input.available() && valid) {
        bool overflow = false;
        if (!readLine(input, line, sizeof(line), overflow) || overflow) {
            valid = false;
            break;
        }
        if (strcmp(line, CT_DBC_MANIFEST_HEADER) == 0) {
            if (sawHeader) valid = false;
            sawHeader = true;
            continue;
        }
        if (strcmp(line, CT_DBC_MANIFEST_FOOTER) == 0) {
            if (!sawHeader || sawFooter) valid = false;
            sawFooter = true;
            continue;
        }
        CtDbcManifestEntry entry;
        if (!sawHeader || sawFooter ||
            !ctDbcManifestParseLine(line, entry)) {
            valid = false;
            break;
        }
        if (strcmp(entry.name, name) == 0) {
            found = true;
            continue;
        }
        if (!_writeManifestEntry(output, entry)) valid = false;
    }
    input.close();
    if (!sawHeader || !sawFooter || !found) valid = false;
    if (valid) valid = writeAll(output, MANIFEST_FOOTER, sizeof(MANIFEST_FOOTER) - 1);
    output.flush();
    output.close();
    if (!valid) {
        fs.remove(tmpPath);
        return false;
    }

    fs.remove(bakPath);
    if (!fs.rename(path, bakPath)) {
        fs.remove(tmpPath);
        return false;
    }
    if (!fs.rename(tmpPath, path)) {
        fs.rename(bakPath, path);
        fs.remove(tmpPath);
        return false;
    }
    fs.remove(bakPath);
    return true;
}

bool DbcStore::deleteUserFile(const char* name, CtStorageLoc location,
                              bool isActive, bool confirmed) {
    _setStatus(DBC_STORE_OK);
    if (!ctDbcNameValid(name)) {
        _setStatus(DBC_STORE_INVALID_NAME);
        return false;
    }
    bool malformed = false;
    if (_isBuiltinName(name, malformed)) {
        _setStatus(DBC_STORE_BUILTIN_FILE);
        return false;
    }
    if (malformed) {
        _setStatus(DBC_STORE_INVALID_MANIFEST);
        return false;
    }
    const CtDbcDeleteDecision decision = ctDbcDeleteDecision(false, isActive, confirmed);
    if (decision == CT_DBC_DEL_NEEDS_CONFIRM) {
        _setStatus(DBC_STORE_CONFIRMATION_REQUIRED);
        return false;
    }

    fs::FS* fs = _fsFor(location);
    if (!fs) {
        _setStatus(DBC_STORE_IO_ERROR);
        return false;
    }
    char path[sizeof(CT_DBC_DIR) + CT_DBC_NAME_MAX];
    char backupPath[sizeof(path) + 5];
    if (!ctDbcBuildPath(name, path, sizeof(path))) {
        _setStatus(DBC_STORE_INVALID_NAME);
        return false;
    }
    snprintf(backupPath, sizeof(backupPath), "%s.bak", path);
    _location = location;
    strncpy(_uploadName, name, sizeof(_uploadName) - 1);
    _uploadName[sizeof(_uploadName) - 1] = '\0';
    if (!_recoverManifest(*fs) || !_recoverFile(*fs, path, backupPath)) {
        _location = CT_LOC_NONE;
        _setStatus(DBC_STORE_INTEGRITY_ERROR);
        return false;
    }
    CtDbcManifestEntry entry = {};
    bool found = false;
    if (!_findUserEntry(*fs, name, entry, found)) {
        _location = CT_LOC_NONE;
        _setStatus(DBC_STORE_INVALID_MANIFEST);
        return false;
    }
    if (!found) {
        _location = CT_LOC_NONE;
        _setStatus(DBC_STORE_NOT_FOUND);
        return false;
    }
    if (!fs->exists(path) || !fs->rename(path, backupPath)) {
        _location = CT_LOC_NONE;
        _setStatus(DBC_STORE_IO_ERROR);
        return false;
    }
    if (!_removeManifestEntry(*fs, name)) {
        if (fs->exists(backupPath)) fs->rename(backupPath, path);
        _location = CT_LOC_NONE;
        _setStatus(DBC_STORE_INVALID_MANIFEST);
        return false;
    }
    if (!fs->remove(backupPath)) {
        _location = CT_LOC_NONE;
        _setStatus(DBC_STORE_IO_ERROR);
        return false;
    }
    _location = CT_LOC_NONE;
    return true;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Crash recovery and file replace
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool DbcStore::_recoverManifest(fs::FS& fs) {
    const bool hasManifest = fs.exists(USER_MANIFEST_PATH);
    const bool hasBackup = fs.exists("/dbc_user.json.bak");
    if (!hasManifest && hasBackup) return fs.rename("/dbc_user.json.bak", USER_MANIFEST_PATH);
    if (hasManifest && hasBackup) return fs.remove("/dbc_user.json.bak");
    return true;
}

bool DbcStore::_recoverFile(fs::FS& fs, const char* path, const char* bakPath) {
    if (!fs.exists(bakPath)) return true;
    if (!fs.exists(path)) {
        const char* name = strrchr(path, '/');
        name = name ? name + 1 : path;
        CtDbcManifestEntry entry = {};
        bool tracked = false;
        if (!_findUserEntry(fs, name, entry, tracked)) return false;
        return tracked ? fs.rename(bakPath, path) : fs.remove(bakPath);
    }

    if (verifyFile(_uploadName, _location)) return fs.remove(bakPath);
    if (_status != DBC_STORE_INTEGRITY_ERROR) return false;
    if (!fs.remove(path) || !fs.rename(bakPath, path)) return false;
    return verifyFile(_uploadName, _location);
}

bool DbcStore::_replaceFile(fs::FS& fs, const char* path, const char* tmpPath,
                            const char* bakPath, bool& hadOriginal) {
    fs.remove(bakPath);
    hadOriginal = fs.exists(path);
    if (hadOriginal && !fs.rename(path, bakPath)) return false;
    if (!fs.rename(tmpPath, path)) {
        if (hadOriginal) fs.rename(bakPath, path);
        return false;
    }
    return true;
}

void DbcStore::_restoreFile(fs::FS& fs, const char* path, const char* bakPath,
                            bool hadOriginal) {
    if (fs.exists(path)) fs.remove(path);
    if (hadOriginal && fs.exists(bakPath)) fs.rename(bakPath, path);
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Finish, abort and verify
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool DbcStore::finishUpload() {
    if (!_uploadActive) {
        _setStatus(DBC_STORE_IO_ERROR);
        return false;
    }
    if ((_sizeKnown && _receivedBytes != _expectedBytes) ||
        !ctDbcSizeInRange(_receivedBytes)) {
        abortUpload();
        _setStatus(DBC_STORE_INVALID_SIZE);
        return false;
    }
    if (_lineLength != 0 && !_scanUploadLine()) {
        abortUpload();
        _setStatus(DBC_STORE_INVALID_CONTENT);
        return false;
    }
    const CtDbcResult scanResult = ctDbcScanVerdict(_scan);
    if (scanResult != CT_DBC_OK) {
        abortUpload();
        _setStatus(DBC_STORE_INVALID_CONTENT);
        return false;
    }
    _uploadFile.flush();
    _uploadFile.close();

    CtDbcManifestEntry entry = {};
    char path[sizeof(CT_DBC_DIR) + CT_DBC_NAME_MAX];
    char tempPath[sizeof(path) + 5];
    char backupPath[sizeof(path) + 5];
    if (!ctDbcBuildPath(_uploadName, path, sizeof(path))) {
        abortUpload();
        _setStatus(DBC_STORE_INVALID_NAME);
        return false;
    }
    strcpy(entry.name, _uploadName);
    entry.size = _receivedBytes;
    entry.messages = _scan.messages;
    strcpy(entry.time, "unknown");
    strcpy(entry.source, "user");
    strcpy(entry.license, "unverified");
    ctSha256FinishHex(_sha, entry.sha256);
    snprintf(tempPath, sizeof(tempPath), "%s.tmp", path);
    snprintf(backupPath, sizeof(backupPath), "%s.bak", path);

    fs::FS* fs = _fsFor(_location);
    if (!fs) {
        abortUpload();
        _setStatus(DBC_STORE_IO_ERROR);
        return false;
    }
    bool hadOriginal = false;
    if (!_replaceFile(*fs, path, tempPath, backupPath, hadOriginal)) {
        abortUpload();
        _setStatus(DBC_STORE_IO_ERROR);
        return false;
    }
    if (!_updateManifest(entry)) {
        _restoreFile(*fs, path, backupPath, hadOriginal);
        _setStatus(DBC_STORE_INVALID_MANIFEST);
        _uploadActive = false;
        return false;
    }
    fs->remove(backupPath);
    _lastLocation = _location;
    _uploadActive = false;
    _location = CT_LOC_NONE;
    _expectedBytes = 0;
    _receivedBytes = 0;
    _sizeKnown = true;
    _setStatus(DBC_STORE_OK);
    return true;
}

void DbcStore::abortUpload() {
    if (_uploadFile) {
        _uploadFile.close();
    }
    if (_location != CT_LOC_NONE) {
        fs::FS* fs = _fsFor(_location);
        if (fs) {
            char path[sizeof(CT_DBC_DIR) + CT_DBC_NAME_MAX];
            if (ctDbcBuildPath(_uploadName, path, sizeof(path))) {
                char tempPath[sizeof(path) + 5];
                snprintf(tempPath, sizeof(tempPath), "%s.tmp", path);
                fs->remove(tempPath);
            }
        }
    }
    _uploadActive = false;
    _location = CT_LOC_NONE;
    _expectedBytes = 0;
    _receivedBytes = 0;
    _sizeKnown = true;
    _lineLength = 0;
}

bool DbcStore::verifyFile(const char* name, CtStorageLoc location) {
    _setStatus(DBC_STORE_OK);
    if (!ctDbcNameValid(name)) {
        _setStatus(DBC_STORE_INVALID_NAME);
        return false;
    }
    fs::FS* fs = _fsFor(location);
    if (!fs) {
        _setStatus(DBC_STORE_IO_ERROR);
        return false;
    }

    File manifest = fs->open(USER_MANIFEST_PATH, "r");
    if (!manifest) {
        _setStatus(DBC_STORE_INVALID_MANIFEST);
        return false;
    }
    bool found = false;
    CtDbcManifestEntry expected = {};
    char line[MANIFEST_LINE_CAPACITY];
    bool sawHeader = false;
    bool sawFooter = false;
    while (manifest.available()) {
        bool overflow = false;
        if (!readLine(manifest, line, sizeof(line), overflow) || overflow) {
            manifest.close();
            _setStatus(DBC_STORE_INVALID_MANIFEST);
            return false;
        }
        if (strcmp(line, CT_DBC_MANIFEST_HEADER) == 0) {
            if (sawHeader) {
                manifest.close();
                _setStatus(DBC_STORE_INVALID_MANIFEST);
                return false;
            }
            sawHeader = true;
            continue;
        }
        if (strcmp(line, CT_DBC_MANIFEST_FOOTER) == 0) {
            if (!sawHeader || sawFooter) {
                manifest.close();
                _setStatus(DBC_STORE_INVALID_MANIFEST);
                return false;
            }
            sawFooter = true;
            continue;
        }
        CtDbcManifestEntry entry;
        if (!sawHeader || sawFooter || !ctDbcManifestParseLine(line, entry)) {
            manifest.close();
            _setStatus(DBC_STORE_INVALID_MANIFEST);
            return false;
        }
        if (strcmp(entry.name, name) == 0) {
            expected = entry;
            found = true;
        }
    }
    manifest.close();
    if (!sawHeader || !sawFooter || !found) {
        _setStatus(DBC_STORE_INVALID_MANIFEST);
        return false;
    }

    char path[sizeof(CT_DBC_DIR) + CT_DBC_NAME_MAX];
    if (!ctDbcBuildPath(name, path, sizeof(path))) {
        _setStatus(DBC_STORE_INVALID_NAME);
        return false;
    }
    File file = fs->open(path, "r");
    if (!file) {
        _setStatus(DBC_STORE_INTEGRITY_ERROR);
        return false;
    }
    CtSha256 sha;
    ctSha256Init(sha);
    uint8_t buffer[512];
    uint32_t total = 0;
    while (file.available()) {
        int readCount = file.read(buffer, sizeof(buffer));
        if (readCount <= 0) {
            file.close();
            _setStatus(DBC_STORE_IO_ERROR);
            return false;
        }
        ctSha256Update(sha, buffer, static_cast<size_t>(readCount));
        total += static_cast<uint32_t>(readCount);
    }
    file.close();
    char actualHash[65];
    ctSha256FinishHex(sha, actualHash);
    if (total != expected.size || !ctSha256HexEqual(actualHash, expected.sha256)) {
        _setStatus(DBC_STORE_INTEGRITY_ERROR);
        return false;
    }
    return true;
}
