# Manual flashing

## 16 MB profile

Use image files from the **same CI profile directory**:

| File | Address |
|---|---:|
| `bootloader.bin` | `0x0` |
| `partitions.bin` | `0x8000` |
| `boot_app0.bin` | `0xE000` |
| `firmware.bin` | `0x10000` |
| `spiffs.bin` | `0xA10000` |

Use ESP32-S3, 16 MB flash, 80 MHz and QIO. If the board does not boot, DIO is a diagnostic fallback. `boot_app0.bin` is written separately at `0xE000`.

## 4 MB profiles

The system images use the same bootloader/partition/firmware addresses; `spiffs.bin` starts at `0x310000`.

The 4 MB filesystem is deliberately reduced and cannot contain the complete DBC collection. Do not mix images from different profiles.

`uploadfs` replaces the SPIFFS image. Export/backup user profiles, recordings and user DBC data first.
