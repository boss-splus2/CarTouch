# CarTouch — Unified Root Repair and Documentation Sync Report

این نسخه یک اصلاح یکپارچه است؛ هیچ Stage جداگانه‌ای بخشی از روش انتشار این نسخه نیست.

## اصلاحات کد

- حذف `src/test_main.cpp` تکراری و نگه‌داشتن Native tests در `test/test_native/`.
- افزودن `actuatorClass` صریح به فرمان‌های learned/manual و fail-closed کردن `UNKNOWN`.
- verification پروفایل‌محور برای Web/TFT بدون تغییر active vehicle سراسری.
- transaction/rollback برای profile و index؛ `index.json` فقط cache مشتق‌شده است.
- lock مشترک `ctSync()` برای تنظیمات SD و Button.
- یک منبع واحد برای سقف DBC و حفظ checker مسیرهای CAN TX.
- حذف API بدون مصرف `WiFiManager::scanNetworks()`.
- افزودن `CtOtaLock` برای مالکیت مشترک OTA بین Web و BLE.
- استفاده Web OTA از authentication/lockout مرکزی.

## همگام‌سازی مستندات

فایل‌های زیر با کد فعلی بازبینی/به‌روزرسانی شده‌اند:

- `README.md`
- `README.en.md`
- `CarTouch_SPEC.md`
- `BLE_OTA.md`
- `docs/OTA.md`
- `docs/SECURITY.md`
- `ROOT_ARCHITECTURE_REPAIR_REPORT.md`

مستندات موجود `docs/FLASHING.md`, `docs/MEMORY.md`, `DBC_AUDIT.md` و `THIRD_PARTY_NOTICES.md` حفظ شده‌اند و در README/SPEC به آن‌ها ارجاع داده می‌شود.

مستندات اکنون صریحاً موارد حل‌نشده را نیز ثبت می‌کنند، از جمله:

- timeout/watchdog خودکار برای OTA Web در قطع غیرعادی کلاینت؛
- نبود `OtaManager` کامل و استفاده مستقیم Web/BLE از `Update` در کنار ownership lock؛
- نبود migration کامل schema برای blob قدیمی `AppConfig`؛
- نبود credential epoch مستقل؛
- نبود storage coordinator واحد برای همه‌ی storage transactionها؛
- refactor کامل `webserver.cpp` و `main.cpp` هنوز انجام نشده است.

## Validation

- `python3 -m py_compile scripts/*.py` — PASS
- `python3 scripts/check_tx_paths.py src` — PASS
- `python3 scripts/check_dbc_limits.py .` — PASS
- `python3 scripts/audit_dbc.py --check` — PASS؛ 57 DBC files
- ZIP integrity check — باید پس از بسته‌بندی نهایی اجرا شود.

## محدودیت اعتبارسنجی

PlatformIO در محیط فعلی موجود نیست؛ بنابراین full firmware build/native PlatformIO test و تست سخت‌افزار واقعی را ادعا نمی‌کنیم. رفتار CAN، TFT/touch، SD، Wi-Fi/BLE، OTA و خودرو باید در CI/برد واقعی تأیید شوند.
