# 🔄 OTA

OTA برای firmware از مسیر Web و BLE در دسترس است. هر تراکنش باید مالک مشخص داشته باشد؛ Web و BLE نمی‌توانند هم‌زمان یک OTA را مالک شوند.

## Web OTA

Web OTA می‌تواند image firmware و filesystem را دریافت کند. برای firmware، SHA-256 مربوط به **همان artifact** باید ارائه شود.

| کنترل | وضعیت |
|---|---|
| Firmware target | inactive application slot |
| Integrity | SHA-256 |
| Digital signature | ❌ وجود ندارد |
| HTTPS/TLS داخل firmware | ❌ وجود ندارد |
| Automatic post-boot rollback | ❌ وجود ندارد |
| Filesystem automatic power-loss recovery | ❌ وجود ندارد |
| محافظت از user data هنگام filesystem replacement | ✅ وجود دارد |

## BLE OTA

BLE OTA نیازمند احراز هویت با password فعلی و SHA-256 image است. فرم legacy که hash ندارد پذیرفته نمی‌شود.

جزئیات protocol و ترتیب پیام‌ها در [`../BLE_OTA.md`](../BLE_OTA.md) مستند شده است.

## نکات امنیتی

> [!WARNING]
> SHA-256 فقط **integrity** را بررسی می‌کند؛ امضای دیجیتال و اصالت منبع image را اثبات نمی‌کند.

> [!WARNING]
> OTA روی HTTP بدون TLS در firmware فعلی اجرا می‌شود. برای deployment واقعی باید یک مسیر امن و کنترل‌شده برای انتقال image در نظر گرفته شود.

## مالکیت تراکنش

در شروع OTA، مسیر Web یا BLE مالک تراکنش را می‌گیرد. مسیر دیگر نباید update جاری را متوقف یا جایگزین کند. مالکیت پس از پایان موفق یا خطای قابل‌شناسایی آزاد می‌شود.
