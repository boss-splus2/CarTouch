<div align="center">

<h1 dir="rtl" id="-cartouch">🚗 CarTouch</h1>

**پایش و کنترل خودرو با ESP32-S3 و CAN Bus — لمسی، وب، و یادگیرنده**

![ESP32-S3](https://img.shields.io/badge/ESP32--S3-N16R8-green)
![CAN Bus](https://img.shields.io/badge/CAN-Dual%20Bus-orange)
![LVGL](https://img.shields.io/badge/LVGL-8.4-purple)
![License](https://img.shields.io/badge/License-Proprietary-lightgrey)

</div>

<div dir="rtl">

<div class="markdown-alert markdown-alert-warning" dir="rtl">
<p class="markdown-alert-title">Warning</p>
<p>‏CarTouch مستقیماً روی <b>CAN Bus زنده‌ی خودرو</b> کار می‌کند. ارسال یک فریم اشتباه می‌تواند باعث رفتار ناخواسته‌ی خودرو شود. توسعه و آزمایش را ابتدا روی میز و در شرایط کنترل‌شده انجام دهید، حالت <b>Listen-Only</b> را تا زمان اطمینان فعال نگه دارید و هر فرمان کنترلی را فقط روی خودرویی اجرا کنید که اختیار آزمایش آن را دارید.</p>
</div>

<h2 dir="rtl" id="-فهرست">📑 فهرست</h2>

<ol dir="rtl">
<li><a href="#-معرفی">معرفی</a></li>
<li><a href="#-وضعیت-و-محدودیتها">وضعیت و محدودیت‌ها</a></li>
<li><a href="#-learn-mode--یادگیری-فرمان-از-خودرو">Learn Mode — یادگیری فرمان از خودرو</a></li>
<li><a href="#-قطعات-مورد-نیاز">قطعات مورد نیاز</a></li>
<li><a href="#-سیمبندی">سیم‌بندی</a></li>
<li><a href="#-نصب-و-راهاندازی">نصب و راه‌اندازی</a></li>
<li><a href="#-اولین-اجرا">اولین اجرا</a></li>
<li><a href="#-معماری-و-امنیت">معماری و امنیت</a></li>
<li><a href="#-ساختار-پروژه">ساختار پروژه</a></li>
<li><a href="#-مستندات-بیشتر">مستندات بیشتر</a></li>
</ol>

---

<h2 dir="rtl" id="-معرفی">✨ معرفی</h2>

‏**CarTouch** یک سامانه‌ی embedded مبتنی بر **ESP32-S3** برای پایش و کنترل خودرو از طریق CAN Bus است. رابط اصلی دستگاه یک **TFT لمسی** است و یک **Web Dashboard** نیز از طریق Wi‑Fi در دسترس است. فرمان‌های کنترلی فقط پس از تأیید صریح شما اجرا می‌شوند.

<table dir="rtl">
<tr>
<td align="center"><b></b></td>
<td align="center"><b>قابلیت</b></td>
</tr>
<tr>
<td align="center">🎮 <b>کنترل</b></td>
<td align="center">TFT لمسی با LVGL (تب‌های Control، Dashboard، Learn و Settings) + Web Dashboard با HTTP API و WebSocket</td>
</tr>
<tr>
<td align="center">📊 <b>مانیتورینگ</b></td>
<td align="center">خواندن OBD-II با state machine <b>غیرمسدودکننده</b>؛ <code>loop()</code> اصلی هرگز روی پاسخ ECU مسدود نمی‌شود</td>
</tr>
<tr>
<td align="center">🔀 <b>Dual CAN</b></td>
<td align="center">CAN1 با TWAI/TJA1051 و CAN2 با MCP2515/TJA1050 (کریستال 8 MHz)، با وضعیت، Listen-Only، diagnostics و recovery مستقل برای هر باس</td>
</tr>
<tr>
<td align="center">🔎 <b>CAN Monitor</b></td>
<td align="center">نمایش و ضبط شنودی CAN1، CAN2 یا هر دو در Web UI احراز‌شده؛ حداکثر 100 فایل CSV با سقف 256KiB برای هر فایل در SPIFFS، فهرست/دانلود/حذف، شمارش frame drop و توقف امن. Timestamp برحسب میلی‌ثانیه از uptime است؛ replay و import ارائه نشده‌اند</td>
</tr>
<tr>
<td align="center">🎓 <b>یادگیری</b></td>
<td align="center">ضبط baseline/action روی کانال منتخب CAN1 یا CAN2، تشخیص candidate، ذخیره و چرخه‌ی تأیید؛ همچنین ورود دستی CAN ID و بایت‌ها</td>
</tr>
<tr>
<td align="center">✅ <b>چرخه‌ی تأیید</b></td>
<td align="center">فرمان‌های یادگرفته‌شده تا زمان تأیید صریح کاربر اجرا نمی‌شوند</td>
</tr>
<tr>
<td align="center">💾 <b>پروفایل خودرو</b></td>
<td align="center">انتخاب پروفایل DBC داخلی یا پروفایل سفارشی (JSON روی SPIFFS) با فهرست، import/export و مدیریت فرمان‌ها</td>
</tr>
<tr>
<td align="center">🛡️ <b>ایمنی</b></td>
<td align="center">Listen-Only، محدودیت فاصله‌ی فرمان و duty-cycle برای actuatorهای مکانیکی، توکن نشست WebSocket و ابطال نشست‌ها هنگام تغییر رمز</td>
</tr>
<tr>
<td align="center">🧾 <b>لاگ و وضعیت</b></td>
<td align="center">لاگ خطا (ring buffer + شمارنده‌های پایدار NVS) و وضعیت runtime ماژول‌ها روی TFT و Web</td>
</tr>
<tr>
<td align="center">🔌 <b>USB Serial</b></td>
<td align="center">کنسول headless برای status/config/CAN/OBD/Learn/storage/errors و شروع/توقف/فهرست وضعیت ضبط؛ فرمان‌های کنترل فقط از مسیر guarded موجود عبور می‌کنند</td>
</tr>
<tr>
<td align="center">🔋 <b>انرژی</b></td>
<td align="center">Auto Sleep پس از ۱۰ دقیقه بی‌فعالیتی و بیدار شدن بر اساس فعالیت CAN</td>
</tr>
<tr>
<td align="center">⬆️ <b>به‌روزرسانی</b></td>
<td align="center">OTA firmware/filesystem از طریق Web احراز‌شده و BLE با رمز؛ BLE همچنین DTC و Recorder را با AUTH، session متصل به connection و همان صف فرمان مشترک ارائه می‌دهد، اما جایگزین کامل Web UI نیست</td>
</tr>
<tr>
<td align="center">⚙️ <b>تنظیمات CAN</b></td>
<td align="center">تنظیم پین، bitrate و mode هر دو باس، به‌علاوه انتخاب مستقل کانال OBD و Learn از Web؛ اعمال پیکربندی پس از reboot</td>
</tr>
</table>

---

<h2 dir="rtl" id="-وضعیت-و-محدودیتها">🧭 وضعیت و محدودیت‌ها</h2>

قبل از شروع بدانید چه چیزی پشتیبانی می‌شود و چه محدودیت‌هایی وجود دارد:

<table dir="rtl">
<tr>
<td align="center"><b>موضوع</b></td>
<td align="center"><b>وضعیت</b></td>
</tr>
<tr>
<td align="center">خواندن OBD-II</td>
<td align="center">✅ polling PIDها در Single Frame؛ DTC Mode 03 با state machine غیرمسدودکننده و پشتیبانی Single/First/Consecutive Frame و Flow Control؛ پاک‌کردن Mode 04 فقط پس از تأیید ECU. کانال OBD مستقل است و TX fallback خودکار وجود ندارد؛ ISO-TP عمومی برای سرویس‌های دلخواه هنوز پشتیبانی نمی‌شود</td>
</tr>
<tr>
<td align="center">ولتاژ ECU</td>
<td align="center">✅ فقط از OBD PID <code>0x42</code> و در بازه‌ی ۶ تا ۳۶ ولت؛ مقدار ناموجود یا منقضی‌شده <code>N/A</code> نمایش داده می‌شود. ورودی ADC مستقل، calibration و درصد شارژ پشتیبانی نمی‌شود</td>
</tr>
<tr>
<td align="center">DTC</td>
<td align="center">✅ خواندن و پاک‌کردن در Dashboard وب و USB Serial؛ نمایش NRC و فهرست کدها. نتیجه‌ی فیزیکی فقط با ECU سازگار قابل تأیید است</td>
</tr>
<tr>
<td align="center">Learn Mode</td>
<td align="center">⚠️ برای خودروهایی که فرمان‌های کنترلی آن‌ها <b>rolling code</b> یا مکانیزم مشابه دارند تضمین‌شده نیست</td>
</tr>
<tr>
<td align="center">پارسر DBC</td>
<td align="center">✅ Intel و Motorola؛ سقف ایمنی <b>۴۰۰ پیام</b> در هر فایل. <code>CM_</code> و <code>VAL_</code> تفسیر نمی‌شوند. هر انتخاب خودرو یک فایل DBC بارگذاری می‌کند و فایل‌های چندمنبعی به‌صورت خودکار merge نمی‌شوند</td>
</tr>
<tr>
<td align="center">DBC با شناسه‌ی Extended</td>
<td align="center">✅ با پرچم <code>isExtended</code> نگهداری می‌شوند؛ فایل‌هایی که شناسه‌ی ۲۹ بیتی را بدون bit 31 ذخیره کرده‌اند نیز تشخیص داده می‌شوند</td>
</tr>
<tr>
<td align="center">تنظیمات CAN1/CAN2</td>
<td align="center">✅ پین‌های رزروشده و تداخل GPIO بررسی می‌شود و تغییرها پس از ذخیره و reboot اعمال می‌شوند. MCP2515 با کریستال 8 MHz فقط bitrateهای 100، 125، 250، 500 و 1000 kbps را می‌پذیرد</td>
</tr>
<tr>
<td align="center">اتصال فیزیکی CAN</td>
<td align="center">⚠️ موفقیت <code>twai_driver_install()</code> یا <code>twai_start()</code> به‌تنهایی اتصال فیزیکی ترنسیور و سیم‌کشی صحیح را ثابت نمی‌کند</td>
</tr>
<tr>
<td align="center">SPIFFS</td>
<td align="center">اگر mount نشود، سیستم آن را <b>خودکار format نمی‌کند</b>؛ فایل‌های سفارشی حفظ می‌شوند و قابلیت‌های وابسته به filesystem تا رفع مشکل غیرفعال می‌مانند</td>
</tr>
<tr>
<td align="center">HTTPS</td>
<td align="center">❌ ندارد — ترافیک وب رمزنگاری نمی‌شود</td>
</tr>
<tr>
<td align="center">Secure Boot / Flash Encryption</td>
<td align="center">❌ در تنظیمات پروژه فعال نشده‌اند</td>
</tr>
<tr>
<td align="center">BLE Pairing</td>
<td align="center">⚠️ از نوع Just Works است (بدون حفاظت MITM)؛ لینک رمزنگاری می‌شود ولی هویت طرف مقابل در زمان pairing تأیید نمی‌شود. جزئیات در <a href="./BLE_OTA.md"><code>BLE_OTA.md</code></a></td>
</tr>
<tr>
<td align="center">کیفیت CI</td>
<td align="center">✅ در آخرین گزارش: 96 تست native بدون خطا، بررسی مسیرهای TX و محدودیت DBC و جدول bit-timing برای MCP2515 موفق، و cppcheck بدون هشدار HIGH/MEDIUM (فقط یک هشدار LOW سبکی در هدر کتابخانه‌ی MCP2515 برای هر پروفایل). عدد دقیق همیشه از CI همان commit معتبر است</td>
</tr>
<tr>
<td align="center">تست روی خودرو</td>
<td align="center">بیلد CI معیار اصلی صحت کامپایل است؛ اجرای واقعی روی خودرو همچنان نیازمند سخت‌افزار و آزمون کنترل‌شده است</td>
</tr>
</table>

---

<h2 dir="rtl" id="-learn-mode--یادگیری-فرمان-از-خودرو">🎓 Learn Mode — یادگیری فرمان از خودرو</h2>

هر خودرو فرمان‌های کنترلی مخصوص خودش را دارد. Learn Mode با شنود امن (Listen-Only) فرمان را از دکمه‌ی فیزیکی خودرو یاد می‌گیرد:

```mermaid
flowchart LR
    A["انتخاب برچسب<br/>مثلاً قفل همه درب‌ها"] --> B["۲ ثانیه شنود پیش‌زمینه<br/>بدون ارسال هیچ فرمانی"]
    B --> C["دکمه‌ی واقعی خودرو<br/>را می‌زنید"]
    C --> D["۲ ثانیه ضبط<br/>و مقایسه با پیش‌زمینه"]
    D --> E["انتخاب کاندید درست"]
    E --> F["ذخیره ⚠️<br/>تأییدنشده"]
    F --> G["تأیید صریح<br/>و یک بار آزمایش"]
    G --> H["✅ فعال در تب کنترل"]
```

<ol dir="rtl">
<li>دستگاه را به CAN Bus وصل کنید (بخش <a href="#-سیمبندی">سیم‌بندی</a>).</li>
<li>از تب «Learn» (روی TFT یا وب) یک برچسب فرمان انتخاب کنید.</li>
<li>دستگاه ۲ ثانیه پیام‌های پیش‌زمینه (baseline) را می‌شنود — <b>بدون ارسال هیچ فرمانی</b>.</li>
<li>دکمه‌ی فیزیکی خودرو را بزنید؛ دستگاه پیام‌های جدید یا تغییرکرده را ضبط می‌کند.</li>
<li>از میان کاندیدهای یافت‌شده، گزینه‌ی درست را انتخاب کنید.</li>
<li>فرمان با وضعیت «تأییدنشده» (⚠️) ذخیره می‌شود و <b>قابل اجرا نیست</b>.</li>
<li>با یک تأیید جداگانه (که CAN ID و بایت‌های واقعی را نشان می‌دهد) آن را یک‌بار آزمایش و نتیجه را دستی تأیید می‌کنید.</li>
<li>فقط پس از این مرحله، فرمان در مسیر عادی کنترل فعال می‌شود.</li>
</ol>

<blockquote dir="rtl">
<p>تشخیص کاندیدها نیمه‌خودکار است و تأیید نهایی همیشه با شماست. هیچ فرمان کنترلی از Learn Mode پیش از تأیید صریح کاربر ارسال نمی‌شود.</p>
<p>معماری، الگوریتم و قوانین ایمنی: <a href="./CarTouch_SPEC.md"><code>CarTouch_SPEC.md</code></a></p>
</blockquote>

---

<h2 dir="rtl" id="-قطعات-مورد-نیاز">🔩 قطعات مورد نیاز</h2>

<table dir="rtl">
<tr>
<td align="center"><b>قطعه</b></td>
<td align="center"><b>تعداد</b></td>
<td align="center"><b>توضیحات</b></td>
</tr>
<tr>
<td align="center"><b>ESP32-S3 DevKitC-1</b> (ماژول N16R8)</td>
<td align="center">۱</td>
<td align="center">فلش ۱۶ مگابایت + PSRAM هشت مگابایتی</td>
</tr>
<tr>
<td align="center"><b>ماژول CAN با TJA1051</b> (مثل CJMCU-1051)</td>
<td align="center">۱</td>
<td align="center">برای CAN1 (TWAI)؛ VCC برابر 5V. اگر برد پایه‌ی <code>VIO</code> دارد آن را به 3.3V وصل کنید</td>
</tr>
<tr>
<td align="center"><b>ماژول MCP2515 + TJA1050</b> (کریستال 8 MHz)</td>
<td align="center">۱</td>
<td align="center">برای CAN2؛ VCC برابر 5V و SPI آن با TFT/Touch مشترک است</td>
</tr>
<tr>
<td align="center"><b>نمایشگر TFT با ILI9341</b></td>
<td align="center">۱</td>
<td align="center">SPI، همراه Touch Controller از نوع XPT2046</td>
</tr>
<tr>
<td align="center"><b>مبدل تغذیه</b></td>
<td align="center">۱</td>
<td align="center">ورودی تغذیه‌ی خودرو با مبدل مناسب به 3.3V</td>
</tr>
<tr>
<td align="center">سیم jumper و محفظه</td>
<td align="center">—</td>
<td align="center">برای اتصال و نصب داخل خودرو</td>
</tr>
</table>

<div class="markdown-alert markdown-alert-important" dir="rtl">
<p class="markdown-alert-title">Important</p>
<p>پروژه برای پنل <b>ILI9341</b> تنظیم شده است (<code>-DILI9341_DRIVER=1</code> در <code>platformio.ini</code>). اگر پنل شما کنترلر دیگری دارد، درایور را در <code>platformio.ini</code> عوض کنید و ابعاد رابط کاربری را با آن تطبیق دهید.</p>
</div>

---

<h2 dir="rtl" id="-سیمبندی">🔌 سیم‌بندی</h2>

<details open>
<summary><b>پین‌های ESP32-S3 برای TFT، تاچ و CAN</b></summary>

<table dir="rtl">
<tr>
<td align="center"><b>سیگنال</b></td>
<td align="center"><b>GPIO</b></td>
</tr>
<tr><td align="center">CAN1 / TWAI TX</td><td align="center">17</td></tr>
<tr><td align="center">CAN1 / TWAI RX</td><td align="center">18</td></tr>
<tr><td align="center">TFT CS</td><td align="center">10</td></tr>
<tr><td align="center">TFT DC</td><td align="center">7</td></tr>
<tr><td align="center">TFT RST</td><td align="center">4</td></tr>
<tr><td align="center">SPI MOSI</td><td align="center">11</td></tr>
<tr><td align="center">SPI MISO</td><td align="center">13</td></tr>
<tr><td align="center">SPI SCLK</td><td align="center">12</td></tr>
<tr><td align="center">TFT Backlight</td><td align="center">21</td></tr>
<tr><td align="center">Touch CS</td><td align="center">14</td></tr>
<tr><td align="center">CAN2 / MCP2515 CS</td><td align="center">15</td></tr>
<tr><td align="center">CAN2 / MCP2515 INT</td><td align="center">16</td></tr>
</table>

<blockquote dir="rtl">
<p>پین‌های نهایی را همیشه از <code>src/config.h</code> و <code>platformio.ini</code> بررسی کنید.</p>
</blockquote>

</details>

<details open>
<summary><b>CAN2 (MCP2515)</b></summary>

CAN2 از کریستال 8 MHz روی MCP2515 و ترنسیور TJA1050 و SPI مشترک با TFT/Touch استفاده می‌کند. SPI HAL در Arduino-ESP32 transactionهای هم‌زمان روی bus را با mutex داخلی سری می‌کند؛ هر کد task جدید باید انتقال SPI را با `beginTransaction()`/`endTransaction()` انجام دهد. CS و INT قابل تنظیم هستند و پیش‌فرض آن‌ها GPIO15 و GPIO16 است. زمین ماژول‌ها باید مشترک باشد.

<div class="markdown-alert markdown-alert-caution" dir="rtl">
<p class="markdown-alert-title">Caution</p>
<p>GPIOهای ESP32-S3 مقاوم در برابر 5V نیستند. سطح منطقی TJA1051 به variant برد بستگی دارد و خروجی RXD نباید بیش از 3.3V به ESP32-S3 بدهد. بسیاری از بردهای MCP2515/TJA1050 با منطق 5V کار می‌کنند؛ در نبود level shifter روی خود برد، برای SCK/MOSI/CS و نیز MISO/INT مبدل سطح مناسب قرار دهید. هیچ سیگنال 5V را مستقیم به GPIO متصل نکنید.</p>
</div>

</details>

---

<h2 dir="rtl" id="-نصب-و-راهاندازی">🚀 نصب و راه‌اندازی</h2>

**پیش‌نیازها:** Git + [VS Code](https://code.visualstudio.com/) + افزونه‌ی PlatformIO

```bash
pio run -e esp32-s3-devkitc-1                 # N16R8، فلش 16MB و PSRAM نوع OPI
pio run -e esp32-s3-headless                  # N16R8 بدون init نمایشگر/تاچ
pio run -e esp32-s3-4mb                       # فلش 4MB بدون PSRAM
pio run -e esp32-s3-4mb-psram                 # فلش 4MB با PSRAM نوع QSPI
pio run -e esp32-s3-devkitc-1 -t buildfs      # filesystem کامل 16MB
pio run -e esp32-s3-4mb -t buildfs            # filesystem کوچک 4MB
pio run -e esp32-s3-4mb-psram -t buildfs      # همان filesystem برای PSRAM نوع QSPI
pio run -e esp32-s3-devkitc-1 -t upload       # آپلود firmware
pio run -e esp32-s3-devkitc-1 -t uploadfs     # آپلود filesystem (وب و DBC)
pio device monitor                            # مانیتور سریال
```

<p class="markdown-alert markdown-alert-warning" dir="rtl"><b>هشدار:</b> فرمان دستی <code>uploadfs</code> کل SPIFFS را جایگزین می‌کند و می‌تواند پروفایل‌های یادگرفته‌شده، ضبط‌های CAN و DBCهای کاربر را پاک کند. قبل از اجرا داده‌ها را export کرده و در محل دیگری پشتیبان بگیرید. OTA وب تا وقتی پروفایل، ضبط CAN یا مانیفست/فایل بازیابی DBC کاربر در SPIFFS داخلی وجود دارد، جایگزینی را رد می‌کند؛ خود OTA پشتیبان خودکار نمی‌سازد.</p>

<div class="markdown-alert markdown-alert-warning" dir="rtl">
<p class="markdown-alert-title">محدودیت پیکربندی حافظه</p>
<p>پروفایل اصلی <code>esp32-s3-devkitc-1</code> از برد N16R8 با فلش 16MB و PSRAM نوع OPI استفاده می‌کند. پروفایل‌های <code>esp32-s3-4mb</code> و <code>esp32-s3-4mb-psram</code> جدول واقعی 4MB و filesystem کوچک‌شده دارند؛ دومی برای PSRAM نوع QSPI است. firmware فعلی در هر دو پروفایل نزدیک به سقف 1.5MB هر OTA slot است، بنابراین تغییرات بزرگ بعدی ممکن است به جدول پارتیشن تازه نیاز داشته باشد.</p>
</div>

<p>در پروفایل 4MB فقط Web UI و 10 فایل DBC منطقه‌ای/وارداتی منتخب بسته‌بندی می‌شوند؛ مجموعه‌ی کامل 57 فایل در <code>data/dbc</code> دست‌نخورده می‌ماند و برای پروفایل 16MB است. فهرست خودرو در زمان اجرا فقط DBCهای موجود در filesystem را نشان می‌دهد. DBCهای کاربر و ضبط CAN از سیاست انتخاب SPIFFS/SD استفاده می‌کنند؛ پروفایل‌های سفارشی فعلاً فقط در SPIFFS ذخیره می‌شوند. SD پیش‌فرض غیرفعال است، خودکار format نمی‌شود و mount/removal آن روی سخت‌افزار هنوز نیازمند آزمون است. صفحه‌ی وب ترجیح مستقل DBC، پروفایل، ضبط و پشتیبان را ذخیره می‌کند، اما در حال حاضر فقط ترجیح DBC و ضبط به مسیر ذخیره‌سازی وصل است؛ ذخیره‌ی پروفایل روی SD و backup/restore واقعی موجود نیست. درایور اختیاری Five-way از GPIO یا ADC، تنظیم Serial و رویدادهای کوتاه/بلند را دارد و ورودی keypad را به LVGL می‌دهد؛ تنظیم از Web/BLE و آزمون عملی روی برد هنوز انجام نشده‌اند. تنظیم پین نمایشگر در زمان اجرا و کنترلرهای TFT غیر از ILI9341 نیز پشتیبانی نمی‌شوند.</p>

<p>پروفایل <code>esp32-s3-headless</code> نمایشگر، تاچ و بافرهای LVGL را init/رزرو نمی‌کند؛ دسترسی شبکه و BLE مستقل می‌ماند و از جدول 16MB استفاده می‌کند. همه‌ی پروفایل‌های نمایش‌دار فعلی روی پین‌بندی ثابت ILI9341/XPT2046 در <code>platformio.ini</code> تنظیم شده‌اند.</p>

<details>
<summary><b>بررسی کیفیت (static analysis و تست‌های native)</b></summary>

```bash
pio check -e esp32-s3-devkitc-1 --skip-packages
pio test -e native
pio run -e esp32-s3-4mb
pio run -e esp32-s3-4mb-psram
pio run -e esp32-s3-headless
```

</details>

<details>
<summary><b>فلش دستی با esptool</b></summary>

پروفایل اصلی از جدول <code>cartouch_16MB.csv</code> استفاده می‌کند. پروفایل‌های 4MB از <code>cartouch_4MB.csv</code> و filesystem کوچک‌شده از Web UI و DBCهای منتخب استفاده می‌کنند؛ فایل‌های اصلی در <code>data/</code> حذف یا تغییر نمی‌کنند. فضای SPIFFS چهارمگابایتی برای کل مجموعه‌ی DBC کافی نیست. برای نقشه‌ی کامل پارتیشن‌ها و imageهای هر دو اندازه، <code>CarTouch_SPEC.md</code> بخش 19 («نقشه‌ی آدرس‌های فلش») را ببینید.

آدرس فایل‌ها برای جدول 16MB:

<table dir="rtl">
<tr>
<td align="center"><b>فایل</b></td>
<td align="center"><b>آدرس</b></td>
</tr>
<tr><td align="center"><code>bootloader.bin</code></td><td align="center"><code>0x0</code></td></tr>
<tr><td align="center"><code>partitions.bin</code></td><td align="center"><code>0x8000</code></td></tr>
<tr><td align="center"><code>boot_app0.bin</code></td><td align="center"><code>0xE000</code></td></tr>
<tr><td align="center"><code>firmware.bin</code></td><td align="center"><code>0x10000</code></td></tr>
<tr><td align="center"><code>spiffs.bin</code></td><td align="center"><code>0xA10000</code></td></tr>
</table>
تنظیمات ابزارهای فلش گرافیکی (مثل ESP32 Flash/Erase): چیپ ESP32-S3، فلش 16MB (یا 4MB برای پروفایل‌های 4MB)، فرکانس 80MHz و حالت QIO (در صورت بوت‌نشدن، DIO). `boot_app0.bin` را حتماً جداگانه روی `0xE000` اضافه کنید. فایل‌ها باید همه از یک پوشه‌ی بیلد (یک پروفایل) باشند.

برای جدول 4MB، آدرس `spiffs.bin` برابر `0x310000` است؛ سایر imageهای سیستم در همان آدرس‌های جدول بالا قرار دارند. جدول و اندازه‌ی دقیق همه‌ی پارتیشن‌ها در بخش 19 مشخصات فنی آمده است. `boot_app0.bin` در `0xE000` داخل پارتیشن `otadata` نوشته می‌شود و پارتیشن جدا نیست.

</details>

<div class="markdown-alert markdown-alert-note" dir="rtl">
<p class="markdown-alert-title">Note</p>
<p>‏CI firmwareهای اصلی، headless و 4MB را می‌سازد و imageهای filesystem کامل و کوچک را بررسی می‌کند. محتوای کامل <code>data/</code> فقط در پروفایل 16MB قرار می‌گیرد؛ فایل‌های learned، ضبط‌های CAN و DBCهای کاربر ممکن است در زمان اجرا در SPIFFS باشند، بنابراین پیش از <code>uploadfs</code> دستی حتماً آن‌ها را export و در محل دیگری backup بگیرید.</p>
</div>

**بیلد خودکار:** workflow در `.github/workflows/main.yml` چهار پروفایل را می‌سازد و filesystem، static analysis، تست‌های native، بررسی اندازه‌ی SPIFFS و بررسی‌های DBC/MCP2515/مسیرهای TX را اجرا می‌کند. خروجی نهایی **یک artifact** با نام `cartouch-firmware` است که برای هر پروفایل یک پوشه (به‌همراه `boot_app0.bin` و فایل‌های `.sha256`) دارد؛ پروفایل `headless` عمداً filesystem نمی‌سازد. گزارش کامل در artifact جداگانه‌ی `ci-report` است.

<details>
<summary><b>کتابخانه‌ها (خودکار توسط PlatformIO نصب می‌شوند)</b></summary>

<table dir="rtl">
<tr>
<td align="center"><b>کتابخانه</b></td>
<td align="center"><b>نسخه</b></td>
<td align="center"><b>کاربرد</b></td>
</tr>
<tr><td align="center">TFT_eSPI</td><td align="center">2.5.43</td><td align="center">درایور نمایشگر و تاچ</td></tr>
<tr><td align="center">lvgl</td><td align="center">8.4.0</td><td align="center">رابط گرافیکی</td></tr>
<tr><td align="center">ESPAsyncWebServer</td><td align="center">3.12.1</td><td align="center">وب‌سرور Async + OTA</td></tr>
<tr><td align="center">AsyncTCP</td><td align="center">3.5.0</td><td align="center">TCP Async</td></tr>
<tr><td align="center">ArduinoJson</td><td align="center">7.4.3</td><td align="center">کار با JSON</td></tr>
<tr><td align="center">NimBLE-Arduino</td><td align="center">2.5.1</td><td align="center">BLE و BLE OTA</td></tr>
<tr><td align="center">autowp-mcp2515</td><td align="center">1.3.1</td><td align="center">درایور MCP2515 برای CAN2</td></tr>
</table>

</details>

---

<h2 dir="rtl" id="-اولین-اجرا">🔑 اولین اجرا</h2>

<ol dir="rtl">
<li>دستگاه را روشن کنید و در صورت نیاز، کالیبراسیون لمسی را انجام دهید.</li>
<li>به Access Point دستگاه وصل شوید یا تنظیمات Station را انجام دهید.</li>
<li>آدرس وبی که در Serial Monitor نمایش داده می‌شود را در مرورگر باز کنید.</li>
<li>با حساب مدیریتی وارد شوید.</li>
<li><b>نام کاربری و رمز پیش‌فرض ثابت است:</b> <code>CarTouch</code> / <code>12345678</code> (فقط در <code>src/config.h</code>: <code>WEB_DEFAULT_USER</code> و <code>WEB_DEFAULT_PASS</code>). این مقدار در مخزن عمومی دیده می‌شود و فقط برای استفاده‌ی شخصی/توسعه پذیرفته شده است؛ <b>پیش از اشتراک‌گذاری یا فروش دستگاه آن را عوض کنید.</b> وای‌فای، وب، صفحه و بلوتوث همه از همین یک رمز استفاده می‌کنند و رمز وای‌فای بعد از تغییر رمز و راه‌اندازی مجدد عوض می‌شود. با <code>CT_REQUIRE_PASSWORD_CHANGE=1</code> فقط BLE و OTA بلوتوثی تا تغییر رمز رد می‌شوند و ورود وب مسدود نمی‌شود؛ مقدار فعلی 0 است.</li>
<li>برای آزمایش CAN ابتدا Listen-Only را نگه دارید.</li>
<li>پیش از فعال‌کردن فرمان‌های کنترلی، پروفایل خودرو را انتخاب یا ایجاد کنید و هر فرمان یادگرفته‌شده را جداگانه تأیید کنید.</li>
</ol>

---

<h2 dir="rtl" id="-معماری-و-امنیت">🏗️ معماری و امنیت</h2>

<details open>
<summary><b>جریان نرم‌افزار</b></summary>

```mermaid
flowchart TD
    UI["TFT / Web<br/>تنظیمات · انتخاب خودرو · Learn / Verify · کنترل"] --> APM["ActiveProfileManager<br/>DBC داخلی یا پروفایل سفارشی"]
    APM --> VC["VehicleControl"]
    VC --> CM["CANManager / TWAI"]
    CM --> BUS["CAN Bus"]
    BUS --> CM2["CANManager"]
    CM2 --> OBD["OBD2Reader"]
    CM2 --> LE["LearnEngine"]
    CM2 --> DIAG["UI diagnostics"]
```

جزئیات معماری، قراردادهای بین ماژول‌ها و محدودیت‌های ایمنی در <a href="./CarTouch_SPEC.md"><code>CarTouch_SPEC.md</code></a> نگهداری می‌شود.

</details>

<details open>
<summary><b>امنیت</b></summary>

- Listen-Only باید نقطه‌ی شروع آزمایش CAN باشد.
- فرمان‌های Learn Mode ابتدا به‌صورت تأییدنشده ذخیره می‌شوند و ارسال فرمان آزمایشی فقط از مسیر تأیید صریح انجام می‌شود.
- WebSocket بدون session token معتبر پذیرفته نمی‌شود.
- تغییر رمز، نشست‌های قبلی را بی‌اعتبار می‌کند.
- بدنه‌ی درخواست‌های HTTP پیش از parse سقف دارد؛ واردکردن پروفایل و بارگذاری DBC سقف‌های جداگانه دارند و بدنه‌های با طول نامشخص رد می‌شوند. OTA فایل به‌صورت تکه‌ای نوشته می‌شود و اندازه‌اش را محدودیت پارتیشن کنترل می‌کند.
- OTA فقط از مسیر احراز هویت‌شده در دسترس است.
- درخواست‌های تغییردهنده‌ی (POST/PUT/PATCH/DELETE) که مرورگر از یک وب‌سایت دیگر بفرستد با خطای 403 رد می‌شوند (بررسی هدر Origin در برابر Host)؛ ابزارهای بدون هدر Origin مثل curl همچنان با رمز کار می‌کنند.
- نام (label) فرمان فقط حرف انگلیسی، عدد، فاصله و `_ - .` می‌پذیرد و خروجی‌های وب هنگام نمایش escape می‌شوند.
- Host Guard روی همه‌ی درخواست‌ها و WebSocket upgrade اعمال می‌شود؛ ورود ناموفق مکرر از یک IP موقتاً قفل می‌شود و پاسخ‌های وب هدرهای سخت‌سازی (`nosniff`، `X-Frame-Options`، `no-store`) دارند.
- فرمان سفارشی بدون metadata معتبر actuator قابل اجرا نیست (fail-closed) و تأیید یک پروفایل مشخص، خودروی فعال را تغییر نمی‌دهد.
- OTA از Web و BLE مالک انحصاری دارد و هم‌زمان اجرا نمی‌شود؛ SHA-256 فقط صحت داده را بررسی می‌کند و امضای دیجیتال نیست.
- USB Serial احراز هویت جدا ندارد (دسترسی فیزیکی)، اما فرمان‌های کنترلی همچنان از همان gate مشترک عبور می‌کنند.
- **ریسک‌های پذیرفته‌شده‌ی فعلی:** نبود HTTPS (session و رمز روی شبکه‌ی محلی محرمانگی TLS ندارند؛ Base64 رمزنگاری نیست)، ذخیره‌ی رمز در NVS به‌صورت متن ساده، و غیرفعال‌بودن Secure Boot و Flash Encryption. جزئیات و مرزهای اعتماد: بخش «مدل امنیتی» در [`CarTouch_SPEC.md`](./CarTouch_SPEC.md).

</details>

<details>
<summary><b>DBC</b></summary>

فایل‌های DBC در <code>data/dbc/</code> داده‌ی ورودی سیستم هستند و metadata داخلی خودشان را حفظ می‌کنند. <code>VehicleDB</code> فقط فایل‌هایی را که برای انتخاب مستقیم مناسب تشخیص داده شده‌اند به فهرست خودروها متصل می‌کند؛ فایل‌های دیگر ممکن است برای merge چندمنبعی، ADAS/radar یا ساختارهای خاص نگهداری شده باشند. صفحه‌ی Settings ترجیح مستقل محل DBC، پروفایل سفارشی، ضبط CAN و پشتیبان را نگه می‌دارد و Reset آن‌ها را به Automatic برمی‌گرداند؛ در وضعیت فعلی فقط ذخیره‌ی DBC و ضبط CAN از سیاست SPIFFS/SD استفاده می‌کنند. پروفایل سفارشی در SPIFFS می‌ماند و backup/restore واقعی هنوز پیاده‌سازی نشده است.

</details>

<details>
<summary><b>وضعیت Runtime و ماژول‌ها</b></summary>

وضعیت runtime ماژول‌های Wi-Fi، Web Server، CAN Bus، OBD-II، Touch، Display، BLE و Storage روی TFT و Web UI نمایش داده می‌شود. وضعیت‌ها شامل <code>DETECTED</code>، <code>INITIALIZING</code>، <code>READY</code>، <code>NOT DETECTED</code>، <code>ERROR</code> و <code>DISABLED</code> هستند. CAN diagnostics شامل شمارنده‌های RX/TX، خطا و وضعیت bus است. قطع یک ماژول نباید boot کل دستگاه را متوقف کند.

</details>

<details>
<summary><b>اندازه‌گیری Heap و Stack</b></summary>

از USB Serial با سرعت 115200، فرمان <code>memory</code> را برای مشاهده‌ی heap آزاد فعلی/کمینه‌ی زمان boot، بزرگ‌ترین بلوک قابل تخصیص، وضعیت PSRAM و high-water کمینه‌ی stack وظیفه‌ی اصلی <code>loop</code> اجرا کنید. برای سنجش، دستگاه را reboot کنید تا مقدارهای کمینه از boot تازه شروع شوند؛ سپس workloadهای سنگین و هم‌زمان (وب، ثبت CAN روی هر دو باس، انتخاب/بارگذاری DBC و BLE) را اجرا و خروجی را در طول آزمون ثبت کنید. این فرمان stack taskهای مستقل AsyncTCP/BLE را اندازه نمی‌گیرد و خروجی اجرای سخت‌افزاری باید جداگانه ثبت شود؛ مقدارهای build یا شبیه‌سازی جایگزین اندازه‌گیری روی برد نیستند.

</details>

<details>
<summary><b>OTA و BLE</b></summary>

OTA برای firmware و filesystem از Web UI فعال است و برای هر تصویر، SHA-256 مورد انتظار را از artifact همان build می‌گیرد؛ فایل‌های `.sha256` همراه firmware در خروجی CI هستند. BLE مستقل از Wi-Fi اجرا می‌شود و BLE OTA فقط پس از تطبیق اندازه و SHA-256 فعال می‌شود؛ قالب قدیمی `START` بدون هش پذیرفته نمی‌شود. Firmware روی slot غیرفعال نوشته می‌شود، اما rollback سلامت پس از اولین boot پیاده نشده؛ filesystem یک پارتیشن دارد و بازیابی خودکار در قطع برق ندارد. SHA-256 امضای دیجیتال نیست و ارتباط وب HTTPS ندارد. نسخه‌ی firmware از <code>CAR_TOUCH_FIRMWARE_VERSION</code> در <code>src/config.h</code> می‌آید. Web و BLE نمی‌توانند هم‌زمان مالک یک OTA باشند. جزئیات در <a href="./BLE_OTA.md"><code>BLE_OTA.md</code></a> و بخش «OTA و filesystem» مشخصات فنی.

</details>

---

<h2 dir="rtl" id="-ساختار-پروژه">📂 ساختار پروژه</h2>

<div dir="ltr">

```
CarTouch/
├── platformio.ini                # تنظیمات PlatformIO، پین‌ها و کتابخانه‌ها
├── cartouch_16MB.csv             # جدول پارتیشن ۱۶ مگابایتی
├── cartouch_4MB.csv              # جدول پارتیشن ۴ مگابایتی
├── boards/                       # تعریف بردهای N16R8 و 4MB
├── scripts/                      # چک‌های CI و اسکریپت filesystem 4MB
├── README.md                     # همین فایل
├── CarTouch_SPEC.md              # مشخصات فنی، معماری، حافظه، OTA، فلش و مدل امنیتی
├── BLE_OTA.md                    # پروتکل BLE و BLE OTA
├── DBC_AUDIT.md                  # گزارش خودکار DBC (خروجی scripts/audit_dbc.py)
├── THIRD_PARTY_NOTICES.md        # مجوزها و منشأ وابستگی‌ها/داده‌ها
├── LICENSE
├── .github/workflows/            # بیلد خودکار در GitHub Actions
├── src/
│   ├── main.cpp                  # setup + loop
│   ├── config.cpp/.h             # پین‌ها، ثابت‌ها، تنظیمات
│   ├── can_manager.cpp/.h        # مدیریت CAN1 (TWAI)
│   ├── can_service.cpp/.h        # مسیریابی بین CAN1 و CAN2
│   ├── mcp2515_can_interface.*   # درایور CAN2 (MCP2515)
│   ├── can_interface.h           # رابط مشترک باس‌ها
│   ├── can_recorder.*            # ضبط CAN (CSV)
│   ├── obd2_reader.cpp/.h        # خواندن OBD-II (غیرمسدودکننده)
│   ├── vehicle_control.cpp/.h    # اجرای فرمان‌ها
│   ├── vehicle_db.cpp/.h         # پارسر DBC (Intel + Motorola)
│   ├── dbc_store.*               # ذخیره و مدیریت DBCهای کاربر
│   ├── tft_ui.cpp/.h             # رابط لمسی LVGL
│   ├── buttons.*                 # ورودی Five-way (اختیاری)
│   ├── webserver.cpp/.h          # وب‌سرور + WebSocket + OTA
│   ├── wifi_manager.cpp/.h       # Wi-Fi (AP/STA)
│   ├── ble_manager.cpp/.h        # BLE و BLE OTA
│   ├── sd_storage.*              # ذخیره‌سازی SD (پیش‌فرض غیرفعال)
│   ├── module_status.cpp/.h      # وضعیت runtime ماژول‌ها
│   ├── custom_vehicle.h          # ساختار پروفایل‌های سفارشی
│   ├── custom_vehicle_store.*    # ذخیره JSON در SPIFFS
│   ├── learn_engine.*            # موتور یادگیری (capture/diff)
│   ├── active_profile_manager.*  # یکپارچه‌ساز DBC + سفارشی
│   ├── error_log.*               # لاگ خطا و شمارنده‌های پایدار
│   └── ct_*.h / ct_*.cpp         # منطق مستقل از سخت‌افزار (guard، parser، validation، OTA lock، SHA-256)
├── test/test_native/             # تست‌های native (Unity)
└── data/                         # محتوای SPIFFS
    ├── index.html, style.css, app.js   # Web Dashboard
    ├── dbc/                            # فایل‌های DBC (57 فایل + manifest)
    └── custom_vehicles/                # در زمان اجرا ساخته می‌شود (نه در سورس)
```

</div>

---

<h2 dir="rtl" id="-مستندات-بیشتر">📚 مستندات بیشتر</h2>

<table dir="rtl">
<tr>
<td align="center"><b>فایل</b></td>
<td align="center"><b>چه چیزی در آن هست</b></td>
</tr>
<tr>
<td align="center"><a href="./CarTouch_SPEC.md"><code>CarTouch_SPEC.md</code></a></td>
<td align="center">معماری ماژول‌ها، Learn Mode و verification، Listen-Only، DBC، حافظه (فرمان <code>memory</code>)، OTA و filesystem، نقشه‌ی فلش، مدل امنیتی و قراردادهای ایمنی و تست</td>
</tr>
<tr>
<td align="center"><a href="./BLE_OTA.md"><code>BLE_OTA.md</code></a></td>
<td align="center">پروتکل BLE و BLE OTA و نکات امنیتی آن</td>
</tr>
<tr>
<td align="center"><a href="./DBC_AUDIT.md"><code>DBC_AUDIT.md</code></a></td>
<td align="center">گزارش خودکار فایل‌های DBC (تولید با <code>scripts/audit_dbc.py</code>؛ دستی ویرایش نکنید)</td>
</tr>
<tr>
<td align="center"><a href="./THIRD_PARTY_NOTICES.md"><code>THIRD_PARTY_NOTICES.md</code></a></td>
<td align="center">نسخه، منبع و مجوز وابستگی‌ها و وضعیت منشأ/مجوز DBCها</td>
</tr>
</table>

**مجوز:** اختصاصی (همه‌ی حقوق محفوظ) — متن کامل در `LICENSE`.

</div>
