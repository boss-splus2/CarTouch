# CarTouch — مشخصات فنی

این سند مرجع فنی پروژه است. اگر رفتاری در این سند با کد واقعی مغایر بود، کد و تست قابل بازتولید باید بررسی و این سند پس از تعیین رفتار واقعی به‌روزرسانی شود.

## 1. هدف

CarTouch یک firmware برای ESP32-S3 است که از CAN Bus خودرو داده دریافت می‌کند و در شرایط تأییدشده می‌تواند فرمان‌های کنترلی را ارسال کند. Web و BLE مستقل از TFT هستند؛ نمایشگر و touch در build headless غیرفعال می‌شوند. ورودی اختیاری کلید پنج‌جهته و کارت SD پیاده‌سازی پایه دارند؛ یکپارچگی کامل کلیدها با منوها و استفاده از SD برای پروفایل‌ها/پشتیبان هنوز موجود نیست.

1. TFT لمسی با LVGL در build نمایش‌دار.
2. Web Dashboard روی Wi‑Fi.
3. BLE برای status و BLE OTA.

طراحی باید در برابر ارسال ناخواسته‌ی CAN محافظه‌کار باشد.

## 2. اجزای اصلی

### `main.cpp`

نقطه‌ی ورود سیستم است و lifecycle ماژول‌ها را در `setup()` و اجرای دوره‌ای آن‌ها را در `loop()` هماهنگ می‌کند.

وظایف اصلی:

- نگهداری objectهای اصلی به‌صورت static؛ PSRAM در صورت وجود برای تخصیص‌های بزرگ در دسترس است.
- راه‌اندازی watchdog.
- بارگذاری configuration.
- راه‌اندازی SPIFFS.
- راه‌اندازی CAN.
- راه‌اندازی OBD-II.
- راه‌اندازی VehicleDB و پروفایل‌های سفارشی.
- اتصال Learn Mode و ActiveProfileManager به UI و Web.
- اجرای LVGL/WebSocket/Learn/OBD در حلقه‌ی اصلی.
- مدیریت sleep/wake؛ پس از بیدار شدن از طریق CAN، ارسال درخواست‌های OBD برای ۱ ثانیه متوقف می‌شود تا wake event باعث TX فوری ناخواسته نشود.

`loop()` نباید روی عملیات شبکه یا OBD به‌صورت طولانی مسدود شود.

### `config.cpp/.h`

منبع مرکزی:

- پین‌ها
- CAN speed
- OBD settings
- TFT/LVGL settings
- Wi‑Fi/web settings
- power-management settings
- vehicle selection
- Learn Mode settings
- credential/session helpers

Configuration پایدار در NVS نگهداری می‌شود.

### `can_manager.cpp/.h`

لایه‌ی انتزاع TWAI:

- نصب و شروع driver
- stop/uninstall
- تغییر Listen-Only/Normal
- ارسال
- دریافت blocking در APIهای مخصوص
- دریافت non-blocking برای loop/Learn Mode
- flush صف
- status/diagnostics

تغییر mode باید lifecycle واقعی driver را رعایت کند؛ تغییر یک flag در برنامه به‌تنهایی enforcement سخت‌افزاری Listen-Only نیست.

### `vehicle_db.cpp/.h`

مسئول:

- فهرست پروفایل‌های built-in
- بارگذاری فایل DBC
- parse پیام‌ها (`BO_`) و سیگنال‌ها (`SG_`)؛ `CM_` و `VAL_` تفسیر نمی‌شوند
- پیدا کردن پیام/سیگنال
- استخراج و encode مقدار سیگنال
- نگهداری سقف تعداد پیام‌ها

هر فایل DBC یک منبع مستقل است؛ merge خودکار چند DBC در یک profile در معماری پروژه پشتیبانی نمی‌شود.
فهرست خودرو در زمان اجرا فقط profileهایی را نشان می‌دهد که فایل DBC آن‌ها در SPIFFS حاضر است؛ DBCها هنگام انتخاب بارگذاری می‌شوند، نه همگی در RAM.

### `obd2_reader.cpp/.h`

دو سطح API دارد:

- APIهای خواندن مستقیم PID که می‌توانند blocking باشند و فقط برای مسیرهای مشخص استفاده می‌شوند.
- state machine غیرمسدودکننده برای استفاده‌ی اصلی firmware.

مسیر اصلی `loop()` باید از `update()` و `getLatestData()` استفاده کند.

### `vehicle_control.cpp/.h`

لایه‌ی اجرای فرمان:

- دریافت label فرمان
- resolve کردن فرمان از profile فعال
- محدودیت زمانی بین فرمان‌ها
- محدودیت duty-cycle actuatorهای مکانیکی
- مسیر verification
- wrapperهای فرمان‌های شناخته‌شده
- گزارش خطا

فرمان نباید از یک CAN ID حدسی ساخته شود وقتی profile فعال اطلاعات دقیق‌تری در اختیار دارد.

### `custom_vehicle.h` و `custom_vehicle_store.cpp/.h`

مدل و persistence پروفایل‌های سفارشی:

- اطلاعات خودرو
- فرمان‌های learned/manual
- وضعیت verification
- منبع فرمان
- serialization به JSON
- index
- import/export
- ایجاد/ویرایش/حذف

فایل‌های JSON پروفایل فعلاً در SPIFFS نگهداری می‌شوند. مسیر DBC کاربر و ضبط‌کننده‌ی CAN می‌توانند بر اساس ترجیح ذخیره‌سازی از SPIFFS یا SD استفاده کنند؛ برای ذخیره‌ی پروفایل‌ها روی SD و backup/restore واقعی هنوز مسیر پیاده‌سازی‌شده‌ای وجود ندارد. SD هرگز خودکار format نمی‌شود و در نبود کارت، منطق ذخیره‌سازی می‌تواند به رسانه‌ی دیگر برگردد. اگر SPIFFS mount نشود، core بدون پاک‌کردن خودکار داده ادامه می‌دهد، اما قابلیت‌های وابسته به فایل‌سیستم داخلی در دسترس نیستند.

### `learn_engine.cpp/.h`

state machine یادگیری:

1. شروع session.
2. اطمینان از Listen-Only سخت‌افزاری.
3. capture baseline.
4. capture action.
5. استخراج candidateها.
6. ranking.
7. آماده‌سازی برای verification.
8. cancel و بازگرداندن mode قبلی.

در هیچ مرحله‌ای قبل از تأیید صریح کاربر نباید فرمان کنترل واقعی از Learn Mode ارسال شود.

### `active_profile_manager.cpp/.h`

منبع فعال فرمان را یکپارچه می‌کند:

- هیچ profile انتخاب نشده
- built-in DBC
- custom profile

VehicleControl باید از این manager برای resolve فرمان استفاده کند.

### `tft_ui.cpp/.h`

مسئول:

- initialization نمایشگر و touch
- calibration
- Control
- Dashboard
- Learn
- Settings
- Manual Entry
- Verify
- password UI
- status indicators
- theme
- notifications
- power state

پروفایل نمایش‌دار فعلی به ILI9341/XPT2046 و پین‌های ثابت build متکی است؛ driver عمومی برای کنترلرها/رزولوشن‌های دیگر و پیکربندی runtime این پین‌ها وجود ندارد. در build headless هیچ نمایشگر یا touchی initialize نمی‌شود.

منطق CAN و business logic نباید در callbackهای UI تکثیر شود.

### `webserver.cpp/.h`

مسئول:

- HTTP
- authentication
- session token
- WebSocket
- custom profile REST API
- Learn Mode messages
- control/status API
- OTA
- broadcasting data/status

تمام endpointهای حساس باید قبل از عملیات state-changing احراز هویت و authorization لازم را بررسی کنند.

### `wifi_manager.cpp/.h`

مدیریت AP/STA، اتصال، قطع، scan و status شبکه.

### `ble_manager.cpp/.h`

BLE مستقل از Wi‑Fi:

- Status (read/notify)، Command و Data با نوشتن فقط روی لینک رمزنگاری‌شده
- BLE OTA با رمز فعلی وب در لایه‌ی برنامه؛ اگر `CT_REQUIRE_PASSWORD_CHANGE` برابر 1 باشد، OTA و فرمان‌های BLE با رمز پیش‌فرض رد می‌شوند (حالت فعلی: 0)
- قفل ۶۰ ثانیه‌ای پس از ۵ رمز اشتباه
- Pairing از نوع Just Works است (بدون حفاظت MITM)؛ جزئیات در `BLE_OTA.md`

### `module_status.cpp/.h`

وضعیت runtime ماژول‌های Wi-Fi، Web Server، CAN، OBD-II، Touch، Display، BLE و Storage را برای TFT و Web نگهداری می‌کند. چرخه‌ی وضعیت‌ها `DETECTED`، `INITIALIZING`، `READY`، `NOT_PRESENT`، `ERROR` و `DISABLED` است؛ این وضعیت runtime به‌تنهایی تشخیص الکتریکی حضور سخت‌افزار را تضمین نمی‌کند.

### `error_log.cpp/.h`

- ring buffer رویدادها
- category/severity
- شمارنده‌های persistent
- serialization JSON
- ثبت رویدادهای subsystemها

## 3. جریان داده

### دریافت CAN

```text
CAN transceiver
      │
      ▼
TWAI
      │
      ▼
CANManager
   ┌──┴───────────────┐
   │                  │
   ▼                  ▼
OBD2Reader        LearnEngine
   │                  │
   └───────┬──────────┘
           ▼
     UI / Web / Logs
```

### ارسال فرمان

```text
TFT/Web
   │
   ▼
authentication / UI rules
   │
   ▼
ActiveProfileManager
   │
   ▼
VehicleControl
   │
   ├── rate limit
   ├── duty-cycle
   ├── verification state
   └── Listen-Only guard
   │
   ▼
CANManager
   │
   ▼
TWAI → CAN transceiver
```

## 4. Learn Mode و verification

WebSocket callbacks اجراهای ناهمگام دارند؛ دسترسی آن‌ها به state ماشین Learn با mutex سریال می‌شود. UI و API برای نمایش/ذخیره از snapshot یکپارچه استفاده می‌کنند تا candidate، label و session ID از یک session باشند. ذخیره‌ی موفق فقط همان session را لغو می‌کند؛ شروع session دیگری هم‌زمان با عملیات ذخیره نباید آن را لغو کند.

### baseline

در ابتدای session، پیام‌های موجود روی bus در Listen-Only ثبت می‌شوند تا تغییرات ناشی از فشردن دکمه با background traffic مقایسه شود.

### action capture

پس از آماده‌شدن baseline، پیام‌های جدید/تغییریافته ثبت و برای candidate generation استفاده می‌شوند.

### verification

هر candidate با وضعیت تأییدنشده ذخیره می‌شود. UI باید داده‌ی لازم برای بررسی انسانی، از جمله CAN ID و payload مربوط، را نشان دهد.

تنها مسیر verification مجاز است که یک فرمان unverified را برای آزمایش ارسال کند؛ این ارسال باید صریحاً توسط کاربر آغاز شود.

پس از تأیید:

```text
UNVERIFIED → VERIFIED
```

و فقط فرمان verified می‌تواند در مسیر عادی control فعال شود.

## 5. Listen-Only

Listen-Only باید در دو سطح رعایت شود:

1. **سخت‌افزار/TWAI:** driver در mode دریافت-only واقعی قرار گیرد.
2. **منطق برنامه:** مسیرهای ارسال فرمان و OBD request نیز هنگام فعال بودن Listen-Only مسدود باشند.

هیچ‌کدام جای دیگری را به‌تنهایی کافی نمی‌کند.

## 6. OBD-II

OBD polling اصلی باید غیرمسدودکننده باشد.

هر چرخه‌ی polling فقط مقدار محدودی کار انجام می‌دهد و state machine به فراخوانی بعدی `update()` ادامه می‌یابد.

در آدرس‌دهی 11-bit، درخواست functional به `0x7DF` ارسال می‌شود و پاسخ‌های استاندارد از `0x7E8` تا `0x7EF` پذیرفته می‌شوند. PID polling مسیر Single Frame دارد؛ Mode 03 برای خواندن DTC علاوه بر Single Frame، First/Consecutive Frame را با بررسی sequence و ارسال Flow Control به همان ECU پشتیبانی می‌کند. این reassembly محدود به حداکثر `MAX_DTC_COUNT` کد است؛ پیاده‌سازی عمومی ISO-TP برای سرویس‌های دلخواه، همه‌ی block-size/separation-time modes و multi-frame در polling معمولی هنوز وجود ندارد. کانال OBD از Web Settings انتخاب می‌شود؛ درخواست، Flow Control و دریافت فقط از همان کانال انجام می‌شوند و در صورت unavailable یا Listen-Only بودن آن، ارسال صورت نمی‌گیرد.

PIDهای استاندارد باید با طول پاسخ و پشتیبانی ECU بررسی شوند و در نبود داده‌ی معتبر مقدار ساختگی تولید نشود.

ولتاژ نمایش‌داده‌شده از OBD PID `0x42` است، نه ADC مستقیم باتری. فقط مقادیر ۶ تا ۳۶ ولت معتبرند؛ timeout، شکست درخواست یا مقدار خارج از محدوده UI را به `N/A` می‌برد. اندازه‌گیری مستقل ولتاژ/درصد شارژ نیازمند ورودی سنسور و کالیبراسیون سخت‌افزاری است و پشتیبانی نمی‌شود.

خواندن DTC و پاک‌کردن DTC عملیات حساس هستند و باید جدا از polling عادی و با authorization مناسب انجام شوند.

## 7. DBC

parser:

- پیام‌های `BO_`
- سیگنال‌های `SG_` با indentationهای متداول DBC

را در حد grammar مورد استفاده‌ی پروژه پردازش می‌کند و سیگنال‌های هر پیام را به‌صورت پویا نگهداری می‌کند. `CM_` و `VAL_` تفسیر نمی‌شوند و parser آن‌ها را به‌عنوان داده‌ی قابل استفاده ثبت نمی‌کند.

parser تا `MAX_DBC_MESSAGES` پیام در هر فایل را نگه می‌دارد و سیگنال‌های هر پیام را به‌صورت پویا ذخیره می‌کند. سقف برای پروفایل‌های مستقیم منوی خودرو 400 پیام است؛ فایل‌های بزرگ‌تر یا چندمنبعی همچنان نیازمند معماری چندفایلی/ذخیره‌سازی گسترده‌تر هستند.

Intel و Motorola باید با mapping بیت صحیح پردازش شوند. تغییر در این قسمت بدون test vector خطرناک است.

برای CAN ID، parser شناسه‌های استاندارد 11 بیتی را بدون تغییر نگه می‌دارد و شناسه‌های Extended را به arbitration ID 29 بیتی به‌همراه پرچم `isExtended` تبدیل می‌کند. DBC استاندارد معمولاً Extended را با bit 31 مشخص می‌کند؛ برخی فایل‌های vendor/OpenDBC شناسه‌ی 29 بیتی را بدون این marker ذخیره می‌کنند و parser آن‌ها را نیز به‌عنوان Extended تشخیص می‌دهد. شناسه‌ی pseudo-message با نام `VECTOR__INDEPENDENT_SIG_MSG` روی CAN بارگذاری نمی‌شود.

## 8. حافظه

`VehicleDB` و objectهای وابسته‌ی بزرگ روی heap ایجاد می‌شوند تا `.bss` داخلی بیش از حد بزرگ نشود.

هر تغییر در:

- تعداد messageها
- تعداد signalها
- اندازه‌ی profileها
- bufferهای UI
- WebSocket client table
- DBC parser

باید با اندازه‌گیری heap/PSRAM بررسی شود.

هیچ فرضی درباره‌ی PSRAM نباید جای check واقعی allocation را بگیرد.

## 9. Web و احراز هویت

سطح حمله‌ی اصلی:

- login
- session token
- WebSocket
- profile import
- command execution
- OTA

است.

الزامات:

- endpointهای حساس بدون authentication قابل استفاده نباشند.
- session token تاریخ مصرف/اعتبار داشته باشد.
- تغییر password نشست‌های قبلی را invalidate کند.
- داده‌ی ورودی JSON با اندازه و ساختار محدود پردازش شود.
- import profile نباید باعث overflow، path traversal یا مصرف بی‌نهایت حافظه شود.
- OTA بدون authentication ممنوع باشد.
- پیام WebSocket قبل از dispatch اعتبارسنجی شود.

HTTPS پشتیبانی نمی‌شود و مستندات نباید خلاف آن ادعا کنند.

## 10. OTA و filesystem

پروفایل اصلی N16R8 از `cartouch_16MB.csv` (16 MB flash و 8 MB OPI PSRAM) استفاده می‌کند. پروفایل‌های `esp32-s3-4mb` و `esp32-s3-4mb-psram` از `cartouch_4MB.csv` استفاده می‌کنند؛ دومی برای PSRAM نوع QSPI است.

در buildهای 4 MB فقط Web UI و 10 فایل DBC منتخب regional/imported در image قرار می‌گیرند. منبع کامل DBC در `data/dbc/` باقی می‌ماند و فقط در build 16 MB بسته‌بندی می‌شود. اندازه‌ی firmware فعلی به سقف 1.5 MB هر OTA slot در جدول 4 MB نزدیک است و حاشیه‌ی تغییرات آینده محدود است.

Partition table باید فضای کافی برای:

- NVS
- OTA metadata
- دو app partition
- SPIFFS

فراهم کند.

**محدودیت طول مسیر SPIFFS:** هر مسیر (شامل `/` ابتدایی) حداکثر ۳۱ نویسه است. این محدودیت برای فایل‌های `data/` (در CI بررسی می‌شود) و برای فایل‌های زمان اجرا نیز صادق است؛ به‌ویژه فایل‌های journal با پسوند `.tmp` و `.bak` که به نام فایل اصلی اضافه می‌شوند. پروفایل‌های سفارشی به‌صورت `/custom_vehicles/pN.json` ذخیره می‌شوند و این طول با `static_assert` در `custom_vehicle_store.cpp` در زمان کامپایل کنترل می‌شود.

در صورت شکست mount، firmware بدون format خودکار boot را ادامه می‌دهد، خطا را ثبت و Storage را `ERROR` اعلام می‌کند؛ عملیات پروفایل سفارشی fail-closed می‌شوند تا از پاک‌شدن داده برای بازیابی موقت جلوگیری شود.

خطای NVS هنگام initialization باعث erase خودکار نمی‌شود: دستگاه با defaultهای RAM بالا می‌آید و تنظیمات تا بازیابی NVS قابل ذخیره نیستند. این رفتار از پاک‌شدن خاموش تنظیمات جلوگیری می‌کند.

CI image کامل و کوچک SPIFFS را می‌سازد و اندازه‌ی خروجی هرکدام را با پارتیشن مربوط مقایسه می‌کند.

## 11. CI و کیفیت

Workflow فعلی این مراحل را اجرا می‌کند:

1. checkout
2. نصب PlatformIO
3. buildهای N16R8، headless، 4 MB بدون PSRAM و 4 MB با QSPI PSRAM
4. filesystem کامل و هر دو filesystem کوچک 4 MB
5. static analysis
6. اجرای native unit tests با `pio test -e native`
7. بررسی اندازه‌ی `spiffs.bin` در برابر پارتیشن واقعی
8. artifact upload

هر خطای build باید باعث شکست workflow شود.

## 12. قراردادهای ایمنی

هر تغییر کدی که می‌تواند مسیر CAN TX را تغییر دهد باید این موارد را بررسی کند:

- آیا Listen-Only واقعاً جلوی ارسال را می‌گیرد؟
- آیا command verification bypass نشده؟
- آیا rate limit باقی مانده؟
- آیا duty-cycle باقی مانده؟
- آیا profile فعال همان profile مورد انتظار است؟
- آیا payload از داده‌ی معتبر profile آمده؟
- آیا ورودی UI/Web می‌تواند مستقیماً به CAN frame تبدیل شود؟

هیچ refactor زیبایی‌شناختی نباید این guardها را حذف یا دور بزند.

## 13. تست‌های خودکار و محدودیت اعتبارسنجی

تست native منطق مستقل از سخت‌افزار را پوشش می‌دهد، از جمله:

- Listen-Only و CAN TX admission guard
- timerهای wrap-safe
- parser مشترک hex برای Web/TFT
- OBD Single-Frame PCI/DLC/service/PID validation و ISO-TP DTC reassembly/sequence
- پذیرش محدوده‌ی پاسخ OBD با آدرس‌دهی 11-bit و frameهای استاندارد
- DTC pair-length validation
- DBC signal/DLC boundary validation برای Intel و Motorola
- verification transaction و rejectionهای profile/label/token/timeout
- verification fingerprint برای تغییر command/profile revision
- validation سخت فیلدهای JSON import

موارد زیر در CI (GitHub Actions) اجرا می‌شوند و نتیجه‌ی همان اجرا معیار است: build کامل firmware، filesystem image و اندازه‌ی آن، static analysis. موارد زیر نیازمند اجرای محیط/سخت‌افزار مناسب هستند و نباید بدون اجرای واقعی به‌عنوان pass گزارش شوند:

- Learn baseline/action روی CAN واقعی
- duty-cycle و rate limiter روی firmware واقعی
- session/login behavior در runtime WebSocket/HTTP
- bench/vehicle validation

تست روی میز باید پیش از اتصال به CAN زنده انجام شود.

## 14. قراردادهای یکپارچگی runtime

- مسیر فایل‌های DBC داخلی با فایل‌های موجود در `data/dbc/` یکسان است؛ profile فاقد فایل در image کوچک از فهرست runtime حذف می‌شود.
- انتخاب پین CAN در زمان اجرا، پین‌های ثابت پروژه و محدوده‌ی GPIO مربوط به Octal Flash/PSRAM ماژول ESP32-S3 N16R8 را پیش از نصب TWAI رد می‌کند.
- وضعیت ماژول‌های Wi-Fi، Web Server، CAN، OBD-II، Touch، Display، BLE، Storage، CAN1، CAN2، PSRAM، SD Card و Buttons روی TFT و از طریق Web API/WebSocket احراز‌شده در دسترس است. نبود PSRAM خطا نیست و «وصل نیست» گزارش می‌شود. وضعیت SD بر اساس mount/دسترسی فایل‌سیستم است؛ وضعیت کلید خاموش در حالت پیکربندی‌نشده و تا پیش از مشاهده‌ی فشار واقعی `UNVERIFIED` است. این وضعیت‌ها تشخیص الکتریکی قطعه را تضمین نمی‌کنند و SD/کلیدها هنوز نیازمند آزمون روی دستگاه‌اند. درایور کلید اختیاری، پیکربندی NVS برای GPIO/ADC و صف رویدادهای debounce/کوتاه/بلند دارد؛ `loop()` رویدادها را به keypad ورودی LVGL می‌دهد. پیکربندی کلید از Serial پشتیبانی می‌شود ولی از Web/BLE در دسترس نیست. CS کارت SD از Serial/Web/BLE قابل تنظیم است، اما تعارض آن با پین کلیدِ از پیش ذخیره‌شده هنوز به‌صورت دوطرفه بررسی نمی‌شود. اگر پنل تاچ در ویزارد کالیبراسیون پاسخ ندهد، پرچم جداگانه `touch_skip` در NVS ذخیره می‌شود (ساختار blob تنظیمات تغییر نمی‌کند) تا بوت‌های بعدی منتظر تاچ نمانند؛ Settings > Recalibrate Touch دوباره تلاش می‌کند و با موفقیت پرچم را پاک می‌کند.
- شمارنده‌های CAN diagnostics و وضعیت bus هر ثانیه برای کلاینت‌های WebSocket احراز‌شده broadcast می‌شود.
- وضعیت «متصل» برای CAN، CAN1 و CAN2 فقط با دریافت واقعی فریم در ۵ ثانیه‌ی اخیر `READY` می‌شود (`ctCanLinkState`). درایور روشن‌شده ولی بدون ترافیک `UNVERIFIED` است (نمی‌توان فرق «خودروی خاموش» و «سیم وصل نیست» را نرم‌افزاری تشخیص داد) و Bus-Off یا درایور متوقف `ERROR` است.
- بازیابی Bus-Off برای هر دو کانال در حلقه اصلی بررسی می‌شود (اولین تلاش فوری، سپس حداکثر هر ۵ ثانیه یک بار). پیش‌تر CAN1 فقط پس از شکست یک ارسال بازیابی می‌شد.
- `scripts/check_tx_paths.py` در CI تضمین می‌کند فقط درایورها، `can_service.cpp`، `obd2_reader.cpp` و `vehicle_control.cpp` تابع `sendMessage()` را صدا بزنند؛ Learn، ضبط‌کننده، وب‌سرور و رابط کاربری هرگز مستقیم ارسال نمی‌کنند.
- کنسول Serial در `loop()` به‌صورت non-blocking خط‌خوانی می‌کند؛ `help`, `status`, `config`, `can`, `obd`, `learn`, `storage`, `errors`, فرمان‌های `sd ...` و `btn ...` و `control <command>` دارد. فرمان کنترل از `handleCommand()` و guardهای اصلی همانند Web/TFT عبور می‌کند. تنظیمات runtime عمومی CAN از Web تغییر می‌کنند؛ تنظیمات ذخیره‌سازی و کلیدها از Serial نیز قابل‌تغییرند.
- جایگزینی JSON پروفایل سفارشی با فایل‌های journal موقت/پشتیبان انجام می‌شود و هنگام راه‌اندازی بازیابی دارد.
- متن رابط وب برای اپراتور فقط انگلیسی است.
- نسخه‌ی firmware فقط یک منبع دارد: `CAR_TOUCH_FIRMWARE_VERSION`.

## 15. پیکربندی CAN در زمان اجرا

صفحه‌ی Settings در Web UI (پس از احراز هویت) پین‌های TX/RX، bitrate و حالت Listen-Only را در NVS ذخیره می‌کند. پس از ذخیره‌ی موفق دستگاه reboot می‌شود تا درایور TWAI فقط یک بار و با پیکربندی اعتبارسنجی‌شده نصب شود. اعتبارسنجی GPIO پین‌های CAN، پین‌های ثابت پروژه، پین‌های Octal Flash/PSRAM در N16R8 و سایر پین‌های رزروشده یا حساس به strapping را رد می‌کند. تنظیم runtime CAN از Web است؛ GPIO نمایشگر و touch runtime قابل تنظیم نیستند. پین‌های کلید از طریق فرمان‌های Serial قابل تنظیم‌اند و پیکربندی آن‌ها از Web/BLE موجود نیست. bitrateهای classic CAN پشتیبانی‌شده: 100، 125، 250، 500، 800 و 1000 kbps.

## 16. Dual CAN

- `CANService` فراخوانی‌های بدون کانال مشخص را برای سازگاری به CAN1/TWAI هدایت می‌کند و انتخاب صریح CAN1 یا CAN2 را فراهم می‌کند؛ شکست هر backend مستقل از دیگری است. مسیرهای OBD و Learn کانال انتخاب‌شده را به‌صورت صریح به‌کار می‌گیرند. در `loop()` یک pump مشترک حداکثر ۱۶ فریم از هر باس را در هر دور دریافت و به صف‌های مستقل OBD، Learn، Monitor و wake fan-out می‌کند؛ عمق هر صف ۳۲ فریم است و هنگام پرشدن صف، فریم جدید drop و شمارنده‌ی drop همان مصرف‌کننده افزایش می‌یابد.
- CAN2 از MCP2515 با نوسان‌ساز 8 MHz، ترنسیور TJA1050 و SPI مشترک با TFT/Touch (SCLK 12، MOSI 11، MISO 13) استفاده می‌کند. CS پیش‌فرض GPIO15 و INT پیش‌فرض GPIO16 است. هر باس bitrate، Listen-Only، شمارنده‌ها، وضعیت، diagnostics و bus-off recovery مستقل دارد.
- CAN2 به‌صورت پیش‌فرض Listen-Only است. کانال OBD و Learn Mode جداگانه در Web Settings انتخاب می‌شود؛ انتخاب CAN2 در OBD فقط در صورت Normal بودن mode خود CAN2 امکان TX دارد. Learn، hardware Listen-Only را فقط روی کانال انتخاب‌شده اعمال و هنگام لغو تلاش می‌کند mode پیشین همان کانال را بازگرداند. wake detection همچنان روی CAN1 است و از صف مستقل مصرف‌کننده دریافت می‌کند؛ کانال فرمان خودرو (`vehicleCanBus`، پیش‌فرض CAN1) از Web Settings انتخاب می‌شود؛ فرمان فقط روی همان کانال و بدون fallback ارسال می‌شود و اگر تنظیم یا mode واقعی درایور همان کانال Listen-Only باشد، رد می‌شود (`ctVehicleTxGuard`).
- Settings احراز‌شده‌ی Web هر دو رابط و انتخاب کانال OBD/Learn/فرمان خودرو را در NVS ذخیره می‌کند، تداخل GPIO را بررسی می‌کند و پیش از اعمال تغییر reboot درخواست می‌کند. فیلدهای جدید به انتهای blob در NVS اضافه شده‌اند و اندازه‌های قدیمی config به انتخاب CAN1 برای OBD و Learn مهاجرت می‌کنند. `vehicleCanBus` در padding انتهایی ذخیره می‌شود و اندازه blob (244 بایت) تغییر نمی‌کند؛ مقدار نامعتبر به CAN1 برمی‌گردد بدون پاک‌شدن بقیه تنظیمات. نام‌های قدیمی `can1*` در NVS و API برای سازگاری حفظ شده‌اند.
- درایور MCP2515 با نوسان‌ساز 8 MHz فقط bitrateهای 100، 125، 250، 500 و 1000 kbps را می‌پذیرد. Web CAN Monitor روی WebSocket احراز‌شده پیاده‌سازی شده؛ خروجی حداکثر ۸ فریم در هر ۵۰ ms و حداکثر ۱۰۰ خط در مرورگر است و تعداد drop صف مانیتور در پیام‌ها گزارش می‌شود. CAN Recorder (`src/can_recorder.cpp`؛ ضبط شنودی روی CAN1، CAN2 یا هر دو، شروع/توقف از Serial و وب، تا ۱۰۰ فایل CSV و حداکثر 256 KiB برای هر فایل، با فهرست، دانلود و حذف) با سیاست ذخیره‌سازی `rec` روی SPIFFS یا SD می‌نویسد؛ مسیر SD نیازمند تست سخت‌افزار است. Replay و فیلتر سخت‌افزاری per-ID پیاده‌سازی نشده‌اند و بازپخش خودکار ممنوع است. APIهای قدیمی CANService که مستقیماً از driver دریافت می‌کنند برای سازگاری باقی مانده‌اند؛ مصرف‌کنندگان runtime یادشده از صف‌های مستقل استفاده می‌کنند. فیلدهای قدیمی NVS و API با پیشوند `can1` برای سازگاری حفظ شده‌اند و به تنظیمات سخت‌افزار CAN2 اشاره دارند.
- باس فیزیکی، پلاریته‌ی interrupt، فرکانس کریستال و سطح منطقی ترنسیور باید روی ماژول واقعی تأیید شوند. GPIOهای ESP32-S3 مقاوم در برابر 5V نیستند؛ اگر برد MCP2515/TJA1050 سیگنال‌های SPI/INT با سطح 5V دارد یا RXD ترنسیور TJA1051 با 3.3V سازگار نیست، از level shifter استفاده کنید.

## 17. شواهد راستی‌آزمایی

اندازه‌ی firmware، RAM ایستا و شمار آزمون‌ها وابسته به commit و toolchain هستند؛ این سند عمداً آن‌ها را ثابت نمی‌کند. برای نتیجه‌ی جاری باید از CI/خروجی build همان commit استفاده شود. build و آزمون نرم‌افزاری به‌تنهایی عملکرد سخت‌افزار متصل را اثبات نمی‌کنند.

- نتیجه‌ی جاری build و آزمون‌ها فقط از اجرای CI همان commit معتبر است.
- HARDWARE REQUIRED: اتصال واقعی CAN1/TJA1051 و CAN2/MCP2515/TJA1050، کریستال 8 MHz، ترافیک هم‌زمان، bus-off/recovery، OBD-II، سطوح منطقی، TFT، touch، Wi-Fi/BLE، کلیدها و SD.
- HARDWARE REQUIRED: کنسول USB Serial روی برد واقعی؛ build تنها وجود کد مسیر Serial را تأیید می‌کند.
- NOT RUN: ماتریس ترکیبی رابط‌ها، خطا/حذف SD، کمبود فضای Flash، نبود Wi-Fi/BLE و برد ESP32-S3 سفارشی؛ محیط فعلی شبیه‌ساز سخت‌افزار یا fixture چندرسانه‌ای ندارد.

وضعیت `UNVERIFIED` یعنی راه‌اندازی نرم‌افزاری driver/display انجام شده، اما وجود ترنسیور متصل، bus زنده یا پنل قابل مشاهده تأیید نشده است. این وضعیت عمداً با `READY` یکسان نیست.

## 18. نقشه‌ی آدرس‌های فلش

مقادیر زیر از partition tableهای CSV پروژه و باینری `partitions.bin` تولیدشده برای هر چهار محیط build تطبیق داده شده‌اند. آدرس imageهای سیستمی از تنظیمات ESP32-S3 در Arduino-ESP32 و تعریف‌های uploader در PlatformIO خوانده شده‌اند. این بررسی build-time است؛ فلش‌کردن روی برد انجام نشده است.

| Image سیستمی | آدرس | توضیح |
|---|---:|---|
| Bootloader | `0x000000` | `CONFIG_BOOTLOADER_OFFSET_IN_FLASH` برای ESP32-S3 |
| Partition table | `0x008000` | اندازه‌ی sector برابر `0x1000` |
| `boot_app0.bin` | `0x00E000` | image کمکی Arduino برای مقداردهی اولیه‌ی OTA data؛ با پارتیشن `otadata` هم‌آدرس است و پارتیشن جداگانه‌ای نیست |
| `firmware.bin` | `0x010000` | آدرس شروع آپلود برنامه (`ESP32_APP_OFFSET`) |

| پارتیشن | 4 MB، با یا بدون PSRAM | 16 MB، با PSRAM |
|---|---|---|
| `nvs` | `0x009000`, size `0x005000` | `0x009000`, size `0x005000` |
| `otadata` | `0x00E000`, size `0x002000` | `0x00E000`, size `0x002000` |
| `app0` | `0x010000`, size `0x180000` | `0x010000`, size `0x500000` |
| `app1` | `0x190000`, size `0x180000` | `0x510000`, size `0x500000` |
| `spiffs` | `0x310000`, size `0x0CF000` | `0xA10000`, size `0x5F0000` |

در پارتیشن 4 MB بازه‌ی `0x3DF000` تا `0x400000`، به اندازه‌ی `0x21000`، به پارتیشنی تخصیص نیافته است. جدول 16 MB تا انتهای `0x1000000` امتداد دارد. تغییر این جدول برای دستگاه‌های موجود می‌تواند داده‌های ذخیره‌شده را از بین ببرد و باید با مسیر مهاجرت/هشدار جداگانه انجام شود.
