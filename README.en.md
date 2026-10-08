<div align="center">

# 🚗 CarTouch

### ESP32-S3 touch, Web and BLE interface for CAN Bus monitoring and controlled vehicle commands

<p>
  <img src="https://img.shields.io/badge/ESP32--S3-N16R8-2ea44f" alt="ESP32-S3">
  <img src="https://img.shields.io/badge/CAN-Dual%20Bus-f97316" alt="Dual CAN">
  <img src="https://img.shields.io/badge/LVGL-TFT%20UI-7c3aed" alt="LVGL">
  <img src="https://img.shields.io/badge/OTA-Web%20%7C%20BLE-2563eb" alt="OTA">
</p>

</div>

> [!CAUTION]
> CarTouch can transmit directly to CAN Bus. A wrong frame can cause unintended vehicle behavior. Keep CAN in **Listen-Only** until the complete command path has been verified on a controlled bench setup.

> [!WARNING]
> The current evidence is software/CI validation. No real-vehicle or field validation is claimed here.

## Overview

CarTouch is an ESP32-S3 firmware for CAN monitoring and controlled command transmission. It provides TFT/LVGL, Web, WebSocket, BLE and USB Serial interfaces.

## Hardware baseline

| Bus | Interface | Pins |
|---|---|---|
| CAN1 | TJA1051 / TWAI | TX `17`, RX `18` |
| CAN2 | MCP2515 + TJA1050 | CS `15`, INT `16`, SCK `12`, MOSI `11`, MISO `13` |

CAN2 uses an 8 MHz MCP2515 crystal. Do not connect 5 V logic directly to ESP32-S3 GPIOs; use suitable level shifting where required.

## First run

- AP SSID: `CarTouch`
- Default password: `12345678`
- Web port: `80`
- Use the address reported by Serial as the final device address.

Change default credentials before real deployment.

## Safety and security

- CAN TX paths use the common command guard.
- Listen-Only is applied at the CAN driver level.
- WebSocket command transmission is blocked after session expiry.
- Web and BLE OTA transactions have exclusive ownership.
- Custom commands without trusted actuator metadata fail closed.
- Verification can target a specific profile without changing the global active vehicle.
- Web currently uses HTTP Basic Authentication without firmware-side TLS.
- Secure Boot and Flash Encryption are not enabled by the current project configuration.
- SHA-256 on OTA images provides integrity checking, not a digital signature.

## Testing

The current CI report contains:

```text
96 Tests 0 Failures 0 Ignored
native:test_native [PASSED]
```

CAN TX path checks, DBC checks, manifest validation and MCP2515 timing checks also pass. cppcheck passes for both ESP32-S3 profiles with six LOW-level style findings.

These results do not constitute real-vehicle validation.

## Documentation

- [`CarTouch_SPEC.md`](CarTouch_SPEC.md) — technical specification and hardware reference
- [`docs/FLASHING.md`](docs/FLASHING.md) — manual flashing
- [`docs/MEMORY.md`](docs/MEMORY.md) — memory measurement
- [`docs/OTA.md`](docs/OTA.md) — Web OTA
- [`BLE_OTA.md`](BLE_OTA.md) — BLE and BLE OTA
- [`DBC_AUDIT.md`](DBC_AUDIT.md) — DBC audit
- [`ROOT_ARCHITECTURE_REPAIR_REPORT.md`](ROOT_ARCHITECTURE_REPAIR_REPORT.md) — architecture repair and validation summary
- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) — third-party notices

The Persian `README.md` is the primary project entry point; detailed behavior belongs in the relevant document.

## License

See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
