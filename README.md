<div align="center" dir="rtl">

# 🚗 CarTouch

### سامانه‌ی لمسی، وب و BLE برای پایش و کنترل CAN Bus با ESP32-S3

<p>
  <img src="https://img.shields.io/badge/ESP32--S3-N16R8-2ea44f" alt="ESP32-S3">
  <img src="https://img.shields.io/badge/CAN-Dual%20Bus-f97316" alt="Dual CAN">
  <img src="https://img.shields.io/badge/LVGL-UI-7c3aed" alt="LVGL">
  <img src="https://img.shields.io/badge/License-Proprietary-6b7280" alt="License">
</p>

<p>
  <a href="#-معرفی">معرفی</a> ·
  <a href="#-ویژگیها">ویژگی‌ها</a> ·
  <a href="#-شروع-سریع">شروع سریع</a> ·
  <a href="#-سیمبندی">سیم‌بندی</a> ·
  <a href="#-ایمنی-و-محدودیتها">ایمنی و محدودیت‌ها</a> ·
  <a href="#-مستندات">مستندات</a>
</p>

</div>

<div dir="rtl">

> [!CAUTION]
> **CarTouch مستقیماً با CAN Bus کار می‌کند.** ارسال یک فریم نادرست می‌تواند باعث رفتار ناخواسته‌ی خودرو شود. توسعه و آزمایش را ابتدا روی میز و در شرایط کنترل‌شده انجام دهید. تا زمانی که صحت فرمان‌ها مشخص نشده است، **Listen-Only** را فعال نگه دارید.

> [!WARNING]
> این پروژه در محیط واقعی خودرو **تأیید میدانی نشده است**. Build، تست‌های Native و بررسی‌های ایستا جایگزین آزمون سخت‌افزاری و تست کنترل‌شده روی خودرو نیستند.

## 📌 معرفی

**CarTouch** یک سامانه‌ی embedded مبتنی بر **ESP32-S3** برای پایش و کنترل خودرو از طریق **CAN Bus** است. رابط اصلی دستگاه یک نمایشگر **TFT لمسی** است و دسترسی وب، WebSocket، BLE و USB Serial نیز برای مدیریت و پایش فراهم شده‌اند.

معماری پروژه روی چند اصل استوار است:

- 🔒 مسیر مشترک و محافظت‌شده برای فرمان‌های کنترلی
- 👂 پشتیبانی از **Listen-Only** در سطح درایور CAN
- 🎓 یادگیری فرمان با **Learn Mode** و تأیید صریح پیش از فعال‌سازی
- 🚗 پشتیبانی از پروفایل‌های داخلی DBC و پروفایل‌های سفارشی
- 🛡️ کنترل نشست WebSocket، احراز هویت و محدودیت‌های ایمنی فرمان
- ⬆️ OTA از مسیر Web و BLE با مالکیت انحصاری عملیات OTA
- 📡 دو باس CAN مستقل: TWAI و MCP2515

---

## ✨ ویژگی‌ها

| حوزه | قابلیت |
|---|---|
| 🎛️ رابط کاربری | TFT لمسی با LVGL، Web Dashboard و BLE |
| 📡 CAN | CAN1 با TWAI/TJA1051 و CAN2 با MCP2515/TJA1050 |
| 👁️ مانیتورینگ | پایش CAN، diagnostics، شمارنده‌های RX/TX و وضعیت Bus |
| 🎓 Learn Mode | ضبط baseline/action، تشخیص candidate و چرخه‌ی تأیید فرمان |
| 🚗 پروفایل خودرو | DBC داخلی و پروفایل سفارشی JSON روی SPIFFS |
| 🧾 ضبط CAN | ضبط، فهرست، دانلود و حذف فایل‌های CSV با محدودیت اندازه |
| 🔧 OBD-II | polling غیرمسدودکننده PID و DTC با state machine |
| 🔐 امنیت فرمان | Basic Auth، Session Token، command guard و محدودیت actuator |
| ⬆️ OTA | Firmware / filesystem OTA از Web و firmware OTA از BLE |
| 💾 ذخیره‌سازی | SPIFFS، NVS و SD اختیاری |
| 🔌 Serial | status، config، CAN، OBD، Learn، storage، recorder و control |
| 😴 انرژی | Auto Sleep پس از بی‌فعالیتی و بیداری بر اساس فعالیت CAN |

---

## 🧭 مدل استفاده

```text
                    ┌─────────────────────┐
                    │      CarTouch       │
                    │      ESP32-S3       │
                    └──────────┬──────────┘
                               │
          ┌────────────────────┼────────────────────┐
          │                    │                    │
          ▼                    ▼                    ▼
     ┌──────────┐        ┌──────────┐        ┌──────────┐
     │   TFT    │        │   Web    │        │   BLE    │
     │   LVGL   │        │ HTTP/WS  │        │  AUTH    │
     └──────────┘        └──────────┘        └──────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │ Command Guard /     │
                    │ Vehicle Control     │
                    └──────────┬──────────┘
                               │
                     ┌─────────┴─────────┐
                     ▼                   ▼
                ┌──────────┐       ┌──────────┐
                │   CAN1   │       │   CAN2   │
                │  TWAI    │       │ MCP2515  │
                └──────────┘       └──────────┘
```

> جزئیات معماری، قراردادهای بین ماژول‌ها و منطق ایمنی در [CarTouch_SPEC.md](./CarTouch_SPEC.md) نگهداری می‌شود.

---

## 🚀 شروع سریع

### پیش‌نیازها

- Git
- VS Code
- PlatformIO
- برد ESP32-S3 سازگار با یکی از environmentهای پروژه

### Build

```bash
pio run -e esp32-s3-devkitc-1
```

### Build filesystem

```bash
pio run -e esp32-s3-devkitc-1 -t buildfs
```

### Upload firmware

```bash
pio run -e esp32-s3-devkitc-1 -t upload
```

### Upload filesystem

```bash
pio run -e esp32-s3-devkitc-1 -t uploadfs
```

> [!WARNING]
> `uploadfs` کل filesystem را جایگزین می‌کند و می‌تواند پروفایل‌های سفارشی، ضبط‌های CAN و داده‌های کاربر را پاک کند. قبل از آن داده‌های مهم را export و backup کنید.

### Serial Monitor

```bash
pio device monitor
```

---

## 🔑 اولین اجرا

1. دستگاه را روشن کنید.
2. در صورت نیاز، کالیبراسیون Touch را انجام دهید.
3. در حالت AP به شبکه‌ی **`CarTouch`** متصل شوید.
4. AP با **WPA2-PSK** اجرا می‌شود و رمز پیش‌فرض دستگاه **`12345678`** است.
5. IP چاپ‌شده در Serial را مرجع اصلی قرار دهید؛ در حالت پیش‌فرض Arduino-ESP32 معمولاً `192.168.4.1` است.
6. Web Server روی پورت `80` در دسترس است.
7. نام کاربری مدیریتی پیش‌فرض **`CarTouch`** و رمز **`12345678`** است.

> [!IMPORTANT]
> رمز پیش‌فرض در مخزن قابل مشاهده است. پیش از استفاده در محیطی که دیگران به دستگاه یا شبکه دسترسی دارند، اعتبارنامه را تغییر دهید.

---

## 🔌 سیم‌بندی اصلی

### ESP32-S3

| سیگنال | GPIO پیش‌فرض |
|---|---:|
| CAN1 / TWAI TX | `17` |
| CAN1 / TWAI RX | `18` |
| CAN2 / MCP2515 CS | `15` |
| CAN2 / MCP2515 INT | `16` |
| SPI SCK | `12` |
| SPI MOSI | `11` |
| SPI MISO | `13` |
| TFT CS | `10` |
| TFT DC | `7` |
| TFT RST | `4` |
| TFT Backlight | `21` |
| Touch CS | `14` |

> [!CAUTION]
> ماژول‌های 5V را مستقیماً به GPIOهای ESP32-S3 متصل نکنید. برای مسیرهای موردنیاز MCP2515/TJA1050 از **level shifter مناسب** استفاده کنید و زمین همه‌ی ماژول‌ها را مشترک نگه دارید.

برای جزئیات کامل اتصال CAN1، CAN2، OBD-II، تغذیه و ترمینیشن، به [CarTouch_SPEC.md](./CarTouch_SPEC.md) و مستندات مرتبط مراجعه کنید.

---

## 👂 Listen-Only و Learn Mode

### Listen-Only

Listen-Only باید در سطح درایور CAN اعمال شود و صرفاً یک محدودیت نرم‌افزاری در مسیر فرمان نیست. این حالت برای مشاهده و یادگیری بدون ارسال فرمان استفاده می‌شود.

### Learn Mode

```text
انتخاب فرمان
     │
     ▼
Baseline در Listen-Only
     │
     ▼
فعال‌سازی فرمان واقعی خودرو
     │
     ▼
Capture و مقایسه
     │
     ▼
انتخاب Candidate
     │
     ▼
ذخیره به‌صورت Unverified
     │
     ▼
Verification صریح
     │
     ▼
فعال‌شدن در مسیر کنترل
```

فرمان یادگرفته‌شده تا زمان **تأیید صریح** نباید وارد مسیر عادی اجرای فرمان شود. خودروهایی که از rolling code یا مکانیزم‌های مشابه استفاده می‌کنند تضمین‌شده نیستند.

---

## 🛡️ ایمنی و محدودیت‌ها

| مورد | وضعیت فعلی |
|---|---|
| HTTPS / TLS | ❌ در firmware فعلی وجود ندارد |
| Secure Boot | ❌ فعال نشده است |
| Flash Encryption | ❌ فعال نشده است |
| رمزنگاری credential در NVS | ❌ credentialها به‌صورت متن ساده نگهداری می‌شوند |
| Web Authentication | Basic Auth روی HTTP |
| WebSocket | Session Token با انقضا و کنترل دسترسی |
| BLE Pairing | Just Works؛ بدون MITM |
| Rolling Code | ❌ پشتیبانی تضمین‌شده ندارد |
| تست روی خودروی واقعی | ❌ انجام نشده است |
| استخراج فرمان از DBC | ❌ انجام نمی‌شود؛ فرمان‌ها از Learn Mode یا ورود دستی می‌آیند |
| DBC multi-file merge | ❌ پشتیبانی نمی‌شود |
| DBC message limit | سقف ایمنی فعلی `400` پیام در هر فایل |
| SPIFFS format خودکار | ❌ انجام نمی‌شود تا داده‌ی کاربر ناخواسته حذف نشود |

> [!WARNING]
> Basic Auth روی HTTP رمزنگاری نیست؛ Base64 فقط encoding است. برای استقرار روی شبکه‌ی واقعی، استفاده از یک لایه‌ی TLS/reverse-proxy باید جداگانه در نظر گرفته شود.

> [!NOTE]
> جزئیات امنیت، threat model و محدودیت‌های باقی‌مانده باید در مستندات تخصصی پروژه نگهداری شوند و README صرفاً نقطه‌ی ورود است.

---

## 🧪 وضعیت تست

تست‌های Native و بررسی‌های ایستای پروژه برای کنترل منطق و ساختار استفاده می‌شوند؛ با این حال:

- تست روی خودروی واقعی انجام نشده است.
- موفقیت build به‌تنهایی صحت سیم‌بندی یا عملکرد ترنسیور را اثبات نمی‌کند.
- `twai_driver_install()` و `twai_start()` اتصال فیزیکی CAN را اثبات نمی‌کنند.
- آزمون سخت‌افزاری باید جداگانه و در شرایط کنترل‌شده انجام شود.

برای دستورات تست و بررسی، environmentهای پروژه و [docs/FLASHING.md](./docs/FLASHING.md) را ببینید.

---

## 📚 مستندات

README فقط نقطه‌ی ورود است. جزئیات تخصصی را در فایل مربوط به همان موضوع نگه دارید:

| سند | موضوع |
|---|---|
| [CarTouch_SPEC.md](./CarTouch_SPEC.md) | مشخصات فنی، معماری و قراردادهای سیستم |
| [BLE_OTA.md](./BLE_OTA.md) | پروتکل BLE و OTA از طریق BLE |
| [docs/OTA.md](./docs/OTA.md) | OTA و الزامات مرتبط |
| [docs/FLASHING.md](./docs/FLASHING.md) | روش‌های فلش و آدرس‌های image |
| [docs/MEMORY.md](./docs/MEMORY.md) | وضعیت و بررسی حافظه |
| [DBC_AUDIT.md](./DBC_AUDIT.md) | ممیزی فایل‌های DBC |
| [ROOT_ARCHITECTURE_REPAIR_REPORT.md](./ROOT_ARCHITECTURE_REPAIR_REPORT.md) | گزارش یکپارچه‌سازی معماری و اصلاحات |
| [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md) | اعلان‌ها و مجوزهای وابستگی‌ها و داده‌ها |
| [LICENSE](./LICENSE) | مجوز پروژه |

---

## 📂 ساختار پروژه

```text
CarTouch/
├── src/                    # Firmware و ماژول‌های اصلی
├── test/                   # تست‌های Native
├── data/                   # Web UI و DBCهای بسته‌بندی‌شده
├── docs/                   # مستندات تخصصی
├── scripts/                # بررسی‌های خودکار و ابزارهای CI
├── boards/                 # تعریف environmentهای برد
├── platformio.ini          # تنظیمات PlatformIO
├── CarTouch_SPEC.md        # مشخصات فنی
├── BLE_OTA.md              # BLE / OTA
└── README.md               # نقطه ورود پروژه
```

---

## 📋 دستورات USB Serial

| دستور | کاربرد |
|---|---|
| `help` | نمایش دستورات |
| `status` | وضعیت سیستم و ماژول‌ها |
| `memory` | وضعیت heap / PSRAM / stack |
| `config` | تنظیمات CAN و انتخاب‌های اصلی |
| `can` | وضعیت و diagnostics باس‌ها |
| `obd` | آخرین داده OBD |
| `dtc read` / `dtc clear` / `dtc status` | عملیات DTC |
| `learn` | وضعیت Learn Mode |
| `record status` / `record start` / `record stop` | کنترل ضبط CAN |
| `storage` | وضعیت SPIFFS و پروفایل‌ها |
| `sd status` / `sd cs` / `sd store` / `sd reset` | مدیریت SD |
| `btn status` / `btn off` / `btn gpio` / `btn adc` | مدیریت Five-way |
| `errors` | مشاهده‌ی Error Log |
| `control <command>` | ارسال درخواست کنترل از مسیر guard مشترک |

---

## ⚠️ قبل از اتصال به خودرو

- CANH و CANL را با قطبیت صحیح وصل کنید.
- زمین مشترک را بررسی کنید.
- سطح منطقی ماژول CAN را با ESP32-S3 تطبیق دهید.
- ترمینیشن اضافه را روی باس خودرو وارد نکنید.
- ابتدا با **Listen-Only** شروع کنید.
- فرمان‌های Learn شده را قبل از فعال‌سازی بررسی کنید.
- هیچ فرمانی را صرفاً بر اساس نام یا حدس DBC تولید نکنید.
- قبل از `uploadfs` از داده‌های کاربر backup بگیرید.

---

<div align="center">

**CarTouch** · Embedded CAN Control & Monitoring

</div>

</div>
