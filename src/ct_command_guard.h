#ifndef CT_COMMAND_GUARD_H
#define CT_COMMAND_GUARD_H

#include <stddef.h>
#include <string.h>

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Command verdicts
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// Gate applied to EVERY control command, whatever its source: Web
// (/api/control and WebSocket), BLE, TFT and the USB Serial `control <cmd>`
// console all end in handleCommand() -> queue -> processCommand().
//
// Commands that only change navigation/configuration stay available while the
// device sleeps or is in Listen-Only mode. Everything else (door/window/trunk
// actuators, alarm, ...) is refused in either state.

enum CtCommandVerdict {
    CT_CMD_ALLOW = 0,
    CT_CMD_REJECT_ASLEEP,
    CT_CMD_REJECT_LISTEN_ONLY,
    CT_CMD_REJECT_INVALID
};

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Command gating
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

static inline bool ctCommandIsSafeConfig(const char* cmd) {
    if (!cmd) return false;
    return strcmp(cmd, "listen_only") == 0 ||
           strcmp(cmd, "vehicle_select") == 0 ||
           strncmp(cmd, "vehicle_select_dbc:", 19) == 0 ||
           strncmp(cmd, "vehicle_select_custom:", 22) == 0 ||
           strcmp(cmd, "toggle_theme") == 0;
}

// active          : device is in MODE_ACTIVE (not sleeping)
// listenOnlyBus   : Listen-Only is on for the currently selected bus
static inline CtCommandVerdict ctCommandGate(bool active, bool listenOnlyBus, const char* cmd) {
    if (!cmd || cmd[0] == '\0') return CT_CMD_REJECT_INVALID;
    const bool safe = ctCommandIsSafeConfig(cmd);
    if (!active && !safe) return CT_CMD_REJECT_ASLEEP;
    if (listenOnlyBus && !safe) return CT_CMD_REJECT_LISTEN_ONLY;
    return CT_CMD_ALLOW;
}

#endif
