/**
 * custom_vehicle.h - Data structures for custom vehicle profiles
 *
 * See CarTouch_SPEC.md.
 *
 * Defines two new command sources, alongside the built-in DBC files
 * (vehicle_db.h):
 *   1. Learned - a command captured from live CAN Bus traffic while the
 *                vehicle's physical button was pressed (see learn_engine.h)
 *   2. Manual  - a command the user entered directly (CAN ID + bytes)
 *
 * Both are stored in the same LearnedCommand struct, since from the
 * device's point of view they're identical: a fixed CAN ID + payload
 * that must not be executable until the user has explicitly verified it
 * (status == VERIFIED).
 *
 * Safety note: the mere presence of a LearnedCommand in this struct
 * does NOT mean it's cleared to send. VehicleControl::executeCommand()
 * only allows an actual send when status == CMD_VERIFIED.
 */

#ifndef CUSTOM_VEHICLE_H
#define CUSTOM_VEHICLE_H

#include <Arduino.h>
#include "config.h"
#include "ct_dbc_store.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Sizing
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// Deliberately conservative to avoid repeating
// The limit is intentionally conservative to keep profile memory bounded.
// Each LearnedCommand is ~50 bytes -> 32 commands * 8 vehicles ~= 12.8 KB total.


// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ Command verification status
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

// CommandStatus, CommandSource, CommandActuatorClass, the standard CMD_LABEL_*
// constants and the ctCommand*FromStored() readers live in ct_command_types.h
// (a single definition, shared with the native unit tests).
#include "ct_command_types.h"

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ A single learned/manual command
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

struct LearnedCommand {
    char label[32]       = {0};  // Internal identifier (e.g. "lock_all" or a custom name)
    char displayName[48] = {0};  // English display name shown to the driver

    uint32_t canId      = 0;
    bool     isExtended = false;
    uint8_t  length     = 0;
    uint8_t  data[8]    = {0};

    CommandSource        source        = SOURCE_MANUAL;
    CommandStatus        status        = CMD_UNVERIFIED;
    CommandActuatorClass actuatorClass = COMMAND_ACTUATOR_UNKNOWN;

    uint8_t  timesObserved = 0;  // Times seen during capture (Learned only)
    uint8_t  failCount     = 0;  // Failed verification attempts
    uint32_t createdAt     = 0;  // Creation time (millis) - for display/debug only; meaningless after reboot, session-local reference
};

// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
// □□□□□□□□□□ A custom vehicle profile
// ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

struct CustomVehicleProfile {
    uint8_t  id                               = 0;    // Index in the profile store (0..MAX_CUSTOM_VEHICLES-1)
    char     name[32]                         = {0};  // User-chosen name, e.g. "Dad's Pride"
    char     brand[24]                        = {0};
    char     model[24]                        = {0};
    char     dbcFileName[CT_DBC_NAME_MAX + 1] = {0};
    uint16_t year                             = 0;
    uint32_t revision                         = 0;    // monotonically changes on every persisted profile mutation

    LearnedCommand commands[MAX_LEARNED_COMMANDS_PER_VEHICLE];
    uint8_t          commandCount = 0;

    bool inUse = false;    // false = this slot is empty
};

#endif    // CUSTOM_VEHICLE_H
