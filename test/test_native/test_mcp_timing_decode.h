#pragma once
// Test helper (not firmware code): decodes MCP2515 CNF1/CNF2/CNF3 back into
// bit rate and sample point, to cross-check the library's 8 MHz timing table.
// It lived in src/ct_can_config.h before, but only the test called it.
#include <stdint.h>

// Returns false for BTLMODE=0 layouts it cannot describe. bitrate in bit/s,
// samplePointPermille e.g. 625 = 62.5 %.
static inline bool testDecodeMcp2515Timing(uint32_t oscHz, uint8_t cnf1, uint8_t cnf2,
                                           uint8_t cnf3, uint32_t& bitrate,
                                           uint32_t& samplePointPermille,
                                           uint8_t& sjwTq) {
    if (oscHz == 0) return false;
    const uint32_t brp     = cnf1 & 0x3Fu;
    const uint32_t prseg   = (cnf2 & 0x07u) + 1u;
    const uint32_t phseg1  = ((cnf2 >> 3) & 0x07u) + 1u;
    const bool     btl     = (cnf2 & 0x80u) != 0;
    const uint32_t phseg2  = (cnf3 & 0x07u) + 1u;
    if (!btl) return false;                    // PS2 would come from PS1/IPT
    const uint32_t totalTq = 1u + prseg + phseg1 + phseg2;
    const uint32_t tqDiv   = 2u * (brp + 1u);  // TQ = tqDiv / Fosc
    bitrate = oscHz / (tqDiv * totalTq);
    samplePointPermille = ((1u + prseg + phseg1) * 1000u) / totalTq;
    sjwTq = (uint8_t)(((cnf1 >> 6) & 0x03u) + 1u);
    return true;
}
