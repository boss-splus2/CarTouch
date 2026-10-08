# OTA summary

Web OTA supports firmware and filesystem images. The expected SHA-256 is supplied from the matching CI artifact `.sha256` file.

Important properties:

- firmware is written to the inactive app slot;
- SHA-256 is an integrity check, not a digital signature;
- HTTPS is not available;
- automatic post-boot rollback is not implemented;
- filesystem has one partition and no automatic power-loss recovery;
- filesystem replacement is protected while user data requiring preservation is present.

BLE OTA independently requires the current application password and SHA-256; the legacy `START` form without a hash is rejected.

The firmware version source of truth is `CAR_TOUCH_FIRMWARE_VERSION` in `src/config.h`.
