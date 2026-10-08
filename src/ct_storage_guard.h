#ifndef CT_STORAGE_GUARD_H
#define CT_STORAGE_GUARD_H

static inline bool ctFilesystemOtaAllowed(bool filesystemMounted,
                                          bool hasProfilesOrRecordings,
                                          bool hasInternalUserDbc) {
    return filesystemMounted && !hasProfilesOrRecordings && !hasInternalUserDbc;
}

#endif