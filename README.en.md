# CarTouch — concise English README

> [!WARNING]
> **Beta — not tested on a vehicle or physical board.** Current evidence is source review, build/native-test history, and CI configuration. Keep CAN in Listen-Only until the installation is verified.

## Hardware baseline

- **CAN1:** CJMCU-1051 / TJA1051, `CTX/TXD -> GPIO17`, `CRX/RXD -> GPIO18`, `VCC=5V`.
- **CAN2:** MCP2515 + TJA1050, 8 MHz crystal; `CS=GPIO15`, `INT=GPIO16`, shared SPI `SCK=12`, `MOSI=11`, `MISO=13`.
- Use appropriate level shifting for the 5 V MCP2515/TJA1050 logic. Never connect a 5 V output directly to an ESP32-S3 GPIO.
- OBD-II: pin 6 CAN-H, 14 CAN-L, 4/5 GND, 16 +12 V.
- Remove the MCP2515 120 Ω J1 termination when connecting to a vehicle OBD-II port. Use termination for a controlled two-node bench setup.
- Vehicle power should go through fuse/transient/load-dump protection and a suitable buck converter to 5 V; the DevKitC generates 3.3 V locally.
- SD is disabled by default (`CS=-1`); five-way input has no default pins.

## First run

- AP SSID: `CarTouch`
- WPA2-PSK; maximum 4 AP clients.
- Default admin credentials: `CarTouch` / `12345678`.
- Web server: port 80. The default AP address from the ESP32 Wi-Fi stack is normally `192.168.4.1`; use the address printed by Serial as the final reference.
- The same password is used for Web Basic Auth, Wi-Fi AP, BLE AUTH and BLE OTA. This is an accepted personal-use risk because the default password is public in the repository.

## Security

- HTTP uses Basic Authentication over plain HTTP; credentials are Base64-encoded, not encrypted.
- Host validation accepts current AP/STA IPs or the device name `CarTouch`; missing Host is rejected.
- WebSocket session tokens are compared in constant time and expire after 15 minutes. Expiry is checked on every post-auth WebSocket message.
- Password changes invalidate all WebSocket sessions.
- Login lockout: 5 failed attempts, 30 seconds, 8 tracked IP slots with oldest-entry eviction.
- USB Serial is physically trusted and exposes `control <command>`; the command still goes through the common command guard.
- BLE uses Just Works / LE Secure Connections without MITM. Control and OTA commands require AUTH; `STATUS` is intentionally public.
- Flash Encryption and Secure Boot are not enabled by this project configuration.

## CI artifacts

The GitHub Actions workflow produces one artifact named `cartouch-firmware` with one directory per profile. Firmware and filesystem images have `.sha256` files. The `esp32-s3-headless` profile intentionally has no `spiffs.bin`; use a profile with a filesystem for the Web UI.

## License / DBC warning

The project license is proprietary. The repository contains 57 DBC files whose complete provenance/licensing has not yet been verified. Before commercializing or redistributing the device, verify and document the provenance and license of each DBC.

See `CarTouch_SPEC.md`, `BLE_OTA.md`, `DBC_AUDIT.md`, and `THIRD_PARTY_NOTICES.md` for detailed technical information.
