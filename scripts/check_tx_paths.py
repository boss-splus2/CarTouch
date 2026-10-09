#!/usr/bin/env python3
"""Static guard: only approved modules may transmit on the CAN bus.

Rule (project principle 4): transmission happens only from an explicit user
action through the guarded paths. Learn Mode, the recorder, the web server and
the UI must never transmit themselves (no automatic replay).

Checked in BOTH .cpp and .h files (an inline function in a header is just as
able to transmit), and for both the project API sendMessage() and the raw
ESP-IDF TWAI driver call twai_transmit*().

Approved .cpp callers: the drivers (can_manager.cpp, mcp2515_can_interface.cpp),
the router (can_service.cpp), OBD-II (obd2_reader.cpp) and vehicle commands
(vehicle_control.cpp). Approved headers only declare the interface.
Anything else fails the check.
Exit code 0 = ok, 1 = a forbidden file transmits.
"""
import re
import sys
from pathlib import Path

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Configuration
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

ALLOWED = {
    "can_manager.cpp", "mcp2515_can_interface.cpp", "can_service.cpp",
    "obd2_reader.cpp", "vehicle_control.cpp",
    # declarations of the interface (no call sites)
    "can_interface.h", "can_manager.h", "can_service.h",
    "mcp2515_can_interface.h",
}

TX_CALL = re.compile(r"\b(sendMessage|twai_transmit\w*)\s*\(")

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Checks
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Entry point
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "src")
    bad = []
    for path in sorted(list(root.glob("*.cpp")) + list(root.glob("*.h"))):
        if path.name in ALLOWED:
            continue
        code = strip_comments(path.read_text(errors="replace"))
        for m in TX_CALL.finditer(code):
            line = code.count("\n", 0, m.start()) + 1
            bad.append("%s (approx. line %d): %s()" % (path.name, line, m.group(1)))
    if bad:
        print("Forbidden CAN transmit call(s):")
        for b in bad:
            print("  " + b)
        return 1
    print("OK: only approved modules transmit on CAN")
    return 0

if __name__ == "__main__":
    sys.exit(main())
