# CarTouch BLE and BLE OTA

CarTouch starts BLE independently of Wi-Fi and advertises as `CarTouch-XXXX`.

## Characteristics
- **Status** — read/notify status and OTA progress. Authenticated diagnostic notifications are sent only to the requesting connection.
- **Command** — status, authenticated DTC/recording commands, and OTA control commands.
- **Data** — firmware bytes during authenticated OTA.

## Authenticated device commands
1. Pair/connect over the encrypted BLE link.
2. Write `AUTH:<web-password>` to Command. While `CT_REQUIRE_PASSWORD_CHANGE` is 1 (commercial setting), the default Web password is not accepted; five failures lock command authentication for 60 seconds.
3. Send one of `DTC:READ`, `DTC:CLEAR`, `DTC:STATUS`, `RECORD:START:1`, `RECORD:START:2`, `RECORD:START:BOTH`, `RECORD:STOP`, `RECORD:STATUS`, or `RECORD:DELETE:canNNNN.csv`. Device operations are rate-limited to one request per 300 ms.
4. Subscribe to Status for the result. Diagnostic status notifications are targeted to the authenticated connection. `DTC:CLEAR` is explicit and reports success only after a positive ECU acknowledgement.
5. Send `LOGOUT` when finished. Command authentication and OTA ownership are bound to the BLE connection and cleared on disconnect.

This is a bounded subset, not Web UI parity. BLE does not accept raw CAN frames, replay, vehicle control, Learn changes, or DBC import. Accepted DTC and recording operations use the same main-loop services as Web and Serial.

## Authenticated device commands
1. Pair/connect over the encrypted BLE link.
2. Write `AUTH:<web-password>` to Command. While `CT_REQUIRE_PASSWORD_CHANGE` is 1 (commercial setting), the default Web password is not accepted; five failures lock command authentication for 60 seconds.
3. Send one of `DTC:READ`, `DTC:CLEAR`, `DTC:STATUS`, `RECORD:START:1`, `RECORD:START:2`, `RECORD:START:BOTH`, `RECORD:STOP`, `RECORD:STATUS`, or `RECORD:DELETE:canNNNN.csv`.
4. Read/subscribe to Status for the resulting state. `DTC:CLEAR` is explicit and requires a positive ECU acknowledgement before reporting success.
5. Send `LOGOUT` when finished. Authentication is connection-scoped and is cleared when that connection disconnects.

BLE device commands are a bounded subset; they do not provide raw CAN transmission, Learn profile management, DBC import, or general Web UI parity. All accepted operations are queued into the same main-loop services used by Web/Serial.

## OTA sequence
1. Connect to the device.
2. Obtain the SHA-256 digest for the exact `firmware.bin` from its matching `firmware.bin.sha256` file in the same trusted firmware artifact. Send `START:<web-password>:<firmware-size>:<sha256>` to Command. The digest is 64 hexadecimal characters; the size and digest are parsed from the end, so the password may contain `:`.
3. Wait for `OTA_STARTED`.
4. Write firmware bytes to Data.
5. Send `END`.
6. The device verifies the exact byte count and SHA-256 before finalizing the image and rebooting. A mismatch returns `OTA_SHA256_MISMATCH` and leaves the new app slot unactivated.

The old `START:<web-password>:<firmware-size>` form is rejected. `ABORT` cancels a transfer; disconnecting also aborts an active OTA.

## Security
BLE link security is enabled. BLE OTA requires the current Web password in `START`; device commands require a separate `AUTH` using that password. Do not expose the password over an untrusted BLE environment.

The authenticated Web UI remains the full configuration surface.

## Hardening notes
- Command/Data characteristics require an encrypted (paired) link.
- One fixed default login is defined in one place (`WEB_DEFAULT_USER` / `WEB_DEFAULT_PASS` in `src/config.h`, currently `CarTouch` / `12345678`). It is never generated automatically. The Web UI, TFT, BLE and the Wi-Fi access point all use it until the owner changes the password (the access point picks up a new password at the next restart).
- `CT_REQUIRE_PASSWORD_CHANGE` in `src/config.h`: 0 (current, development) accepts the default login everywhere; 1 (commercial) makes BLE commands and BLE OTA refuse the default (`OTA_CHANGE_DEFAULT_PASSWORD`) and forbids choosing the default again.
- 5 wrong passwords lock BLE OTA for 60 seconds.
- The image header is checked while data arrives: `OTA_BAD_HEADER` (not an ESP image), `OTA_WRONG_CHIP` (not built for the ESP32-S3) and `OTA_WRONG_FLASH_SIZE` (built for more flash than the device has, for example a 16 MB image sent to a 4 MB board). The transfer is aborted and the running firmware is untouched.
- SHA-256 is computed incrementally as bytes arrive. It detects a transfer/file mismatch against the supplied digest; it is not a digital signature and does not prove who built the image. Use a checksum from the same trusted release artifact.
- Pairing uses Just Works (no passkey, no MITM protection). The link is encrypted, but an attacker present during first pairing could impersonate the device or the phone and capture the OTA password. Pair only in a trusted place, and change the Web password if pairing may have been observed.

## Power-loss behavior and limits

Firmware OTA writes the inactive app slot. The updater keeps its boot magic unset until finalization, then selects the new slot only after the full image and digest have been accepted. A power loss during transfer should therefore leave the currently selected app slot intact. This is not a boot-health rollback: if the new image is accepted and then fails after boot, the firmware does not automatically mark itself valid or roll back.

Filesystem OTA writes the single SPIFFS partition in place; it has no second filesystem slot. A power loss or digest mismatch during that update can leave SPIFFS unusable. Web OTA blocks the operation when it detects protected internal user data, but there is no automatic filesystem restore; the embedded OTA page can be used to retry a filesystem upload if the firmware itself still boots. Manual `uploadfs` bypasses that web guard.

SHA-256 only detects mismatch with the supplied digest. Web OTA uses HTTP without TLS, and BLE pairing is Just Works, so the digest does not authenticate a release against an active attacker. Use a trusted artifact and a trusted local connection.
