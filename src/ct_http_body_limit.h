#ifndef CT_HTTP_BODY_LIMIT_H
#define CT_HTTP_BODY_LIMIT_H

#include <stddef.h>
#include <string.h>

static inline size_t ctHttpBodyLimitForPath(const char* path,
                                            size_t defaultLimit,
                                            size_t profileImportLimit,
                                            size_t dbcUploadLimit) {
    if (!path) return defaultLimit;
    if (strcmp(path, "/update") == 0) return 0;
    if (strcmp(path, "/api/vehicles/custom/import") == 0) return profileImportLimit;
    if (strcmp(path, "/api/dbc/upload") == 0) return dbcUploadLimit;
    return defaultLimit;
}

static inline bool ctHttpBodyExceedsLimit(size_t contentLength,
                                          bool unknownLength,
                                          size_t maxBytes) {
    return maxBytes != 0 && (unknownLength || contentLength > maxBytes);
}

#endif
