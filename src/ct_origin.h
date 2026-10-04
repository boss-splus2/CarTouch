#ifndef CT_ORIGIN_H
#define CT_ORIGIN_H

#include <stddef.h>
#include <string.h>
#include <strings.h>

// Cross-site request protection for the web server.
//
// A browser sends the "Origin" header on every cross-site POST. The page served
// by the device is plain HTTP, so the legitimate value is "http://" + the Host
// header. Requests without an Origin header (curl, scripts, tools) are not
// browser-driven cross-site requests and are allowed; they still need the
// normal login.
//
// origin / host may be NULL when the header is absent.
static inline bool ctOriginAllowed(const char* origin, const char* host) {
    if (!origin || origin[0] == '\0') return true;     // no Origin header
    if (!host || host[0] == '\0') return false;        // Origin but no Host
    static const char prefix[] = "http://";
    const size_t prefixLen = sizeof(prefix) - 1;
    if (strncasecmp(origin, prefix, prefixLen) != 0) return false;
    return strcasecmp(origin + prefixLen, host) == 0;
}

#endif
