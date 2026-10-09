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
    if (!origin || origin[0] == '\0') return true;  // no Origin header
    if (!host || host[0] == '\0') return false;     // Origin but no Host
    static const char prefix[] = "http://";
    const size_t prefixLen = sizeof(prefix) - 1;
    if (strncasecmp(origin, prefix, prefixLen) != 0) return false;
    return strcasecmp(origin + prefixLen, host) == 0;
}

// DNS-rebinding protection.
//
// A page on an attacker's domain can re-point its DNS name at the device's IP
// address and then talk to the device from the victim's browser. Such
// requests still carry the attacker's name in the "Host" header. The device
// has no hostname of its own, so only its own IP addresses are valid values.
//
// host    : raw Host header, may include ":port". NULL/empty = absent and rejected.
// apIp    : dotted address of the access point (e.g. "192.168.4.1")
// staIp   : dotted address on the router network, NULL/empty/"0.0.0.0" if none
// allowedName : optional device hostname accepted without DNS lookup (e.g. "CarTouch")
static inline bool ctHostAllowed(const char* host, const char* apIp, const char* staIp,
                                 const char* allowedName = nullptr) {
    if (!host || host[0] == '\0') return false;
    size_t n = strlen(host);
    // strip an optional ":port" (digits only, 1..5)
    const char* colon = strrchr(host, ':');
    if (colon) {
        size_t digits = strlen(colon + 1);
        if (digits < 1 || digits > 5) return false;
        for (const char* p = colon + 1; *p; ++p) if (*p < '0' || *p > '9') return false;
        n = (size_t)(colon - host);
    }
    if (n == 0) return false;
    if (apIp && apIp[0] && strlen(apIp) == n && strncasecmp(host, apIp, n) == 0) return true;
    if (staIp && staIp[0] && strcmp(staIp, "0.0.0.0") != 0 &&
        strlen(staIp) == n && strncasecmp(host, staIp, n) == 0) return true;
    if (allowedName && allowedName[0] && strlen(allowedName) == n &&
        strncasecmp(host, allowedName, n) == 0) return true;
    return false;
}

#endif
