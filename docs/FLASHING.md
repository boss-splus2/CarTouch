# 🔧 فلش دستی

> این سند فقط فرآیند فلش را توضیح می‌دهد. مشخصات سخت‌افزار در [`../CarTouch_SPEC.md`](../CarTouch_SPEC.md) و محدودیت‌های OTA در [`OTA.md`](OTA.md) قرار دارند.

## 16 MB

از imageهای **یک profile واحد** استفاده کنید:

| Image | Address |
|---|---:|
| `bootloader.bin` | `0x0` |
| `partitions.bin` | `0x8000` |
| `boot_app0.bin` | `0xE000` |
| `firmware.bin` | `0x10000` |
| `spiffs.bin` | `0xA10000` |

تنظیمات پایه: ESP32-S3، فلش 16 MB، QIO و 80 MHz. در صورت مشکل boot، DIO فقط به‌عنوان مسیر عیب‌یابی بررسی شود.

## 4 MB

در profileهای 4 MB، آدرس‌های bootloader، partition table و firmware همان مسیر بالا هستند و `spiffs.bin` از `0x310000` شروع می‌شود.

Filesystem کوچک‌شده نمی‌تواند مجموعه کامل DBC را در خود جای دهد. imageهای profileهای مختلف را با هم ترکیب نکنید.

> [!WARNING]
> اجرای `uploadfs` می‌تواند filesystem را جایگزین کند. پیش از آن از profileهای کاربر، recordingها و DBCهای کاربر پشتیبان بگیرید.

## بررسی پس از فلش

1. دستگاه را با Serial متصل کنید.
2. بوت و وضعیت CAN را بررسی کنید.
3. ابتدا Listen-Only را نگه دارید.
4. سپس فقط روی bench setup وارد Learn/Verification شوید.
