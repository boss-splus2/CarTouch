CarTouch v2 targeted fixes

Copy these six files over the matching paths in CarTouch-main2:
- src/webserver.cpp
- src/ble_manager.cpp
- src/wifi_manager.cpp
- src/ct_ota_authenticity.h
- data/app.js
- data/index.html

Changes:
- Added authenticated asynchronous Wi-Fi scan API and dashboard network picker.
- Made AP startup more resilient with radio sleep disabled, a short driver settle delay,
  one AP restart/retry, and explicit failure logging.
- Replaced direct web/BLE credential reads with locked snapshots in authentication paths.
- Corrected the OTA signature hex-length loop to check bounds before dereferencing.

Validation: JavaScript syntax check passed. PlatformIO is not installed in this execution
runtime, so the firmware was not compiled here; hardware Wi-Fi/AP discovery is not verified.
The OTA production key still requires a real release public key and matching private signing
process before CT_PRODUCT_MODE=1 can be safely enabled.
