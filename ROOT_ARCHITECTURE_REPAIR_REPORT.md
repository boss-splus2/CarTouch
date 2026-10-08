# 🧩 گزارش اصلاح معماری CarTouch

این سند خلاصه‌ای از اصلاحات انجام‌شده در معماری و مسیرهای ایمنی است؛ شرح جزئیات هر subsystem باید در سند تخصصی خودش نگهداری شود.

## اصلاحات اصلی

- فایل تکراری `src/test_main.cpp` حذف و تست‌ها در مسیر canonical Native نگهداری شدند.
- actuator metadata به فرمان‌های learned/manual اضافه شد و مقدار ناشناخته به‌صورت fail-closed رفتار می‌کند.
- Verification می‌تواند یک profile مشخص را بدون تغییر active vehicle هدف بگیرد.
- ذخیره و حذف profile/index دارای rollback تراکنشی شده است.
- `index.json` به‌عنوان داده مشتق‌شده از profileهای authoritative بازسازی می‌شود.
- transactionهای مربوط به تنظیمات SD و Button از sync policy مشترک استفاده می‌کنند.
- limit پیام‌های DBC به یک منبع مشترک متصل شده است.
- API بلااستفاده network scan حذف شده است.
- مالکیت OTA بین Web و BLE مشترک و انحصاری شده است.
- احراز هویت Web OTA از مسیر احراز هویت مرکزی استفاده می‌کند.
- بررسی مسیرهای CAN TX تضعیف نشده است.

## رفتارهای fail-safe

- فرمانی که actuator metadata معتبر ندارد قابل اجرا نیست.
- built-in DBC write mapping اختراع نشده است.
- credential پیش‌فرض و pin mapping سخت‌افزاری در این اصلاحات تغییر داده نشده‌اند.

## اعتبارسنجی CI

گزارش CI فعلی:

```text
96 Tests 0 Failures 0 Ignored
native:test_native [PASSED]
```

همچنین موارد زیر PASS هستند:

- CAN TX path check
- DBC limit check
- DBC manifest audit
- MCP2515 timing check
- cppcheck برای profile 16 MB
- cppcheck برای profile 4 MB

cppcheck مجموعاً 6 هشدار LOW گزارش کرده و هیچ HIGH یا MEDIUM ندارد.

## مواردی که نباید ادعا شوند

تست واقعی CAN، SPI، TFT، SD، BLE یا OTA روی خودروی واقعی از این گزارش نتیجه نمی‌شود. validation سخت‌افزاری همچنان جداگانه لازم است.
