#!/usr/bin/env python3
"""Cross-check the MCP2515 bit-timing table of the installed library.

CarTouch uses an 8 MHz crystal on CAN2. The library ships CNF1/2/3 constants
per bitrate; this script decodes the 8 MHz ones from the header that
PlatformIO really installed and verifies bitrate and sample point.

Usage: python3 scripts/check_mcp_timing.py [path/to/mcp2515.h]
Exit code 0 = all supported bitrates are correct, 1 = mismatch or header missing.
"""
import re
import sys
from pathlib import Path

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Configuration
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

OSC_HZ = 8_000_000
SUPPORTED = {100: 100_000, 125: 125_000, 250: 250_000, 500: 500_000, 1000: 1_000_000}

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Checks
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def find_header(arg):
    if arg:
        return Path(arg)
    hits = sorted(Path(".pio/libdeps").glob("*/*/mcp2515.h"))
    return hits[0] if hits else None

def decode(c1, c2, c3):
    brp = c1 & 0x3F
    sjw = ((c1 >> 6) & 3) + 1
    prseg = (c2 & 7) + 1
    ph1 = ((c2 >> 3) & 7) + 1
    btl = bool(c2 & 0x80)
    ph2 = (c3 & 7) + 1
    if not btl:
        return None
    total = 1 + prseg + ph1 + ph2
    bitrate = OSC_HZ // (2 * (brp + 1) * total)
    sample = (1 + prseg + ph1) * 1000 // total
    return bitrate, sample, sjw, total

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Entry point
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def main():
    header = find_header(sys.argv[1] if len(sys.argv) > 1 else None)
    if not header or not header.is_file():
        print("mcp2515.h not found (build once so PlatformIO installs the library)")
        return 1
    text = header.read_text(errors="replace")
    ok = True
    for kbps, expected in SUPPORTED.items():
        vals = []
        for n in (1, 2, 3):
            m = re.search(r"#define\s+MCP_8MHz_%dkBPS_CFG%d\s*\(?\s*(0x[0-9A-Fa-f]+)" % (kbps, n), text)
            vals.append(int(m.group(1), 16) if m else None)
        if None in vals:
            print("%4d kbps: constants missing in %s" % (kbps, header))
            ok = False
            continue
        d = decode(*vals)
        if d is None or d[0] != expected or not (600 <= d[1] <= 800):
            print("%4d kbps: MISMATCH %s -> %s" % (kbps, [hex(v) for v in vals], d))
            ok = False
        else:
            print("%4d kbps: OK  CNF=%s  sample=%.1f%%  SJW=%dTQ  TQ/bit=%d" % (
                kbps, "/".join("%02X" % v for v in vals), d[1] / 10, d[2], d[3]))
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
