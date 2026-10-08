# CarTouch Root-Level Repair Report

This tree is a unified repair, not a CI-only patch.

## Fixed in code
- Removed the duplicate production-side `src/test_main.cpp`; Native tests remain under `test/test_native/`.
- Added explicit persisted actuator metadata to learned/manual commands. Unknown legacy/custom actuator types fail closed instead of silently bypassing duty-cycle protection.
- Added profile-specific verification execution so Web/TFT verification does not switch the global active vehicle.
- Added transactional rollback around custom-profile/index commits and deletes.
- Treat `index.json` as a derived cache and reconcile it from authoritative profile files at boot.
- Added real SD/Button configuration transactions using the existing recursive sync mutex.
- Added a single DBC message-limit source (`CT_DBC_MAX_MESSAGES`) and updated the CI checker to resolve the alias.
- Removed the unused Wi-Fi network-scan API.
- Added a shared OTA ownership lock preventing Web and BLE from aborting/starting each other's firmware update transaction.
- Routed Web OTA authentication through the central Web authentication/lockout path.
- Kept CAN TX path checks intact; they were not weakened.
- Moved the two previously misplaced Native tests into the canonical Native test file.

## Compatibility/safety behavior
- Existing commands without actuator metadata are intentionally treated as unknown and are not executable. This is fail-safe; they must be re-created/re-learned (or explicitly migrated with trusted metadata).
- Built-in DBC command behavior is unchanged: no guessed write mapping is introduced.
- Default credentials and hardware pin mappings were not changed.

## Validation actually run in this environment
- `python3 scripts/check_tx_paths.py src` — PASS.
- `python3 scripts/check_dbc_limits.py .` — PASS.
- `python3 scripts/audit_dbc.py --check` — PASS for 57 DBC files.
- Python scripts compile with `py_compile`.
- Source brace/static structural checks passed.

## Not claimed
A full PlatformIO firmware/native build was not executed because the available environment does not contain the PlatformIO toolchain/dependencies. Hardware CAN, SPI, TFT, SD, BLE and OTA behavior therefore still require real-device/CI validation.
