# CarTouch security model

## Accepted baseline risk

The current default credentials are intentionally fixed: `CarTouch` / `12345678`, with `CT_REQUIRE_PASSWORD_CHANGE=0`. Because the repository is public, this credential is not secret. This is accepted only for the current personal/development use case.

The same password is used for Web Basic Auth, WPA2 AP, BLE AUTH and BLE OTA. HTTPS is not implemented. Basic Auth therefore transports a Base64 representation over plain HTTP; Base64 is not encryption.

## Trust boundaries

| Surface | Trust assumption | Protection |
|---|---|---|
| Wi-Fi AP | Anyone who knows the public password may connect | WPA2-PSK, max 4 AP clients |
| Web | Network client | Basic Auth, Host validation, login lockout, session token |
| WebSocket | Authenticated client | per-message session expiry check, constant-time token compare |
| BLE | Paired device | encrypted link, AUTH for control/OTA; Just Works has no MITM |
| USB Serial | Physical access | no separate authentication; control still passes common command guard |

## Command safety

CAN TX remains behind the project's common admission/verification guards. Learn/custom commands are not executable until explicitly verified. Unknown actuator metadata is fail-closed and cannot silently acquire a mechanical duty-cycle class from an arbitrary label.

## Password-change mode

`CT_REQUIRE_PASSWORD_CHANGE=1` currently blocks BLE commands and BLE OTA while the default password is still active. Web Basic Auth is intentionally not blocked by this flag in the current implementation. Default remains `0`.

## Storage and secrets

The web password is stored in the existing NVS configuration as plaintext. Flash Encryption and Secure Boot are not enabled by this project configuration. The password must not be printed in status/config/log/API/export paths.

## Known remaining architecture items

- Full NVS schema-version migration is not yet implemented.
- A credential epoch separate from password state is not yet implemented.
- Full OTA manager/lifecycle watchdog is not yet implemented; current code provides shared ownership locking.
