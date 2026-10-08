<div align="center" dir="rtl">

# 🚗 CarTouch

### رابط لمسی، وب و BLE برای پایش و کنترل CAN Bus با ESP32-S3

<p>
  <img src="https://img.shields.io/badge/ESP32--S3-N16R8-2ea44f" alt="ESP32-S3">
  <img src="https://img.shields.io/badge/CAN-Dual%20Bus-f97316" alt="Dual CAN">
  <img src="https://img.shields.io/badge/LVGL-TFT%20UI-7c3aed" alt="LVGL">
  <img src="https://img.shields.io/badge/OTA-Web%20%7C%20BLE-2563eb" alt="OTA">
</p>

<p>
  <a href="#-معرفی">معرفی</a> ·
  <a href="#-ویژگیها">ویژگی‌ها</a> ·
  <a href="#-سختافزار">سخت‌افزار</a> ·
  <a href="#-شروع-سریع">شروع سریع</a> ·
  <a href="#-ایمنی">ایمنی</a> ·
  <a href="#-مستندات">مستندات</a>
</p>

</div>

> [!CAUTION]
> **CarTouch مستقیماً با CAN Bus کار می‌کند.** یک فریم نادرست می‌تواند رفتار ناخواسته ایجاد کند. پیش از هر ارسال، ابتدا روی میز و در شرایط کنترل‌شده کار کنید و تا زمان تأیید مسیر فرمان، **Listen-Only** را فعال نگه دارید.

## 🧭 معرفی

**CarTouch** یک firmware مبتنی بر **ESP32-S3** برای دریافت، نمایش و در شرایط کنترل‌شده ارسال پیام‌های **CAN Bus** است. رابط لمسی TFT، داشبورد Web، WebSocket، BLE و USB Serial از مسیرهای اصلی تعامل با سیستم هستند.

معماری پروژه برای دو مسیر CAN طراحی شده است:

- **CAN1:** رابط TWAI از طریق transceiver نوع TJA1051.
- **CAN2:** کنترلر MCP2515 با transceiver نوع TJA1050.
- **TFT/LVGL:** رابط کاربری محلی در build دارای نمایشگر.
- **Web:** پیکربندی، پایش، پروفایل‌ها و عملیات مدیریتی.
- **BLE:** وضعیت، عملیات محدود مدیریتی و BLE OTA.
- **Learn Mode:** یادگیری فرمان‌ها از ترافیک CAN و ذخیره آن‌ها در پروفایل‌های سفارشی.

## ✨ ویژگی‌ها

| حوزه | قابلیت |
|---|---|
| CAN | دریافت و ارسال روی مسیرهای CAN مجاز، Listen-Only و مدیریت وضعیت لینک |
| رابط کاربری | TFT + LVGL و کنترل لمسی |
| Web | داشبورد HTTP، WebSocket و مدیریت پروفایل |
| BLE | Status، عملیات محدود و OTA احراز‌شده |
| Learn Mode | یادگیری فرمان و نگهداری آن در پروفایل سفارشی |
| DBC | بارگذاری، اعتبارسنجی و استفاده برای decode؛ بدون حدس‌زدن فرمان write |
| ذخیره‌سازی | پروفایل‌های سفارشی، تنظیمات و داده‌های کاربر با مسیرهای محافظت‌شده |
| OTA | Firmware OTA از Web و BLE با SHA-256 و مالکیت انحصاری تراکنش |
| تست | مجموعه Native شامل 96 تست موفق در گزارش CI فعلی |

## 🔌 سخت‌افزار

### CAN1 — TJA1051

| سیگنال | ESP32-S3 |
|---|---:|
| TX | GPIO17 |
| RX | GPIO18 |
| تغذیه transceiver | 5V |

### CAN2 — MCP2515 + TJA1050

| سیگنال | GPIO |
|---|---:|
| CS | GPIO15 |
| INT | GPIO16 |
| SCK | GPIO12 |
| MOSI / SI | GPIO11 |
| MISO / SO | GPIO13 |
| Crystal | 8 MHz |

> **هشدار سطح منطقی:** خروجی 5V ماژول MCP2515/TJA1050 را مستقیماً به GPIOهای ESP32-S3 متصل نکنید. برای مسیرهای لازم از level shifting مناسب استفاده کنید.

برای سیم‌بندی کامل و جزئیات تغذیه و OBD-II به [`CarTouch_SPEC.md`](CarTouch_SPEC.md) مراجعه کنید.

## 🚀 شروع سریع

### 1. پیش‌نیاز

- ESP32-S3 سازگار با یکی از board profileهای پروژه
- PlatformIO و toolchain مربوط به ESP32
- رابط CAN مناسب برای CAN1 و/یا CAN2
- منبع تغذیه محافظت‌شده برای محیط خودرو

### 2. Build و Flash

ساخت و فلش را با environment متناسب با سخت‌افزار انجام دهید. جزئیات image و آدرس‌های فلش در [`docs/FLASHING.md`](docs/FLASHING.md) آمده است.

### 3. راه‌اندازی اولیه

- AP پیش‌فرض: `CarTouch`
- رمز پیش‌فرض: `12345678`
- پورت Web: `80`
- آدرس AP را از Serial به‌عنوان مرجع نهایی بخوانید.

> رمز پیش‌فرض برای استقرار واقعی مناسب نیست. پیش از اتصال دستگاه به شبکه یا خودروی واقعی، credentialها را تغییر دهید.

### 4. اولین تست

ابتدا سیستم را بدون ارسال CAN اجرا کنید، وضعیت لینک را بررسی کنید و سپس Learn/Verification را روی میز انجام دهید. نتیجه موفقیت فرمان فقط زمانی معتبر است که ارسال واقعی CAN موفق باشد.

## 🛡️ ایمنی و امنیت

- **Listen-Only** در سطح driver CAN اعمال می‌شود و فقط یک flag نرم‌افزاری در UI نیست.
- مسیرهای ارسال CAN از command guard مشترک عبور می‌کنند.
- انقضای WebSocket session باید ارسال فرمان را مسدود کند.
- Web و BLE نمی‌توانند هم‌زمان مالک OTA باشند.
- فرمان سفارشی بدون actuator metadata معتبر، **fail-closed** است و قابل اجرا نیست.
- Verification پروفایل مشخص را بدون تغییر دادن active vehicle انجام می‌دهد.

### محدودیت‌های امنیتی فعلی

- Web از HTTP و Basic Authentication استفاده می‌کند؛ TLS داخل firmware ارائه نشده است.
- Basic Auth رمز را رمزنگاری نمی‌کند؛ Base64 جایگزین encryption نیست.
- Secure Boot و Flash Encryption توسط پیکربندی فعلی پروژه فعال نشده‌اند.
- credentialهای Wi-Fi و Web در NVS به‌صورت رمزنگاری‌شده ذخیره نمی‌شوند.
- OTA با SHA-256 صحت داده را بررسی می‌کند، اما SHA-256 به‌تنهایی امضای دیجیتال نیست.

جزئیات بیشتر در [`docs/OTA.md`](docs/OTA.md) و [`BLE_OTA.md`](BLE_OTA.md) قرار دارد.

## 🧪 وضعیت تست

گزارش CI ارائه‌شده برای این پروژه نشان می‌دهد:

```text
96 Tests 0 Failures 0 Ignored
native:test_native [PASSED]
```

همچنین بررسی‌های CAN TX، محدودیت DBC، manifest و timing مربوط به MCP2515 موفق بوده‌اند. cppcheck برای هر دو profile نیز PASS شده و فقط 6 هشدار سطح LOW گزارش کرده است.

> این نتایج، تست نرم‌افزاری/CI هستند و به معنی تأیید رفتار روی خودروی واقعی نیستند.

## 📚 مستندات

| سند | موضوع |
|---|---|
| [`CarTouch_SPEC.md`](CarTouch_SPEC.md) | مشخصات فنی، پین‌ها و مرجع سخت‌افزار |
| [`docs/FLASHING.md`](docs/FLASHING.md) | فلش دستی و profileهای حافظه |
| [`docs/MEMORY.md`](docs/MEMORY.md) | اندازه‌گیری heap و stack |
| [`docs/OTA.md`](docs/OTA.md) | Web OTA، integrity و محدودیت‌ها |
| [`BLE_OTA.md`](BLE_OTA.md) | BLE، احراز هویت و BLE OTA |
| [`DBC_AUDIT.md`](DBC_AUDIT.md) | وضعیت مجموعه DBC و audit |
| [`ROOT_ARCHITECTURE_REPAIR_REPORT.md`](ROOT_ARCHITECTURE_REPAIR_REPORT.md) | خلاصه اصلاحات معماری و validation |
| [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) | اطلاعیه‌های وابستگی‌ها |

README فقط نقطه ورود است؛ جزئیات فنی باید در سند مربوط به خودشان نگهداری شوند.

## ⚠️ محدودیت‌های شناخته‌شده

- تست خودروی واقعی انجام نشده است.
- Learn Mode برای سامانه‌های دارای rolling code مناسب نیست.
- DBC parser محدودیت مشخصی برای تعداد پیام‌ها دارد و merge چند فایل DBC انجام نمی‌دهد.
- فایل DBC برای استخراج خودکار فرمان write استفاده نمی‌شود؛ فرمان‌های کنترلی از Learn Mode یا ورود دستی می‌آیند.
- HTTPS/TLS، Secure Boot و Flash Encryption در firmware فعلی فعال نیستند.

## 📄 مجوز

مجوز پروژه در [`LICENSE`](LICENSE) قرار دارد. اطلاعیه وابستگی‌های شخص ثالث در [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) موجود است.

---

<div align="center">

**CarTouch · CAN Bus · ESP32-S3**

</div>
