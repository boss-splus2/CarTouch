# CarTouch OTA

این سند رفتار واقعی OTA در نسخه فعلی را توضیح می‌دهد.

## Web OTA

- firmware و filesystem از Web احراز‌شده قابل به‌روزرسانی هستند.
- Web OTA از مسیر مرکزی authentication/lockout استفاده می‌کند.
- مالکیت OTA با `src/ct_ota_lock.*` ثبت می‌شود تا BLE و Web نتوانند هم‌زمان یک عملیات firmware را کنترل کنند.
- firmware روی app slot غیرفعال نوشته می‌شود و تا finalize شدن کامل، slot جدید فعال نمی‌شود.
- SHA-256 فقط integrity check است و امضای دیجیتال یا اصالت سازنده را ثابت نمی‌کند.
- HTTPS در پروژه وجود ندارد؛ Web OTA روی HTTP اجرا می‌شود.

## BLE OTA

BLE OTA به password فعلی و digest دقیق artifact نیاز دارد. قالب قدیمی `START` بدون SHA-256 رد می‌شود. جزئیات پروتکل در `BLE_OTA.md` است. BLE فقط در صورت موفقیت `tryAcquire(CT_OTA_OWNER_BLE)` مالک OTA می‌شود و نمی‌تواند OTA متعلق به Web را abort کند.

## مالکیت و lifecycle

در هر لحظه فقط یکی از `NONE`, `WEB`, `BLE` owner است. release در مسیرهای موفقیت و خطای شناخته‌شده انجام می‌شود.

> [!WARNING]
> **محدودیت باقی‌مانده:** اگر کلاینت Web در میانه‌ی upload بدون رسیدن به مسیر cleanup قطع شود، timeout خودکار مستقل برای آزادکردن lock هنوز وجود ندارد. این مورد باید پیش از ادعای lifecycle کامل/self-healing OTA با watchdog/timeout تکمیل شود.

## Filesystem OTA

filesystem یک partition دارد و slot دوم ندارد. شکست digest یا قطع برق می‌تواند filesystem را unusable کند. Web OTA از نوشتن زمانی که داده‌ی داخلی محافظت‌شده وجود دارد جلوگیری می‌کند، اما restore خودکار وجود ندارد.

## اعتبار artifact

SHA-256 را فقط از artifact مورداعتماد همان release بردارید. digest بدون signature در برابر attacker فعال اصالت release را اثبات نمی‌کند.

منبع یگانه نسخه firmware: `CAR_TOUCH_FIRMWARE_VERSION` در `src/config.h`.
