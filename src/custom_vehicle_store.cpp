/**
 * custom_vehicle_store.cpp - Custom profile storage implementation
 */

#include "custom_vehicle_store.h"
#include <SPIFFS.h>
#include <cstring>
#include <cstdio>
#include <ArduinoJson.h>
#include "ct_json_validation.h"

#define CUSTOM_VEHICLES_DIR "/custom_vehicles"
#define INDEX_FILE_PATH     "/custom_vehicles/index.json"

// SPIFFS (default CONFIG_SPIFFS_OBJ_NAME_LEN = 32) rejects any path longer than
// 31 characters, including the leading '/'. Every profile file is also written
// as "<path>.tmp" / "<path>.bak" by _atomicWriteJSON(), so the *longest* derived
// name must still fit. (The legacy "profile_N.json" naming made ".tmp"/".bak"
// 35 characters long, so every profile save failed on real hardware.)
#define CVS_PROFILE_PREFIX         "/custom_vehicles/p"
#define CVS_LEGACY_PROFILE_PREFIX  "/custom_vehicles/profile_"
#define CVS_SPIFFS_MAX_PATH        31

static_assert(MAX_CUSTOM_VEHICLES <= 10,
              "Profile file names assume a single-digit slot index");
static_assert((sizeof(CVS_PROFILE_PREFIX) - 1) + 1 + (sizeof(".json") - 1) +
              (sizeof(".tmp") - 1) <= CVS_SPIFFS_MAX_PATH,
              "Profile .tmp/.bak file name exceeds the SPIFFS 31-character path limit");
static_assert((sizeof(INDEX_FILE_PATH) - 1) + (sizeof(".tmp") - 1) <= CVS_SPIFFS_MAX_PATH,
              "Index .tmp/.bak file name exceeds the SPIFFS 31-character path limit");

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Constructor
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

CustomVehicleStore::CustomVehicleStore() {
    _initialized = false;
    for (int i = 0; i < MAX_CUSTOM_VEHICLES; i++) {
        _summaryCache[i].inUse = false;
    }
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Init
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

// SPIFFS keeps no file index: every exists()/remove() of a name that is not
// there walks the whole flash (about 0.3 s each on a 6 MB partition). The old
// boot code did ~60 such lookups and took ~17 s. Instead, list the folder ONCE
// and remember which of this store's files really exist.
struct CvsBootScan {
    bool legacy[MAX_CUSTOM_VEHICLES];   // profile_N.json (old naming)
    bool leftover[MAX_CUSTOM_VEHICLES + 1];  // slot N (last = index): .tmp or .bak present
    bool anyFile;                       // any store file at all
};

static bool cvsBaseNameIs(const char* base, const char* prefix, uint8_t slot, const char* suffix) {
    char expected[40];
    snprintf(expected, sizeof(expected), "%s%u%s", prefix, (unsigned)slot, suffix);
    return strcmp(base, expected) == 0;
}

// Returns false if the folder could not be listed (caller then uses the
// slower per-file checks, which behave exactly as before).
static bool cvsScanFiles(CvsBootScan& scan) {
    memset(&scan, 0, sizeof(scan));
    File root = SPIFFS.open("/");
    if (!root) return false;

    File entry = root.openNextFile();
    while (entry) {
        const char* full = entry.name();
        const char* slash = strrchr(full, '/');
        const char* base = slash ? slash + 1 : full;

        if (strcmp(base, "index.json") == 0) {
            scan.anyFile = true;
        } else if (strcmp(base, "index.json.tmp") == 0 || strcmp(base, "index.json.bak") == 0) {
            scan.anyFile = true;
            scan.leftover[MAX_CUSTOM_VEHICLES] = true;
        } else {
            for (uint8_t i = 0; i < MAX_CUSTOM_VEHICLES; ++i) {
                if (cvsBaseNameIs(base, "p", i, ".json")) {
                    scan.anyFile = true;
                } else if (cvsBaseNameIs(base, "p", i, ".json.tmp") ||
                           cvsBaseNameIs(base, "p", i, ".json.bak")) {
                    scan.anyFile = true;
                    scan.leftover[i] = true;
                } else if (cvsBaseNameIs(base, "profile_", i, ".json")) {
                    scan.anyFile = true;
                    scan.legacy[i] = true;
                }
            }
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    return true;
}

bool CustomVehicleStore::begin() {
    // SPIFFS on ESP32 has no real directories (it's flat), so there is
    // nothing to create: files are opened by their full path.
    CvsBootScan scan;
    bool scanned = cvsScanFiles(scan);
    // Safety check: an empty listing is only trusted if one direct lookup
    // agrees. If the listing ever misses files, fall back to the slow
    // per-file checks instead of hiding saved profiles.
    if (scanned && !scan.anyFile && SPIFFS.exists(INDEX_FILE_PATH)) scanned = false;

    // Fresh device: no custom profile files exist, so there is nothing to
    // migrate, recover or rebuild. Skip all of it (this is the common case
    // and used to cost ~17 s at every boot).
    if (scanned && !scan.anyFile) {
        _initialized = true;
        Serial.println("[CVS] No custom profile files - nothing to load");
        Serial.printf("[CVS] CustomVehicleStore ready - %d profile(s) found\n", getProfileCount());
        return true;
    }

    // One-time migration from the legacy "profile_N.json" names (31 chars, so
    // they could not be journaled) to the short "pN.json" names.
    bool migrated = false;
    for (uint8_t i = 0; i < MAX_CUSTOM_VEHICLES; ++i) {
        if (scanned && !scan.legacy[i]) continue;
        const String legacyPath = String(CVS_LEGACY_PROFILE_PREFIX) + String(i) + ".json";
        if (!SPIFFS.exists(legacyPath)) continue;
        if (SPIFFS.exists(_profilePath(i))) SPIFFS.remove(legacyPath);
        else SPIFFS.rename(legacyPath, _profilePath(i));
        migrated = true;
    }
    if (migrated && scanned) cvsScanFiles(scan);   // names changed: list again

    // Crash recovery is only needed when an interrupted save left a .tmp or
    // .bak file behind; with no leftovers it would only remove files that are
    // not there.
    if (!scanned || scan.leftover[MAX_CUSTOM_VEHICLES]) _recoverAtomicFile(INDEX_FILE_PATH);
    for (uint8_t i = 0; i < MAX_CUSTOM_VEHICLES; ++i) {
        if (!scanned || scan.leftover[i]) _recoverAtomicFile(_profilePath(i));
    }

    bool result = _loadIndex();
    _initialized = result;

    Serial.printf("[CVS] CustomVehicleStore %s - %d profile(s) found\n",
                  result ? "ready" : "failed", getProfileCount());

    return result;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Profile file path
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

String CustomVehicleStore::_profilePath(uint8_t index) {
    return String(CVS_PROFILE_PREFIX) + String(index) + ".json";
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Index load / save
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

bool CustomVehicleStore::_loadIndex() {
    // Profile files are authoritative; index.json is only a derived cache.
    // Always reconcile the cache against every bounded profile slot at boot so
    // a stale-but-valid index can never hide or resurrect a profile.
    for (uint8_t i = 0; i < MAX_CUSTOM_VEHICLES; ++i) {
        memset(&_summaryCache[i], 0, sizeof(CustomVehicleProfile));
        _summaryCache[i].id = i;
    }
    if (!_rebuildIndexFromProfiles()) {
        // No profile files is a valid empty store. Remove a stale index rather
        // than treating it as authoritative.
        if (SPIFFS.exists(INDEX_FILE_PATH)) SPIFFS.remove(INDEX_FILE_PATH);
    }
    return true;
}

bool CustomVehicleStore::_rebuildIndexFromProfiles() {
    bool foundAny = false;
    for (uint8_t i = 0; i < MAX_CUSTOM_VEHICLES; ++i) {
        String path = _profilePath(i);
        if (!SPIFFS.exists(path)) continue;

        CustomVehicleProfile profile;
        if (!_readProfileFile(i, profile)) {
            Serial.printf("[CVS] Ignoring invalid profile file %s during index rebuild\n", path.c_str());
            continue;
        }
        if (profile.id != i) {
            Serial.printf("[CVS] Ignoring profile %s: embedded id %u does not match slot %u\n",
                          path.c_str(), (unsigned)profile.id, (unsigned)i);
            continue;
        }

        _summaryCache[i] = profile;
        _summaryCache[i].inUse = true;
        foundAny = true;
    }

    if (foundAny) {
        // Recreate the derived index only after all authoritative profile files
        // have been validated. If this write fails, the profiles themselves
        // remain intact and the next boot can rebuild again.
        if (!_saveIndex()) {
            Serial.println("[CVS] Profile recovery succeeded but index rewrite failed");
        }
    }
    return foundAny;
}

bool CustomVehicleStore::_atomicWriteJSON(const String& path, JsonDocument& doc) {
    const String tmpPath = path + ".tmp";
    const String bakPath = path + ".bak";

    // Journal the previous committed file before promoting the new file.
    // If power fails between these operations, begin() can recover the
    // backup or a validated temporary JSON instead of losing the profile.
    SPIFFS.remove(tmpPath);
    File tmpFile = SPIFFS.open(tmpPath, "w");
    if (!tmpFile) return false;
    const size_t written = serializeJson(doc, tmpFile);
    tmpFile.flush();
    tmpFile.close();
    if (written == 0) { SPIFFS.remove(tmpPath); return false; }

    SPIFFS.remove(bakPath);
    if (SPIFFS.exists(path) && !SPIFFS.rename(path, bakPath)) {
        SPIFFS.remove(tmpPath);
        return false;
    }
    if (!SPIFFS.rename(tmpPath, path)) {
        if (SPIFFS.exists(bakPath)) SPIFFS.rename(bakPath, path);
        return false;
    }
    SPIFFS.remove(bakPath);
    return true;
}

void CustomVehicleStore::_recoverAtomicFile(const String& path) {
    const String tmpPath = path + ".tmp";
    const String bakPath = path + ".bak";
    if (SPIFFS.exists(path)) { SPIFFS.remove(tmpPath); SPIFFS.remove(bakPath); return; }
    if (SPIFFS.exists(bakPath) && SPIFFS.rename(bakPath, path)) {
        SPIFFS.remove(tmpPath);
        return;
    }
    if (SPIFFS.exists(tmpPath)) {
        File tmp = SPIFFS.open(tmpPath, "r");
        bool valid = false;
        if (tmp) { JsonDocument check; valid = !deserializeJson(check, tmp); tmp.close(); }
        if (valid && SPIFFS.rename(tmpPath, path)) return;
        SPIFFS.remove(tmpPath);
    }
    SPIFFS.remove(bakPath);
}

bool CustomVehicleStore::_saveIndex() {
    JsonDocument doc;
    JsonArray arr = doc["profiles"].to<JsonArray>();

    for (int i = 0; i < MAX_CUSTOM_VEHICLES; i++) {
        if (!_summaryCache[i].inUse) continue;

        JsonObject item = arr.add<JsonObject>();
        item["id"]              = _summaryCache[i].id;
        item["name"]               = _summaryCache[i].name;
        item["brand"]                  = _summaryCache[i].brand;
        item["model"]                     = _summaryCache[i].model;
        item["year"]                          = _summaryCache[i].year;
        item["revision"]                       = _summaryCache[i].revision;
        item["commandCount"]                     = _summaryCache[i].commandCount;
    }

    return _atomicWriteJSON(INDEX_FILE_PATH, doc);
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Profile <-> JSON conversion
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

void CustomVehicleStore::_profileToJson(const CustomVehicleProfile& profile, JsonDocument& doc) {
    doc["id"]      = profile.id;
    doc["name"]       = profile.name;
    doc["brand"]          = profile.brand;
    doc["model"]              = profile.model;
    if (profile.dbcFileName[0]) doc["dbcFile"] = profile.dbcFileName;
    doc["year"]                   = profile.year;
    doc["revision"]               = profile.revision;

    JsonArray cmds = doc["commands"].to<JsonArray>();
    for (int i = 0; i < profile.commandCount; i++) {
        const LearnedCommand& c    = profile.commands[i];
        JsonObject            item = cmds.add<JsonObject>();
        item["label"]           = c.label;
        item["displayName"]        = c.displayName;
        item["canId"]                  = c.canId;
        item["extended"]                   = c.isExtended;
        item["length"]                          = c.length;

        JsonArray dataArr = item["data"].to<JsonArray>();
        for (int b = 0; b < c.length && b < 8; b++) {
            dataArr.add(c.data[b]);
        }

        item["source"]           = (int)c.source;
        item["actuatorClass"]    = (int)c.actuatorClass;
        item["status"]              = (c.status == CMD_VERIFIED) ? "verified" : "unverified";
        item["timesObserved"]           = c.timesObserved;
        item["failCount"]                  = c.failCount;
        item["createdAt"]                     = c.createdAt;
    }
}

bool CustomVehicleStore::_jsonToProfile(JsonDocument& doc, CustomVehicleProfile& profile,
                                        bool strict) {
    const char* nameStr  = doc["name"]  | "";
    const char* brandStr = doc["brand"] | "";
    const char* modelStr = doc["model"] | "";
    JsonVariant dbcFileValue = doc["dbcFile"];
    if (strict && !dbcFileValue.isNull() && !dbcFileValue.is<const char*>()) {
        Serial.println("[CVS] Reject import: 'dbcFile' must be a string");
        return false;
    }
    const char* dbcFileStr = dbcFileValue | "";
    int         yearVal  = doc["year"]  | 0;
    uint32_t    revisionVal = doc["revision"] | 0;

    if (strict) {
        if (strlen(nameStr) == 0 || strlen(nameStr) >= sizeof(profile.name)) {
            Serial.println("[CVS] Reject import: 'name' missing or too long"); return false;
        }
        if (strlen(brandStr) >= sizeof(profile.brand) ||
            strlen(modelStr) >= sizeof(profile.model)) {
            Serial.println("[CVS] Reject import: 'brand'/'model' too long"); return false;
        }
        if (yearVal < 0 || yearVal > 9999) {
            Serial.println("[CVS] Reject import: 'year' out of range"); return false;
        }
    }
    if (strlen(dbcFileStr) >= sizeof(profile.dbcFileName) ||
        (dbcFileStr[0] && !ctDbcNameValid(dbcFileStr))) {
        Serial.println("[CVS] Reject profile: 'dbcFile' is invalid"); return false;
    }

    profile.id = doc["id"] | 0;
    memset(profile.name,  0, sizeof(profile.name));
    memset(profile.brand, 0, sizeof(profile.brand));
    memset(profile.model, 0, sizeof(profile.model));
    memset(profile.dbcFileName, 0, sizeof(profile.dbcFileName));
    strncpy(profile.name,  nameStr,  sizeof(profile.name)  - 1);
    strncpy(profile.brand, brandStr, sizeof(profile.brand) - 1);
    strncpy(profile.model, modelStr, sizeof(profile.model) - 1);
    strncpy(profile.dbcFileName, dbcFileStr, sizeof(profile.dbcFileName) - 1);
    profile.year  = (uint16_t)yearVal;
    profile.revision = revisionVal;
    profile.inUse = true;
    profile.commandCount = 0;

    if (strict && !doc["commands"].is<JsonArray>()) {
        Serial.println("[CVS] Reject import: 'commands' array missing or not an array");
        return false;
    }
    JsonArray cmds = doc["commands"].as<JsonArray>();

    if (strict && cmds.size() > MAX_LEARNED_COMMANDS_PER_VEHICLE) {
        Serial.printf("[CVS] Reject import: %u commands exceed cap %u\n",
                      (unsigned)cmds.size(), (unsigned)MAX_LEARNED_COMMANDS_PER_VEHICLE);
        return false;
    }

    for (JsonObject item : cmds) {
        if (profile.commandCount >= MAX_LEARNED_COMMANDS_PER_VEHICLE) {
            Serial.println("[CVS] Command count exceeded the cap - remaining skipped");
            break;
        }
        const char* labelStr = item["label"] | "";
        CtJsonCommandFields validatedFields;
        if (strict && !ctValidateImportedCommand(item, validatedFields)) {
            Serial.println("[CVS] Reject import: command fields failed strict validation");
            return false;
        }
        if (strict) {
            if (strlen(labelStr) == 0) {
                Serial.println("[CVS] Reject import: empty 'label'"); return false;
            }
            if (strlen(labelStr) >= sizeof(profile.commands[0].label)) {
                Serial.println("[CVS] Reject import: 'label' too long"); return false;
            }
            for (uint8_t k = 0; k < profile.commandCount; k++) {
                if (strcmp(profile.commands[k].label, labelStr) == 0) {
                    Serial.printf("[CVS] Reject import: duplicate label '%s'\n", labelStr);
                    return false;
                }
            }
        }

        if (strict) {
            if (!item["canId"].is<int>()) {
                Serial.printf("[CVS] Reject import: 'canId' must be an integer for '%s'\n", labelStr);
                return false;
            }
            if (!item["extended"].is<bool>()) {
                Serial.printf("[CVS] Reject import: 'extended' must be boolean for '%s'\n", labelStr);
                return false;
            }
            if (!item["length"].is<int>()) {
                Serial.printf("[CVS] Reject import: 'length' must be an integer for '%s'\n", labelStr);
                return false;
            }
        }

        int canIdRaw = item["canId"] | -1;
        uint32_t canIdVal = (uint32_t)canIdRaw;
        bool extendedVal = item["extended"] | false;
        if (strict && (canIdRaw < 0 || canIdVal > (extendedVal ? 0x1FFFFFFFu : 0x7FFu))) {
            Serial.printf("[CVS] Reject import: 'canId' out of range for '%s'\n", labelStr);
            return false;
        }

        int lenVal = item["length"] | (strict ? -1 : 0);
        if (strict && (lenVal < 0 || lenVal > 8)) {
            Serial.printf("[CVS] Reject import: 'length' out of range for '%s'\n", labelStr);
            return false;
        }
        if (!strict && lenVal > 8) lenVal = 8;

        JsonArray dataArr = item["data"].as<JsonArray>();
        if (strict) {
            if (item["data"].isNull()) {
                if (lenVal > 0) {
                    Serial.printf("[CVS] Reject import: missing 'data' for '%s'\n", labelStr);
                    return false;
                }
            } else if (!item["data"].is<JsonArray>()) {
                Serial.printf("[CVS] Reject import: 'data' not an array for '%s'\n", labelStr);
                return false;
            } else if ((int)dataArr.size() != lenVal) {
                Serial.printf("[CVS] Reject import: 'data' size %u != length %d for '%s'\n",
                              (unsigned)dataArr.size(), lenVal, labelStr);
                return false;
            }
            if (dataArr.size() > 8) {
                Serial.printf("[CVS] Reject import: 'data' >8 for '%s'\n", labelStr);
                return false;
            }
            for (JsonVariant v : dataArr) {
                if (!v.is<int>() || v.as<int>() < 0 || v.as<int>() > 255) {
                    Serial.printf("[CVS] Reject import: invalid byte for '%s'\n", labelStr);
                    return false;
                }
            }
        }

        const char* displayNameStr = item["displayName"] | "";
        if (strict && strlen(displayNameStr) >= sizeof(profile.commands[0].displayName)) {
            Serial.printf("[CVS] Reject import: 'displayName' too long for '%s'\n", labelStr);
            return false;
        }

        if (strict && !item["source"].is<int>()) {
            Serial.printf("[CVS] Reject import: 'source' must be an integer for '%s'\n", labelStr);
            return false;
        }
        int sourceVal = item["source"] | (int)SOURCE_MANUAL;
        if (strict && (sourceVal < (int)SOURCE_DBC || sourceVal > (int)SOURCE_MANUAL)) {
            Serial.printf("[CVS] Reject import: invalid 'source' for '%s'\n", labelStr);
            return false;
        }

        const char* statusStr = item["status"] | "unverified";
        if (strict && !item["status"].is<const char*>()) {
            Serial.printf("[CVS] Reject import: 'status' must be a string for '%s'\n", labelStr);
            return false;
        }
        if (strict && strcmp(statusStr, "verified") != 0 &&
            strcmp(statusStr, "unverified") != 0) {
            Serial.printf("[CVS] Reject import: unknown 'status' for '%s'\n", labelStr);
            return false;
        }

        LearnedCommand& cc = profile.commands[profile.commandCount];
        memset(&cc, 0, sizeof(cc));
        strncpy(cc.label, labelStr, sizeof(cc.label) - 1);
        strncpy(cc.displayName, displayNameStr, sizeof(cc.displayName) - 1);
        cc.canId      = canIdVal;
        cc.isExtended = extendedVal;
        cc.length     = (uint8_t)lenVal;

        int bi = 0;
        for (JsonVariant v : dataArr) { if (bi >= 8) break; cc.data[bi++] = v.as<uint8_t>(); }

        cc.source = (CommandSource)sourceVal;
        // Actuator class: a profile saved before this field existed keeps its
        // standard commands usable (documented class per label); a custom label
        // without the field stays UNKNOWN and is refused (fail-closed). An
        // out-of-range value is also UNKNOWN. Rules: ct_command_types.h.
        cc.actuatorClass = ctCommandActuatorFromStored(
            !item["actuatorClass"].isNull(),
            item["actuatorClass"] | (int)COMMAND_ACTUATOR_UNKNOWN,
            cc.label);
        // Status: this device's own flash (strict == false) keeps the stored
        // "verified", otherwise every verification would be forgotten at the
        // next read. An import (strict == true) is NEVER trusted as verified;
        // importProfileJSON() also downgrades it explicitly.
        cc.status = ctCommandStatusFromStored(statusStr, /*trustedStorage=*/!strict);

        cc.timesObserved = item["timesObserved"] | 0;
        cc.failCount     = item["failCount"] | 0;
        cc.createdAt     = item["createdAt"] | 0;
        profile.commandCount++;
    }

    if (strict && profile.commandCount == 0) {
        Serial.println("[CVS] Reject import: profile contains no commands"); return false;
    }
    return true;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Profile file I/O
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

bool CustomVehicleStore::_readProfileFile(uint8_t index, CustomVehicleProfile& outProfile) {
    String path = _profilePath(index);
    if (!SPIFFS.exists(path)) {
        Serial.printf("[CVS] Profile file not found: %s\n", path.c_str());
        return false;
    }

    File file = SPIFFS.open(path, "r");
    if (!file) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        Serial.printf("[CVS] Failed to parse profile %d: %s\n", index, err.c_str());
        return false;
    }

    return _jsonToProfile(doc, outProfile);
}

bool CustomVehicleStore::_writeProfileFile(const CustomVehicleProfile& profile) {
    JsonDocument doc;
    _profileToJson(profile, doc);

    String path = _profilePath(profile.id);
    return _atomicWriteJSON(path, doc);
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Profile count / summary
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

uint8_t CustomVehicleStore::getProfileCount() {
    if (!_initialized) return 0;
    uint8_t count = 0;
    for (int i = 0; i < MAX_CUSTOM_VEHICLES; i++) {
        if (_summaryCache[i].inUse) count++;
    }
    return count;
}

bool CustomVehicleStore::getProfileSummary(uint8_t index, CustomVehicleProfile& outSummary) {
    if (!_initialized || index >= MAX_CUSTOM_VEHICLES || !_summaryCache[index].inUse) return false;
    outSummary = _summaryCache[index];
    return true;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Load / save
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool CustomVehicleStore::loadProfile(uint8_t index, CustomVehicleProfile& outProfile) {
    if (!_initialized || index >= MAX_CUSTOM_VEHICLES || !_summaryCache[index].inUse) return false;
    if (!_readProfileFile(index, outProfile)) return false;
    // Keep the summary generation synchronized with the authoritative profile
    // file, including profiles created before the revision field existed.
    _summaryCache[index].revision = outProfile.revision;
    return true;
}

bool CustomVehicleStore::saveProfile(const CustomVehicleProfile& profile) {
    if (!_initialized || profile.id >= MAX_CUSTOM_VEHICLES) return false;

    CustomVehicleProfile toSave = profile;
    uint32_t nextRevision = _summaryCache[profile.id].inUse
        ? (_summaryCache[profile.id].revision + 1u)
        : 1u;
    if (nextRevision == 0) nextRevision = 1;
    toSave.revision = nextRevision;

    CustomVehicleProfile previous;
    const bool hadPrevious = _summaryCache[toSave.id].inUse && loadProfile(toSave.id, previous);
    CustomVehicleProfile oldSummary = _summaryCache[toSave.id];
    if (!_writeProfileFile(toSave)) return false;

    // Commit the derived index only after the authoritative profile file is valid.
    _summaryCache[toSave.id] = toSave;
    _summaryCache[toSave.id].inUse = true;
    if (_saveIndex()) return true;

    // Roll back both the file and the in-RAM cache if the derived index cannot commit.
    bool restored = false;
    if (hadPrevious) restored = _writeProfileFile(previous);
    else { String path = _profilePath(toSave.id); if (SPIFFS.exists(path)) SPIFFS.remove(path); restored = true; }
    _summaryCache[toSave.id] = oldSummary;
    if (!restored) Serial.printf("[CVS] CRITICAL: profile %u changed but rollback failed\n", (unsigned)toSave.id);
    _saveIndex();
    return false;
}

bool CustomVehicleStore::setDbcFileName(uint8_t profileIndex, const char* fileName) {
    if (!_initialized || profileIndex >= MAX_CUSTOM_VEHICLES ||
        !_summaryCache[profileIndex].inUse || !fileName) return false;
    const size_t length = strlen(fileName);
    if (length > CT_DBC_NAME_MAX ||
        (length != 0 && !ctDbcNameValid(fileName))) return false;

    CustomVehicleProfile profile;
    if (!loadProfile(profileIndex, profile)) return false;
    memset(profile.dbcFileName, 0, sizeof(profile.dbcFileName));
    memcpy(profile.dbcFileName, fileName, length);
    return saveProfile(profile);
}

bool CustomVehicleStore::referencesDbcFile(const char* fileName, bool& referenced) {
    referenced = false;
    if (!_initialized || !ctDbcNameValid(fileName)) return false;
    for (uint8_t i = 0; i < MAX_CUSTOM_VEHICLES; ++i) {
        if (!_summaryCache[i].inUse) continue;
        CustomVehicleProfile profile;
        if (!loadProfile(i, profile)) return false;
        if (strcmp(profile.dbcFileName, fileName) == 0) referenced = true;
    }
    return true;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Create new profile
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

bool CustomVehicleStore::createNewProfile(const char* name, const char* brand,
                                          const char* model, uint16_t year,
                                          uint8_t& outIndex) {
    if (!_initialized) return false;
    int freeSlot = -1;
    for (int i = 0; i < MAX_CUSTOM_VEHICLES; i++) {
        if (!_summaryCache[i].inUse) {
            freeSlot = i;
            break;
        }
    }

    if (freeSlot < 0) {
        Serial.printf("[CVS] Custom profile capacity full (max %d)\n",
                     (int)MAX_CUSTOM_VEHICLES);
        return false;
    }

    CustomVehicleProfile profile;
    profile.id = (uint8_t)freeSlot;
    profile.inUse = true;
    strncpy(profile.name, name ? name : "New Vehicle", sizeof(profile.name) - 1);
    strncpy(profile.brand, brand ? brand : "", sizeof(profile.brand) - 1);
    strncpy(profile.model, model ? model : "", sizeof(profile.model) - 1);
    profile.year          = year;
    profile.commandCount     = 0;

    if (!saveProfile(profile)) return false;

    outIndex = (uint8_t)freeSlot;
    Serial.printf("[CVS] New profile created: %s (slot %d)\n", profile.name, freeSlot);
    return true;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Delete
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

bool CustomVehicleStore::deleteProfile(uint8_t index) {
    if (!_initialized || index >= MAX_CUSTOM_VEHICLES || !_summaryCache[index].inUse) return false;
    CustomVehicleProfile previous;
    if (!loadProfile(index, previous)) return false;
    CustomVehicleProfile oldSummary = _summaryCache[index];
    String path = _profilePath(index);
    if (SPIFFS.exists(path) && !SPIFFS.remove(path)) return false;
    memset(&_summaryCache[index], 0, sizeof(CustomVehicleProfile));

    if (_saveIndex()) return true;

    // Index commit failed: restore the authoritative profile and cache.
    if (!_writeProfileFile(previous)) {
        Serial.printf("[CVS] CRITICAL: deleted profile %u could not be restored\n", (unsigned)index);
        return false;
    }
    _summaryCache[index] = oldSummary;
    _saveIndex();
    return false;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Add / update a command
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool CustomVehicleStore::upsertCommand(uint8_t profileIndex, const LearnedCommand& cmd) {
    if (!_initialized) return false;
    if (profileIndex >= MAX_CUSTOM_VEHICLES || !_summaryCache[profileIndex].inUse) {
        Serial.println("[CVS] Target profile does not exist");
        return false;
    }

    CustomVehicleProfile profile;
    if (!loadProfile(profileIndex, profile)) return false;

    // Check whether a command with this label already exists (relearning)
    int existingIdx = -1;
    for (int i = 0; i < profile.commandCount; i++) {
        if (strcmp(profile.commands[i].label, cmd.label) == 0) {
            existingIdx = i;
            break;
        }
    }

    if (existingIdx >= 0) {
        profile.commands[existingIdx] = cmd;
        Serial.printf("[CVS] Command '%s' overwritten\n", cmd.label);
    } else {
        if (profile.commandCount >= MAX_LEARNED_COMMANDS_PER_VEHICLE) {
            Serial.println("[CVS] This profile's command capacity is full");
            return false;
        }
        profile.commands[profile.commandCount] = cmd;
        profile.commandCount++;
        Serial.printf("[CVS] New command '%s' added\n", cmd.label);
    }

    return saveProfile(profile);
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Change command status
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

bool CustomVehicleStore::setCommandStatus(uint8_t profileIndex, const char* label,
                                          CommandStatus newStatus, bool incrementFailCount) {
    if (!_initialized) return false;
    CustomVehicleProfile profile;
    if (!loadProfile(profileIndex, profile)) return false;

    for (int i = 0; i < profile.commandCount; i++) {
        if (strcmp(profile.commands[i].label, label) == 0) {
            profile.commands[i].status = newStatus;
            if (incrementFailCount) {
                profile.commands[i].failCount++;
            }
            return saveProfile(profile);
        }
    }

    Serial.printf("[CVS] Command '%s' not found for status update\n", label);
    return false;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Find a command
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

bool CustomVehicleStore::findCommand(uint8_t profileIndex, const char* label, LearnedCommand& outCmd) {
    if (!_initialized) return false;
    CustomVehicleProfile profile;
    if (!loadProfile(profileIndex, profile)) return false;

    for (int i = 0; i < profile.commandCount; i++) {
        if (strcmp(profile.commands[i].label, label) == 0) {
            outCmd = profile.commands[i];
            return true;
        }
    }

    return false;
}

// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○
// ○○○○○○○○○○ Export
// ○○○○○○○○○○○○○○○○○○○○○○○○○○○○○○

bool CustomVehicleStore::exportProfileJSON(uint8_t index, String& outJson) {
    if (!_initialized) return false;
    CustomVehicleProfile profile;
    if (!loadProfile(index, profile)) return false;

    JsonDocument doc;
    _profileToJson(profile, doc);

    outJson = "";
    serializeJson(doc, outJson);
    return outJson.length() > 0;
}

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Import
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

bool CustomVehicleStore::importProfileJSON(const String& json, uint8_t& outIndex) {
    if (!_initialized) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[CVS] Imported JSON is invalid: %s\n", err.c_str());
        return false;
    }

    // Minimal structural validation
    if (!doc["name"].is<const char*>() && !doc["name"].is<String>()) {
        Serial.println("[CVS] 'name' field missing from imported JSON");
        return false;
    }

    int freeSlot = -1;
    for (int i = 0; i < MAX_CUSTOM_VEHICLES; i++) {
        if (!_summaryCache[i].inUse) {
            freeSlot = i;
            break;
        }
    }
    if (freeSlot < 0) {
        Serial.println("[CVS] Profile capacity full - import not possible");
        return false;
    }

    CustomVehicleProfile profile;
    if (!_jsonToProfile(doc, profile, /*strict=*/true)) {
        Serial.println("[CVS] Import rejected - strict validation failed");
        return false;
    }

    profile.id     = (uint8_t)freeSlot;
    profile.inUse    = true;

    // Critical security policy (see CarTouch_SPEC.md): regardless of the
    // verified/unverified status in the source file, every imported
    // command is downgraded to UNVERIFIED - this device has not tested
    // these specific commands on this specific vehicle, even if they
    // were verified on a different device/vehicle.
    for (int i = 0; i < profile.commandCount; i++) {
        profile.commands[i].status      = CMD_UNVERIFIED;
        profile.commands[i].failCount      = 0;
    }

    if (!saveProfile(profile)) return false;

    outIndex = (uint8_t)freeSlot;
    Serial.printf("[CVS] Profile '%s' imported (slot %d) - all commands marked UNVERIFIED\n",
                  profile.name, freeSlot);
    return true;
}
