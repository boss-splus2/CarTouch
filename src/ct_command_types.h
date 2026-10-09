#ifndef CT_COMMAND_TYPES_H
#define CT_COMMAND_TYPES_H

/**
 * ct_command_types.h - Pure (no Arduino / ESP-IDF) command vocabulary.
 *
 * Holds everything about a learned/manual command that does not need the
 * hardware: the persisted enums, the standard command labels, and the rules
 * for reading status and actuator class back from storage. It lives in its
 * own header so the native unit tests compile it. Before this split a
 * refactor of custom_vehicle.h could delete these definitions and only the
 * slow firmware build noticed.
 *
 * The numeric values of CommandStatus / CommandSource / CommandActuatorClass
 * are written to flash and to exported JSON. Never renumber them.
 */

#include <stdint.h>
#include <string.h>
#include "ct_command_actuator.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Command enums
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

enum CommandStatus : uint8_t {
    CMD_UNVERIFIED = 0,  // Saved but not yet user-confirmed - not executable
    CMD_VERIFIED   = 1   // User explicitly confirmed this works on the vehicle
};

enum CommandSource : uint8_t {
    SOURCE_DBC     = 0,  // From a DBC file (vehicle_db) - a write signal (rare in practice)
    SOURCE_LEARNED = 1,  // Captured via Learn Mode from a real physical button press
    SOURCE_MANUAL  = 2   // Entered manually by the user
};

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Standard command labels
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// Standard suggested command labels (see CarTouch_SPEC.md). These are just
// suggested strings - the user can also enter a free-form custom label,
// so they're defined as plain string constants (not a strict enum) here
// and reused by both the UI (TFT/web) and executeCommand() so all three
// stay in sync.
#define CMD_LABEL_LOCK_ALL         "lock_all"
#define CMD_LABEL_UNLOCK_ALL       "unlock_all"
#define CMD_LABEL_UNLOCK_DRIVER    "unlock_driver"
#define CMD_LABEL_WINDOW_FL_UP     "window_fl_up"
#define CMD_LABEL_WINDOW_FL_DOWN   "window_fl_down"
#define CMD_LABEL_WINDOW_FR_UP     "window_fr_up"
#define CMD_LABEL_WINDOW_FR_DOWN   "window_fr_down"
#define CMD_LABEL_WINDOW_RL_UP     "window_rl_up"
#define CMD_LABEL_WINDOW_RL_DOWN   "window_rl_down"
#define CMD_LABEL_WINDOW_RR_UP     "window_rr_up"
#define CMD_LABEL_WINDOW_RR_DOWN   "window_rr_down"
#define CMD_LABEL_ALL_WINDOWS_UP   "all_windows_up"
#define CMD_LABEL_ALL_WINDOWS_DOWN "all_windows_down"
#define CMD_LABEL_SUNROOF_OPEN     "sunroof_open"
#define CMD_LABEL_SUNROOF_CLOSE    "sunroof_close"
#define CMD_LABEL_SUNROOF_TILT     "sunroof_tilt"
#define CMD_LABEL_TRUNK_OPEN       "trunk_open"
#define CMD_LABEL_TRUNK_LOCK       "trunk_lock"
#define CMD_LABEL_MIRROR_FOLD      "mirror_fold"
#define CMD_LABEL_MIRROR_UNFOLD    "mirror_unfold"
#define CMD_LABEL_ALARM_ARM        "alarm_arm"
#define CMD_LABEL_ALARM_DISARM     "alarm_disarm"
// For a custom label, the user enters a free-form string that is stored
// directly as the label (no special prefix required).

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Stored-state readers
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

/**
 * Status of a command read back from a profile.
 *
 * trustedStorage = true  : the profile comes from this device's own flash
 *                          (loadProfile). The stored "verified" is honoured,
 *                          otherwise a verification would be forgotten at the
 *                          next read and no command could ever be executed.
 * trustedStorage = false : the profile comes from outside (import). It is
 *                          NEVER trusted as verified - this device has not
 *                          tested the command on this vehicle.
 */
static inline CommandStatus ctCommandStatusFromStored(const char* stored, bool trustedStorage) {
    if (!trustedStorage || !stored) return CMD_UNVERIFIED;
    return strcmp(stored, "verified") == 0 ? CMD_VERIFIED : CMD_UNVERIFIED;
}

/**
 * Actuator class of a command read back from a profile.
 *
 * present = false : the profile was saved before actuator metadata existed.
 *                   A standard label gets its documented class, so existing
 *                   commands keep working. A custom label stays UNKNOWN and
 *                   is refused until the user re-creates it (fail-closed).
 * present = true  : a value outside the enum is UNKNOWN (fail-closed).
 */
static inline CommandActuatorClass ctCommandActuatorFromStored(bool present, long value,
                                                               const char* label) {
    if (!present) return ctSuggestedActuatorClassForStandardLabel(label);
    if (value < (long)COMMAND_ACTUATOR_UNKNOWN || value > (long)COMMAND_ACTUATOR_MIRROR) {
        return COMMAND_ACTUATOR_UNKNOWN;
    }
    return (CommandActuatorClass)value;
}

#endif
