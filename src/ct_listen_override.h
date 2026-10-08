#ifndef CT_LISTEN_OVERRIDE_H
#define CT_LISTEN_OVERRIDE_H

// While a Learn session runs, the driver is forced into Listen-Only and the
// in-RAM setting is switched to true as well. That is temporary. Whatever is
// written to flash must still be the mode the user chose, otherwise any
// unrelated save (password, theme, ...) during the session would silently make
// Listen-Only permanent and the device would boot without transmit ability.
static inline bool ctPersistedListenOnly(bool currentRam, bool forcedByLearn,
                                         bool userChoiceBeforeLearn) {
    return forcedByLearn ? userChoiceBeforeLearn : currentRam;
}

#endif
