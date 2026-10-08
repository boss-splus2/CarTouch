#ifndef CT_COMMAND_ACTUATOR_H
#define CT_COMMAND_ACTUATOR_H

#include <stdint.h>
#include <string.h>

enum CommandActuatorClass : uint8_t {
    COMMAND_ACTUATOR_UNKNOWN = 0,
    COMMAND_ACTUATOR_NONE    = 1,
    COMMAND_ACTUATOR_WINDOW  = 2,
    COMMAND_ACTUATOR_SUNROOF = 3,
    COMMAND_ACTUATOR_MIRROR  = 4
};

static inline CommandActuatorClass ctSuggestedActuatorClassForStandardLabel(const char* label) {
    if (!label) return COMMAND_ACTUATOR_UNKNOWN;
    if (!strncmp(label, "window_", 7) || !strncmp(label, "all_windows_", 12)) return COMMAND_ACTUATOR_WINDOW;
    if (!strncmp(label, "sunroof_", 8)) return COMMAND_ACTUATOR_SUNROOF;
    if (!strncmp(label, "mirror_", 7)) return COMMAND_ACTUATOR_MIRROR;
    if (!strcmp(label, "lock_all") || !strcmp(label, "unlock_all") || !strcmp(label, "unlock_driver") ||
        !strcmp(label, "trunk_open") || !strcmp(label, "trunk_lock") || !strcmp(label, "alarm_arm") || !strcmp(label, "alarm_disarm")) return COMMAND_ACTUATOR_NONE;
    return COMMAND_ACTUATOR_UNKNOWN;
}

#endif
