#!/usr/bin/env python3
"""Static guard: the DBC limits in ct_dbc_store.h must match the real loader.

- CT_DBC_MAX_MESSAGES (upload check) must equal MAX_DBC_MESSAGES (vehicle_db.h),
  otherwise an upload could be accepted and then silently cut off by the loader.
- "/dbc/" + CT_DBC_NAME_MAX + NUL must fit in VehicleProfile::dbcFileName[N].
- Every bundled DBC (data/dbc/manifest.json) must have a valid name and no more
  messages than the cap.
Exit code 0 = ok, 1 = mismatch.
"""
import json
import re
import sys
from pathlib import Path

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Checks
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def find(text, pattern, what):
    m = re.search(pattern, text)
    if not m:
        print("ERROR: could not find %s" % what)
        sys.exit(1)
    return int(m.group(1))

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Entry point
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    store = (root / "src/ct_dbc_store.h").read_text(errors="replace")
    vdb = (root / "src/vehicle_db.h").read_text(errors="replace")
    cap_store = find(store, r"#define\s+CT_DBC_MAX_MESSAGES\s+(\d+)", "CT_DBC_MAX_MESSAGES")
    name_max = find(store, r"#define\s+CT_DBC_NAME_MAX\s+(\d+)", "CT_DBC_NAME_MAX")
    m_vdb = re.search(r"#define\s+MAX_DBC_MESSAGES\s+(.+)", vdb)
    if not m_vdb:
        print("ERROR: could not find MAX_DBC_MESSAGES")
        sys.exit(1)
    expr = m_vdb.group(1).strip()
    if expr.isdigit():
        cap_vdb = int(expr)
    else:
        m_alias = re.search(r"#define\s+CT_DBC_MAX_MESSAGES\s+(\d+)", store)
        if not m_alias or expr != "CT_DBC_MAX_MESSAGES":
            print("ERROR: MAX_DBC_MESSAGES is not a resolvable DBC limit")
            sys.exit(1)
        cap_vdb = int(m_alias.group(1))
    field = find(vdb, r"char\s+dbcFileName\[(\d+)\]", "dbcFileName size")
    prefix = len("/dbc/")
    bad = []
    if cap_store != cap_vdb:
        bad.append("CT_DBC_MAX_MESSAGES=%d but MAX_DBC_MESSAGES=%d" % (cap_store, cap_vdb))
    if prefix + name_max + 1 > field:
        bad.append("'/dbc/'+%d chars+NUL does not fit dbcFileName[%d]" % (name_max, field))
    manifest = root / "data/dbc/manifest.json"
    if manifest.exists():
        for f in json.loads(manifest.read_text())["files"]:
            if len(f["name"]) > name_max or not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*\.dbc", f["name"], re.I) or ".." in f["name"]:
                bad.append("bundled file name not accepted by upload rules: %s" % f["name"])
            if f["messages"] > cap_vdb:
                bad.append("%s has %d messages, above the cap %d" % (f["name"], f["messages"], cap_vdb))
    if bad:
        print("DBC limit mismatch:")
        for b in bad:
            print("  " + b)
        return 1
    print("OK: DBC limits consistent (cap %d messages, name max %d, field %d)" % (cap_vdb, name_max, field))
    return 0

if __name__ == "__main__":
    sys.exit(main())
