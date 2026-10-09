#ifndef DBC_STORE_H
#define DBC_STORE_H

#include <FS.h>
#include <stdint.h>
#include "ct_dbc_store.h"
#include "ct_dbc_manifest.h"
#include "ct_sha256.h"
#include "ct_storage_policy.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Status codes
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

enum DbcStoreStatus : uint8_t {
    DBC_STORE_OK = 0,
    DBC_STORE_BUSY,
    DBC_STORE_INVALID_NAME,
    DBC_STORE_BUILTIN_FILE,
    DBC_STORE_INVALID_SIZE,
    DBC_STORE_NO_SPACE,
    DBC_STORE_IO_ERROR,
    DBC_STORE_INVALID_CONTENT,
    DBC_STORE_INVALID_MANIFEST,
    DBC_STORE_INTEGRITY_ERROR,
    DBC_STORE_NOT_FOUND,
    DBC_STORE_CONFIRMATION_REQUIRED
};

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ DbcStore class
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

class DbcStore {
public:
    bool beginUpload(const char* name, uint32_t expectedBytes);
    bool beginUploadStream(const char* name, uint32_t requestBytes);
    size_t writeChunk(const uint8_t* data, size_t length);
    bool finishUpload();
    void abortUpload();

    bool verifyFile(const char* name, CtStorageLoc location);
    bool isBuiltinFile(const char* name, bool& isBuiltin);
    bool openVerifiedUserFile(const char* name, File& file, CtStorageLoc& location,
                              bool& isUserFile);
    bool listFiles(CtDbcManifestEntry* entries, CtStorageLoc* locations,
                   size_t capacity, size_t& count, bool& truncated);
    bool deleteUserFile(const char* name, CtStorageLoc location,
                        bool isActive, bool confirmed);
    DbcStoreStatus status() const { return _status; }
    const char* errorText() const;
    bool uploadActive() const { return _uploadActive; }
    CtStorageLoc uploadLocation() const { return _uploadActive ? _location : _lastLocation; }

private:
    bool _beginUpload(const char* name, uint32_t maxUploadBytes,
                      uint32_t requiredFreeBytes, bool sizeKnown);
    bool _isBuiltinName(const char* name, bool& malformed);
    bool _findUserEntry(fs::FS& fs, const char* name, CtDbcManifestEntry& entry,
                        bool& found);
    bool _appendManifest(fs::FS& fs, const char* path, CtStorageLoc location,
                         CtDbcManifestEntry* entries, CtStorageLoc* locations,
                         size_t capacity, size_t& count, bool& truncated);
    bool _removeManifestEntry(fs::FS& fs, const char* name);
    bool _scanUploadLine();
    bool _writeManifestEntry(File& file, const CtDbcManifestEntry& entry);
    bool _updateManifest(const CtDbcManifestEntry& entry);
    bool _recoverManifest(fs::FS& fs);
    bool _recoverFile(fs::FS& fs, const char* path, const char* bakPath);
    bool _replaceFile(fs::FS& fs, const char* path, const char* tmpPath,
                      const char* bakPath, bool& hadOriginal);
    void _restoreFile(fs::FS& fs, const char* path, const char* bakPath,
                      bool hadOriginal);
    fs::FS* _fsFor(CtStorageLoc location);
    void _setStatus(DbcStoreStatus status);

    File _uploadFile;
    CtSha256 _sha = {};
    CtDbcScan _scan = {};
    CtStorageLoc _location = CT_LOC_NONE;
    CtStorageLoc _lastLocation = CT_LOC_NONE;
    DbcStoreStatus _status = DBC_STORE_OK;
    char _uploadName[CT_DBC_NAME_MAX + 1] = {};
    uint32_t _expectedBytes = 0;
    uint32_t _receivedBytes = 0;
    uint16_t _lineLength = 0;
    uint8_t _line[4096] = {};
    uint8_t _currentDlc = 0;
    bool _uploadActive = false;
    bool _lineHasMessage = false;
    bool _lineTooLong = false;
    bool _sizeKnown = true;
};

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Global instance
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

extern DbcStore dbcStore;

#endif
