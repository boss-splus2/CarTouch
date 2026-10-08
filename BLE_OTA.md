# 📡 BLE و BLE OTA

CarTouch سرویس BLE را مستقل از Wi-Fi راه‌اندازی می‌کند و با نامی بر پایه `CarTouch-XXXX` advertise می‌شود.

## سرویس‌ها و characteristicها

| Characteristic | کاربرد |
|---|---|
| **Status** | وضعیت، اعلان‌ها و progress مربوط به OTA |
| **Command** | احراز هویت، عملیات محدود و کنترل OTA |
| **Data** | دریافت byteهای firmware در زمان OTA احراز‌شده |

## احراز هویت و فرمان‌ها

1. به دستگاه متصل شوید.
2. در صورت نیاز لینک BLE را pair کنید.
3. در `Command` مقدار `AUTH:<web-password>` را ارسال کنید.
4. پس از احراز هویت، فقط مجموعه فرمان‌های پشتیبانی‌شده قابل استفاده است.
5. برای پایان session مقدار `LOGOUT` را ارسال کنید؛ disconnect نیز session را پاک می‌کند.

BLE مسیر عمومی برای raw CAN transmission، replay، Learn profile management یا DBC import نیست.

## OTA sequence

1. به دستگاه متصل شوید.
2. SHA-256 دقیق `firmware.bin` را از artifact متناظر بردارید.
3. پیام زیر را ارسال کنید:

```text
START:<web-password>:<firmware-size>:<sha256>
```

4. منتظر `OTA_STARTED` بمانید.
5. byteهای firmware را روی `Data` بنویسید.
6. پیام `END` را ارسال کنید.
7. دستگاه تعداد byteها و SHA-256 را بررسی می‌کند و فقط در صورت تطابق image را finalize می‌کند.

در صورت mismatch، نتیجه `OTA_SHA256_MISMATCH` است و image جدید نباید فعال شود.

> [!WARNING]
> SHA-256 integrity check است، نه digital signature. BLE OTA نیز جایگزین اعتبارسنجی سخت‌افزاری و کنترل منبع image نیست.
