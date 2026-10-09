#!/usr/bin/env python3
"""Print firmware size vs. OTA slot for every built environment and fail if
the free space in the slot gets too small.

OTA needs the new image to fit in one app slot, so the free space is the
real limit for adding features.

Usage:
    python3 scripts/check_fw_size.py [MIN_FREE_BYTES] [BUILD_DIR] [--strict]

  MIN_FREE_BYTES  smallest allowed free space in a slot (default 8192)
  BUILD_DIR       folder holding <environment>/firmware.bin
                  (default .pio/build; CI passes the assembled bundle)
  --strict        an environment without a firmware.bin is an error
                  (CI uses this so a missing build can never pass silently)
"""
import argparse
import csv
import sys
from pathlib import Path

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Configuration
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

ENVS = (
    ("esp32-s3-devkitc-1", "cartouch_16MB.csv"),
    ("esp32-s3-headless", "cartouch_16MB.csv"),
    ("esp32-s3-4mb", "cartouch_4MB.csv"),
    ("esp32-s3-4mb-psram", "cartouch_4MB.csv"),
)

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Checks
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def app_slot(csv_path):
    with Path(csv_path).open(newline="") as f:
        for row in csv.reader(f):
            if row and row[0].strip() == "app0":
                return int(row[4].strip(), 0)
    raise SystemExit(f"app0 not found in {csv_path}")

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Entry point
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("min_free", nargs="?", type=int, default=8192)
    parser.add_argument("build_dir", nargs="?", default=".pio/build")
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()

    failed = False
    print(f"{'environment':22} {'firmware':>10} {'slot':>10} {'free':>9}")
    for env, table in ENVS:
        image = Path(args.build_dir) / env / "firmware.bin"
        if not image.is_file():
            if args.strict:
                print(f"{env:22} MISSING: {image}")
                failed = True
            else:
                print(f"{env:22} (not built)")
            continue
        size, slot = image.stat().st_size, app_slot(table)
        free = slot - size
        flag = ""
        if free < 0 or free < args.min_free:
            flag, failed = "  <-- TOO SMALL", True
        print(f"{env:22} {size:>10} {slot:>10} {free:>9}{flag}")
    if failed:
        print(f"Check failed: a firmware is missing or an OTA slot has less than "
              f"{args.min_free} bytes free.")
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
