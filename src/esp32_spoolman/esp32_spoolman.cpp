// esp32_spoolman.cpp — все глобальные переменные и тела функций
#include "esp32_spoolman.h"
#include "spool_format.h"  //D24: палитра имён цветов и санитизация подписи опции (host-тестируемо)
#include "card_uids.h"     //D29: кодек spool.extra["card_uids"] (host-тестируемо)
#include <Adafruit_NeoPixel.h>

//Управление WS2812 через NeoPixel (62% flash, 2MB partition — хватает)
Adafruit_NeoPixel strip(NUMPIXELS, PIN_DATA, NEO_GRB + NEO_KHZ800);

static void ws2812_send(uint32_t color) {
  strip.setPixelColor(0, color);
  strip.show();
}

//Определения глобальных переменных

MFRC522DriverPinSimple ss_pin(SS_PIN);
MFRC522DriverSPI driver{ ss_pin };
MFRC522 mfrc522{ driver };

GyverDBFile db(&LittleFS, "/data.db");
SettingsGyver sett("ESP to Spoolman", &db);

//D32: логгер пишет и в RAM-буфер (виджет b.Log), и в /log.txt на LittleFS — по строке.
FileLogger logger(1024);

size_t FileLogger::write(uint8_t v) {
  sets::Logger::write(v);  //RAM-буфер логгера: поведение виджета лога не меняется
  const char* line = nullptr;
  size_t len = 0;
  if (_split.feed(v, line, len)) appendLine(line, len);
  return 1;
}

//Строка в файл: открыть на дописывание, записать, закрыть. Ошибки FS — только в Serial:
//логгер не может логировать себя сам (рекурсия), а UI ломать не должен.
//Открытие/закрытие на строку — осознанно: File, открытый на сессию, пережил бы LittleFS.end()
//(«Save & Restart») и OTA-обновление фронтенда, то есть стал бы висячим дескриптором.
void FileLogger::appendLine(const char* data, size_t len) {
  File f = LittleFS.open(LOG_PATH, "a");
  if (!f) {
    Serial.print("Лог: ");
    Serial.print(LOG_PATH);
    Serial.println(" не открыт, строка потеряна");
    return;
  }
  f.write((const uint8_t*)data, len);
  f.write('\n');
  f.close();
}

bool LED_SP = false;
bool LED_MN = false;
bool LED_NFC = false;
int curentID;
bool wifiSettingMode = 0;
bool fl_status = false;
bool fl_led = false;
bool mmu = false;
bool writeMode = false;  //D10: режим записи — активация катушки/MMU/tag-scan заблокированы
uint8_t activePage = PAGE_MAIN;  //D18: страница, открытая в вебморде (Tabs больше нет)
uint32_t lastUID = 0;
//Машина записи (D16): ожидание метки после нажатия кнопки + таймаут 60 с
WriteState wstate = IDLE;
bool erasePending = false;  //D9: ожидание по кнопке «Стереть метку» (false — запись, T3)
unsigned long writeStart = 0;
//D31: разовый эффект светодиода после NFC-операции (2 с свечения либо мигания) —
//в это время статусный updateLED() не трогает цвет; подробности — в ledEffect()/ledEffectTick()
bool ledFxBlink = false;
bool ledFxOn = true;
uint32_t ledFxColor = 0;
String conflictUID = "";  //D13: UID, по которому уже сообщили владельца (повтор = перелинковка)
int conflictSpool = 0;    //D13: спул-владелец UID из 409
//D33: метка считается залинкованной -> запись ждёт подтверждения в вебморде (переходы —
//confirm_state.h). UID и содержимое метки запоминаются на момент касания: к моменту клика
//считыватель метку уже не видит (PICC_HaltA), и повторно читать её неоткуда.
wconfirm::State tagConfirm;
static String pendingUID = "";      //UID метки, ожидающей подтверждения
static String pendingPayload = "";  //её содержимое (NDEF payload) — по нему восстанавливается doc
//D35 (v1.3.4): confirmActivationSent и update-активация скрытого Confirm удалены — подтверждение
//рисуется видимыми кнопками в build() (D35), отдельного активирующего пакета больше нет.

//D33: снять ожидание подтверждения и забыть запомненную метку — только вместе: автомат и
//сохранённый UID/payload не должны разъезжаться (иначе подтверждение запишет чужую метку).
static void clearPendingConfirm() {
  tagConfirm.clear();
  pendingUID = "";
  pendingPayload = "";
}
unsigned long lastSpoolmanFail = 0;
unsigned long lastMoonrakerFail = 0;
bool prevSpoolmanOk = false;
bool prevMoonrakerOk = false;

//D22 (диагностика): PCD_PerformSelfTest() начинается с PCD_Reset(), который возвращает RFCfgReg
//к дефолту (0x40 = 33 dB) и сбрасывает выставленный при старте 0x70 (48 dB). Гипотеза отказа
//MIFARE_Read — деградация усиления приёма после самотеста; без лога gain это не проверить.
//always=false — печатать только при смене значения (тик watchdog раз в 30 с не должен спамить).
//D22: PCD_PerformSelfTest() завершается PCD_Init() — gain и режимы антенны сброшены в дефолт.
//После КАЖДОГО самотеста (boot, watchdog, reset) восстанавливаем RxGain 0x07 (48 дБ) + антенну,
//а не только логируем деградацию — иначе ридер с первого самотеста работает на 33 дБ.
static void restoreReaderConfig() {
  mfrc522.PCD_SetAntennaGain(0x07);
  mfrc522.PCD_AntennaOff();
  mfrc522.PCD_AntennaOn();
}

static void logReaderGain(const char* after, bool always) {
  static uint8_t lastGain = 0xFF;
  uint8_t gain = mfrc522.PCD_GetAntennaGain();
  if (!always && gain == lastGain) return;
  lastGain = gain;
  String msg = String("RC522 gain ") + after + ": 0x" + String(gain, HEX);
  if (gain != 0x70) msg += " (не 0x70: самотест вернул RxGain в дефолт)";
  Serial.println(msg);
  logger.println(gain == 0x70 ? msg : sets::Logger::warn() + msg);
}

TimerMs T_LED_AP(500, 1, 0);    //экс-T_LED_WIFI: только мигание синим в AP-режиме (имя врало — P2 ревью 1.3.9)
TimerMs T_SERVERS(3000, 1, 0);  //опрос Moonraker/Spoolman: было 500 мс — 4 блокирующих HTTP-запроса/с
TimerMs ResetLED(1000, 1, 0);
TimerMs WatchdogNFC(30000, 1, 0);
TimerMs T_LED_WRITE(250, 1, 0);
//D31: срок эффекта светодиода (2 с) — режим таймера: после срабатывания останавливается сам
TimerMs T_LED_FX(LED_EFFECT_MS, 0, 1);
//D31: период мигания ошибки — как у ожидания метки (T_LED_WRITE)
TimerMs T_LED_BLINK(LED_BLINK_MS, 1, 0);
//D30: слоты Gate опрашиваются списком катушек (limit=1000), поэтому реже цикла индикаторов (500 мс)
TimerMs T_GATES(10000, 1, 0);

//Сброс write-состояния: вызывается на ЛЮБОЙ переход таба (Review Focus 4),
//чтобы выход из подменю не оставлял активного ожидания метки.
static void resetWriteState() {
  db[web::w_id] = 0;
  db[web::w_uid] = "";
  db[web::w_type] = "";
  db[web::w_brand] = "";
  db[web::w_color] = "";  //D28: сватч цвета в меню записи гаснет вместе с полями метки
  lastUID = 0;  //метка, прочитанная до входа в режим, должна читаться заново
  //Выход из подменю отменяет ожидание метки немедленно (D16), конфликт 409 тоже сбрасывается
  wstate = IDLE;
  erasePending = false;
  conflictUID = "";
  conflictSpool = 0;
  //D33: неподтверждённая запись на залинкованную метку умирает вместе с ожиданием метки
  clearPendingConfirm();
}

//=== D28: цвет метки — LED-сватч (кружок) + подпись именем цвета ===
//Label.updateColor красит ТОЛЬКО текст значения (span.style.color), не полосу, поэтому цвет
//катушки показываем LED-виджетом: у него updateColor перекрашивает сам кружок (--color).
//Нейтральный серый — когда цвета нет (метка пуста / color_hex не разобран): сватч не врёт.
#define LED_SWATCH_OFF 0x303030

//Spoolman color_hex -> uint32 0xRRGGBB для сватча; has=false — hex не разобран.
static uint32_t swatchColor(const char* hex, bool& has) {
  uint8_t r, g, b;
  has = spoolfmt::parseHexColor(hex, r, g, b);
  if (!has) return LED_SWATCH_OFF;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

//Подпись сватча: имя ближайшего цвета палитры v1.3.2; hex не разобран — показываем сам hex
//(подпись не врёт молчанием), пусто — «—».
static String swatchLabel(const char* hex) {
  String label = "Цвет: ";
  if (!hex || !*hex) return label + "—";
  const char* name = spoolfmt::colorNameForHex(hex);
  label += name ? name : String(hex);
  return label;
}

//Интерфейс (D18, порядок — D26): корень build() — два подменю («Записать метку», «Настройки») и
//главная страница. Меню объявляются ПЕРВЫМИ: навигация вебморды строится в порядке объявления, а
//«Записать метку» — основной сценарий, он должен стоять выше индикаторов доступности (D26).
//Tabs не используются: переключение меню мгновенное, а build() вызывается библиотекой и при
//входе в меню (Build::Type::Menu + b.enterMenu() нужного блока). Главная страница — код build()
//вне меню, после endMenu() настроек.
void build(sets::Builder& b) {
  const bool menuAction = (b.build.type == sets::Build::Type::Menu);
  bool enteredWrite = false;
  bool enteredSettings = false;

  //==== Подменю «Записать метку» (D10: только чтение метки, активация заблокирована) ====
  //beginMenu/endMenu парные: первый виджет внутри меню попадает на отдельную страницу вебморды
  if (b.beginMenu("Записать метку")) {
    enteredWrite = menuAction && b.enterMenu();
    {
      sets::Group g(b, "Катушка для записи");
      if (b.Switch(web::f_notag, "Только катушки без метки")) {
        db[web::spool_sel] = 0;  //после смены фильтра индексы опций другие
        fetchSpoolList((bool)db[web::f_notag]);
        //Опции Select приходят только в пакете сборки страницы — перерисовываем её
        //(reload без force оставляет вебморду на этом же меню)
        b.reload();
      }
      String opts = db[web::spool_opts];
      b.Select(web::spool_sel, "Катушка", opts);
    }
    {
      sets::Group g(b, "Метка в поле считывателя");
      b.Label(web::w_id, "ID:");
      b.Label(web::w_uid, "UID:");
      b.Label(web::w_type, "Материал:");
      b.Label(web::w_brand, "Производитель:");
      //D28: цвет метки — сватч-кружок + имя цвета (в меню записи своего id: дубли id фронтенд не
      //принимает). Значение LED — «есть ли цвет»: пустая метка показывает нейтральный серый.
      String wColorHex = db[web::w_color];
      bool wHasColor = false;
      uint32_t wSwatch = swatchColor(wColorHex.c_str(), wHasColor);
      b.LED(web::led_tag_w, swatchLabel(wColorHex.c_str()), wHasColor, wSwatch, wSwatch);
    }
    {
      sets::Group g(b, "Действия");
      //«Записать метку»: катушка выбрана -> ждём метку (мигает жёлтый) -> запись по касанию (Task 3)
      if (b.Button(web::btn_write, "Записать метку")) {
        int spoolID = selectedSpoolID();
        if (spoolID <= 0) {
          Serial.println("Запись метки: катушка не выбрана");
          logger.println(sets::Logger::error() + "Запись метки: катушка не выбрана (обновите список катушек)");
          set_led_color(RED);
        } else {
          lastUID = 0;  //метка, уже лежащая на считывателе, должна прочитаться заново
          erasePending = false;  //намерение: запись (не стирание)
          //D33: новая операция не наследует неподтверждённую запись прошлой метки
          clearPendingConfirm();
          writeStart = millis();
          wstate = WAIT_TAG;
          T_LED_WRITE.restart();
          Serial.println("Запись метки: ожидание метки (катушка ID:" + String(spoolID) + ")");
          logger.println("Запись метки: приложите метку к считывателю (ожидание " + String(WRITE_TAG_TIMEOUT / 1000) + " с), катушка ID:" + String(spoolID));
        }
      }
      //«Стереть метку» (D9/D14): катушку выбирать не нужно — владелец UID ищется в Spoolman по тегу.
      //Ожидание метки то же, что у записи (D16): мигание жёлтым, таймаут 60 с, отмена при выходе из подменю.
      if (b.Button(web::btn_erase, "Стереть метку", sets::Colors::Red)) {
        lastUID = 0;  //метка, уже лежащая на считывателе, должна прочитаться заново
        erasePending = true;
        //D33: стирание не наследует ожидание подтверждения записи (иначе Confirm запустил бы запись)
        clearPendingConfirm();
        writeStart = millis();
        wstate = WAIT_TAG;
        T_LED_WRITE.restart();
        Serial.println("Стирание метки: ожидание метки");
        logger.println("Стирание метки: приложите метку к считывателю (ожидание " + String(WRITE_TAG_TIMEOUT / 1000) + " с)");
      }
      //D35 (v1.3.4): вместо скрытого Confirm с update-активацией — две ВИДИМЫЕ кнопки.
      //Update-пакет не имеет повтора и терялся до готовности перерисованной страницы — диалог
      //не появлялся. Кнопки рисуются обычным Build-пакетом, клик приходит Set-действием.
      if (tagConfirm.pending()) {
        String linked = String(tagConfirm.spoolID());
        b.Label("Метка залинкована к катушке ID:", linked);
        //D35 (v1.3.4): Button не имеет перегрузки с bool* (в отличие от Confirm) — значение Set
        //читаем явно из b.build.value: «1» — подтверждение, «0» — не подтверждение.
        //Builder::_isSet срабатывает на любое Set-действие с этим id, значение само по себе
        //решением не является.
        bool ok = false;
        if (b.Button(web::confirm_write, "Записать на залинкованную метку (ID " + linked + ")", sets::Colors::Red)) {
          ok = b.build.value.toBool();
          if (tagConfirm.reply(ok) == wconfirm::Reply::Confirm) {
            tagConfirm.confirm();
            Serial.println("Запись метки " + pendingUID + ": подтверждена в вебморде");
            logger.println("Метка " + pendingUID + ": запись подтверждена в вебморде");
          }
        }
        //Отмена: снять ожидание, ничего не писать (как по таймауту)
        if (b.Button(web::confirm_cancel, "Отмена")) {
          String uid = pendingUID;
          wstate = IDLE;
          erasePending = false;
          clearPendingConfirm();
          Serial.println("Запись метки " + uid + ": отменена в вебморде");
          logger.println(sets::Logger::warn() + "Запись метки " + uid + ": отменена в вебморде, запись не выполнена");
          sett.reload();  //кнопки и подсказка должны исчезнуть из вебморды
        }
      }
    }
    {
      //D26: логгер повторяем внутри меню записи — результат записи/стирания виден там же, где
      //нажимали кнопку, не уходя на главную. b.Log — обычный виджет, вызывается с тем же
      //буфером (logger). ВНИМАНИЕ: id должен быть УНИКАЛЕН глобально (фронтенд ругается на
      //дубли даже на разных страницах), поэтому у копии в меню свой ключ H(log_w).
      sets::Group g(b, "Логгер");
      b.Log(H(log_w), logger);
    }
    b.endMenu();
  }

  //==== Подменю «Настройки»: wifi, серверы Spoolman/Moonraker, версия устройства ====
  if (b.beginMenu("Настройки")) {
    enteredSettings = menuAction && b.enterMenu();
    {
      sets::Group g(b, "WiFi");
      b.Input(web::wifi_ssid, "SSID");
      b.Pass(web::wifi_pass, "Password");
      //Режим задаётся переключателем SW_WIFI на плате при старте: AP — настройка, STA — работа в сети
      b.Label("Режим:", wifiSettingMode ? "AP (точка доступа)" : "STA (клиент сети)");
    }
    {
      sets::Group g(b, "Сервера");
      if (b.Input(web::serverSpoolman, "IP:Port Spoolman")) {
        logger.println("Set IP:Port Spoolman:" + String(db[web::serverSpoolman]));
        db.update();
      }
      if (b.Input(web::serverMoonraker, "IP:Port Moonraker(Принтер)")) {
        logger.println("Set IP:Port Moonraker(Принтер):" + String(db[web::serverMoonraker]));
        db.update();
      }
      if (b.Switch(web::sw_mmu, "Устанавливать в MMU (Happy Hare)", &mmu)) {
        db[web::sw_mmu] = mmu;
        db.update();
      }
      if (b.Button(web::apply1, "Save & Restart")) {
        db.update();
        LittleFS.end();
        delay(100);
        ESP.restart();
      }
    }
    {
      sets::Group g(b, "Устройство");
      b.Label("Версия прошивки:", VER);
    }
    b.endMenu();
  }

  //==== Главная страница (D26: код вне меню, поэтому идёт после endMenu() настроек) ====
  {
    sets::Group g(b, "Доступность");
    b.LED(web::led_mn, "Статус Moonraker", LED_MN, sets::Colors::Yellow, sets::Colors::Green);
    b.LED(web::led_sp, "Статус Spoolman", LED_SP, sets::Colors::Yellow, sets::Colors::Green);
    b.LED(web::led_nfc, "NFC считыватель", LED_NFC, sets::Colors::Yellow, sets::Colors::Green);
    //Проверка доступности: loop() по флагу дёргает statusServer() + curent_filament_name()
    if (b.Button(web::apply2, "Проверка доступности")) {
      fl_status = true;
    }
  }
  {
    sets::Group g(b, "Текущая катушка установленная в SpoolMan");
    b.Label(web::id, "ID:");
    b.Label(web::filament, "Имя:");
  }
  //D30: слоты MMU видны ТОЛЬКО при включённом «Устанавливать в MMU» (sw_mmu); значения
  //Gate0..Gate3 заполняет updateMmuGates() на цикле проверки серверов.
  if (mmu) {
    sets::Group g(b, "MMU Gate");
    b.Label(web::gate0, "Gate0:");
    b.Label(web::gate1, "Gate1:");
    b.Label(web::gate2, "Gate2:");
    b.Label(web::gate3, "Gate3:");
  }
  {
    sets::Group g(b, "NFC Метка");
    b.Label(web::nfc_id, "ID:");
    b.Label(web::nfc_uid, "UID:");
    b.Label(web::nfc_name, "Материал:");
    b.Label(web::nfc_brand, "Производитель:");
    //D28: вместо hex-строки — сватч + имя цвета из палитры v1.3.2 (окраска — upd.updateColor
    //при чтении метки, при пересборке страницы цвет приходит из colorOn/colorOff)
    String colorHex = db[web::nfc_color];
    bool hasColor = false;
    uint32_t swatch = swatchColor(colorHex.c_str(), hasColor);
    b.LED(web::led_tag, swatchLabel(colorHex.c_str()), hasColor, swatch, swatch);
  }
  {
    sets::Group g(b, "Логгер");
    b.Log(H(log), logger);
  }

  //==== Смена страницы (D18) ====
  //Вход в меню библиотека сообщает вызовом build() с типом Menu; возврат на главную — Menu,
  //в котором ни один beginMenu() не подтвердил вход (enterMenu() == false).
  //Полная сборка (Build) и действия виджетов (Set) страницу не меняют: после sett.reload()
  //вебморда остаётся на той же странице, поэтому writeMode не сбрасывается на каждом чтении метки.
  if (menuAction) {
    uint8_t page = enteredWrite ? PAGE_WRITE : (enteredSettings ? PAGE_SETTINGS : PAGE_MAIN);
    if (page != activePage) {
      resetWriteState();  //переход страницы отменяет ожидание метки (D16)
      activePage = page;
      writeMode = (page == PAGE_WRITE);
      if (page == PAGE_WRITE) {
        logger.println("Режим записи: активация катушки заблокирована");
        //Список катушек тянем при входе в меню записи, затем перерисовываем страницу:
        //опции Select попадают в вебморду только пакетом сборки (reload без force меню не закрывает)
        fetchSpoolList((bool)db[web::f_notag]);
        sett.reload();
      } else if (page == PAGE_SETTINGS) {
        logger.println("Режим настроек: активация катушки разрешена");
      } else {
        logger.println("Главная страница: активация катушки разрешена");
      }
    }
  }
}

void update(sets::Updater& upd) {
  //D32: FileLogger — наследник sets::Logger, а template-update() взял бы его по значению
  //(Logger некопируемый) — явно приводим к базовой ссылке, чтобы работал update(id, Logger&)
  upd.update(H(log), static_cast<sets::Logger&>(logger));
  upd.update(H(log_w), static_cast<sets::Logger&>(logger));  //копия логгера в меню записи

  //D28: сватч цвета метки — updateColor красит кружок (Label.updateColor красил бы только текст).
  //Шлём только при смене цвета: подпись (имя цвета) приходит с пересборкой страницы, которую
  //делает readNFC (sett.reload после касания).
  //D35 (v1.3.4): скрытый Confirm и update-активация убраны — подтверждение видно кнопками в build().

  //D37 (v1.3.4): id update-пакетов — только из DB_KEYS. H(led_tag)/H(led_tag_w) маскируются
  //(DB_HASH_MASK) и расходятся со значением web::*, которым виджет нарисован в build() — пакет
  //уходит на несуществующий id и фронтенд молча отбрасывает его. У led_tag/led_tag_w хэши
  //SH=0x6C724DB0/0x523C8666 — старшие биты реально терялись; у log/log_w/gate0..3 хэши в 29 бит
  //укладываются (проверено расчётом), поэтому там H() и web::* численно совпадают, но для
  //единообразия правила upd.update-вызовы ведут по id из DB_KEYS (см. D37-тест в host-tests).
  static String lastMainColor, lastWriteColor;
  String mainColor = db[web::nfc_color];
  if (mainColor != lastMainColor) {
    lastMainColor = mainColor;
    bool has = false;
    upd.updateColor((size_t)web::led_tag, swatchColor(mainColor.c_str(), has));
  }
  String writeColor = db[web::w_color];
  if (writeColor != lastWriteColor) {
    lastWriteColor = writeColor;
    bool has = false;
    upd.updateColor((size_t)web::led_tag_w, swatchColor(writeColor.c_str(), has));
  }

  //D30: слоты Gate обновляются тем же каналом и в том же цикле, что и индикаторы серверов;
  //шлём только изменившиеся слоты (опрос — раз в T_GATES, лишний трафик каждую секунду не нужен)
  if (mmu) {
    static String lastGates[4] = { "", "", "", "" };
    //D37 (v1.3.4): id — из DB_KEYS (web::gateN), не H(gateN): оба числа сегодня совпадают
    //(хэш «gate0..3» < 2^29), но правило одно для всех update-пакетов — id только из DB_KEYS.
    const size_t gateIds[4] = { (size_t)web::gate0, (size_t)web::gate1, (size_t)web::gate2, (size_t)web::gate3 };
    const String gateTexts[4] = { String(db[web::gate0]), String(db[web::gate1]), String(db[web::gate2]), String(db[web::gate3]) };
    for (int i = 0; i < 4; i++) {
      if (gateTexts[i] == lastGates[i]) continue;
      lastGates[i] = gateTexts[i];
      upd.update(gateIds[i], gateTexts[i]);
    }
  }

}

//Обновление светодиода по текущему состоянию
void updateLED() {
  if (wifiSettingMode == 1) {
    //AP режим — мигаем синим
    if (T_LED_AP.ready()) set_led_color(BLUE);
    else set_led_color(0);
  } else if (WiFi.status() != WL_CONNECTED) {
    set_led_color(RED);
  } else if (LED_NFC && LED_SP && LED_MN) {
    set_led_color(GREEN);
  } else {
    set_led_color(BLUE);
  }
}

void setup() {
  Serial.begin(115200);
  //D35: без терминала хост не читает CDC FIFO, TX-ring (256 байт) переполняется и каждый
  //Serial.println блокирует loop до ~2 с (tx_timeout_ms=100 × 20 попыток в HWCDC): чтение метки
  //растягивалось до 25-30 с, HTTP-таймауты давали «обрыв связи». Нулевой таймаут — запись в
  //переполненный ring не ждёт, лишний отладочный вывод отбрасывается (файловый логгер пишет всё).
  Serial.setTxTimeoutMs(0);
  //D32: LittleFS поднимается ДО первых записей в логгер — иначе версия и boot reason (D27) не
  //попали бы в файл. Файловый лог живёт одну сессию: /log.txt усекается на каждом старте
  //(решение владельца — файл не растёт между перезагрузками).
  if (LittleFS.begin(true)) {
    if (LittleFS.exists(LOG_PATH) && !LittleFS.remove(LOG_PATH)) {
      Serial.print("Лог: ");
      Serial.print(LOG_PATH);
      Serial.println(" не усечён (remove не удался)");
    }
  } else {
    Serial.println("Лог: LittleFS не поднялась — файловый лог отключён");
  }
  Serial.println("Версия " VER);
  logger.println("Версия " VER);
  //D27: причина перезагрузки — единственный след «зависания, вылеченного reboot'ом»
  //(power-on, panic, watchdog, brownout, software reset и т.д. — числовой код esp_reset_reason())
  Serial.println("Boot reason: " + String(esp_reset_reason()));
  logger.println("Boot reason: " + String(esp_reset_reason()));
  strip.begin();
  strip.setBrightness(50);
  strip.clear();
  strip.show();
  T_LED_AP.setPeriodMode();
  T_SERVERS.setPeriodMode();  //P2 (1.3.9): опрос серверов вынесен из 500-мс таймера LED-мигания
  ResetLED.setPeriodMode();
  WatchdogNFC.setPeriodMode();
  T_LED_WRITE.setPeriodMode();
  T_LED_FX.setTimerMode();    //D31: эффект — разовый (2 с), сам себя останавливает
  T_LED_BLINK.setPeriodMode();  //D31: мигание ошибки
  T_GATES.setPeriodMode();      //D30: опрос слотов MMU Gate
  set_led_color(0);  //гасим светодиод

  WiFi.mode(WIFI_AP_STA);

  sett.begin();
  sett.onBuild(build);
  sett.onUpdate(update);
  sett.config.updateTout = 1000;
  sett.setVersion(VER);

  //LittleFS уже поднята в начале setup() (D32: до первых записей логгера)
  db.begin();
  db.init(web::wifi_ssid, "");
  db.init(web::wifi_pass, "");
  db.init(web::serverMoonraker, "");
  db.init(web::serverSpoolman, "");
  db.init(web::nfc_id, 0);
  db.init(web::nfc_name, "");
  db.init(web::nfc_brand, "");
  db.init(web::nfc_color, "");
  db.init(web::nfc_uid, "");
  db.init(web::id, 0);
  db.init(web::filament, "");
  //Ключи подменю «Записать метку» (v1.3 Task 2, меню с v1.3.1)
  db.init(web::spool_sel, 0);
  db.init(web::spool_ids, "");
  db.init(web::spool_opts, "");
  db.init(web::f_notag, false);
  db.init(web::w_id, 0);
  db.init(web::w_uid, "");
  db.init(web::w_type, "");
  db.init(web::w_brand, "");
  db.init(web::w_color, "");  //D28: цвет метки в поле считывателя (сватч в меню записи)
  //D30: слоты MMU Gate по умолчанию пусты (иначе Label прочитал бы мусор прошлой прошивки)
  db.init(web::gate0, "пусто");
  db.init(web::gate1, "пусто");
  db.init(web::gate2, "пусто");
  db.init(web::gate3, "пусто");
  db.init(web::btn_write, 0);
  db.init(web::btn_erase, 0);
  db.init(web::confirm_write, 0);  //D33: окно подтверждения записи на залинкованную метку
  mmu = db[web::sw_mmu]; 
  db[web::nfc_id] = 0;
  db[web::nfc_name] = "";
  db[web::nfc_brand] = "";
  db[web::nfc_color] = "";
  db[web::nfc_uid] = "";
  db[web::id] = 0;
  db[web::filament] = "";
  //Режим записи не переживает перезагрузку: иначе блокировка активации «залипнет»
  activePage = PAGE_MAIN;
  db[web::spool_sel] = 0;
  db[web::f_notag] = false;
  db[web::spool_ids] = "";
  db[web::spool_opts] = "";
  db[web::w_id] = 0;
  db[web::w_uid] = "";
  db[web::w_type] = "";
  db[web::w_brand] = "";
  db[web::w_color] = "";  //D28: сватч меню записи стартует без цвета
  //D30: до первого опроса слотов — «пусто» (раздел виден только при включённом MMU)
  db[web::gate0] = "пусто";
  db[web::gate1] = "пусто";
  db[web::gate2] = "пусто";
  db[web::gate3] = "пусто";
  writeMode = false;

  pinMode(SW_WIFI, INPUT_PULLUP);

  if (digitalRead(SW_WIFI) == 0 or !db[web::wifi_ssid].length()) {
    wifiSettingMode = 1;
    WiFi.softAP("AP ESP_to_Spoolman");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  if (digitalRead(SW_WIFI) == 1 and db[web::wifi_ssid].length()) {
    wifiSettingMode = 0;
    WiFi.begin(db[web::wifi_ssid], db[web::wifi_pass]);
    Serial.print("Connect STA");
    int tries = 20;
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print('.');
      if (!--tries) break;
    }
    Serial.println();
    Serial.print(F("IP: "));
    Serial.println(WiFi.localIP());
    WiFi.softAPdisconnect(true);
  }

  if (WiFi.status() == WL_CONNECTED) {
    statusServer();
    curent_filament_name(curentID);
  }

  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, HIGH);

  //Явно задаём SPI пины для RC522 на ESP32-C3
  SPI.begin(2, 3, 4, 5);  // SCK=2, MISO=3, MOSI=4, SS=5
  delay(10);

  mfrc522.PCD_Init();
  mfrc522.PCD_SetAntennaGain(0x07);
  mfrc522.PCD_AntennaOff();
  mfrc522.PCD_AntennaOn();
  MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);
  delay(100);

  if (mfrc522.PCD_PerformSelfTest()) {
    Serial.print("Найден считыватель RC522: ");
    MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);
    logger.print("Найден считыватель RC522: ");
    MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, logger);
    LED_NFC = true;
  } else {
    Serial.println("Не найден считыватель RC522");
    logger.println(sets::Logger::error() + "Не найден считыватель RC522");
    LED_NFC = false;
  }
  logReaderGain("после самотеста (boot)", true);  //D22: фактический RxGain после PCD_Reset внутри самотеста
  restoreReaderConfig();  //самотест сбросил gain — возвращаем 0x07 (48 дБ)
}

//D30: слоты Gate0..Gate3 из extra.mmu_gate_map (определение — в секции «MMU Gate» ниже)
static void updateMmuGates();
//D31: тик эффекта светодиода (определение — в секции «D31» ниже)
static bool ledEffectTick();

void loop() {
  sett.tick();
  T_LED_AP.tick();
  T_SERVERS.tick();
  ResetLED.tick();
  WatchdogNFC.tick();
  T_GATES.tick();

  if (T_SERVERS.ready() && WiFi.status() == WL_CONNECTED) {
    statusServer();
    curent_filament_name(curentID);
    //D30: слоты MMU Gate — в том же цикле проверки серверов; свой 10-секундный период, потому
    //что это GET списка катушек (limit=1000), а цикл индикаторов — 3 с (см. T_SERVERS)
    if (T_GATES.ready()) updateMmuGates();
  }

  if (ResetLED.ready()) {
    fl_led = false;
  }

  if (WatchdogNFC.ready()) {
    if (!mfrc522.PCD_PerformSelfTest()) {
      Serial.println("NFC reader hang detected, resetting...");
      logger.println(sets::Logger::error() + "NFC reader hang detected, resetting...");
      resetMFRC();
    } else {
      logReaderGain("после самотеста (watchdog)", false);  //D22: печатаем только при смене (самотест сбрасывает gain)
      restoreReaderConfig();  //P1-фикс: gain после успешного самотеста не восстанавливался — ридер работал на 33 дБ
    }
  }

  if (WiFi.status() != WL_CONNECTED && wifiSettingMode == 0) {
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect > 30000) {
      lastReconnect = millis();
      Serial.println("WiFi lost, reconnecting...");
      WiFi.reconnect();
    }
  }

  //Режим записи — производное от открытой страницы (D18: меню «Записать метку»)
  writeMode = (activePage == PAGE_WRITE);

  readNFC();

  //LED: ожидание метки (мигание жёлтым) имеет приоритет над статусным обновлением (D16), затем —
  //результат NFC-операции (2 с, D31), и только потом статусный цвет updateLED()
  if (!writeTick()) {
    if (!ledEffectTick()) updateLED();
  }

  //Читаем Serial
  if (Serial.available()) {
    String i = Serial.readString();
    String status = F("status");
    if (i == status) {
      statusServer();
      curent_filament_name(curentID);
    } else {
      int setID = i.toInt();
      SetSpool(setID);
    }
  } else if (fl_status == true) {
    fl_status = false;
    statusServer();
    curent_filament_name(curentID);
  }
}

void set_led_color(uint32_t color) {
  ws2812_send(color);
  fl_led = true;
}

//=== D31: семантика светодиода NFC-операций ===
//Чтение/запись/стирание показывают РЕЗУЛЬТАТ операции 2 с, затем возвращается статусный цвет
//updateLED() (сеть, Moonraker, Spoolman — без изменений):
//  успешное чтение метки            -> YELLOW 2 с
//  ошибка чтения (в т.ч. пустая)    -> мигание RED 2 с
//  успешная запись/стирание         -> PURPLE 2 с
//  ошибка записи/стирания           -> мигание RED 2 с
//Приоритеты в loop(): ожидание метки (writeTick, D16) -> эффект NFC (эта пара) -> updateLED().
//Срок эффекта — таймер T_LED_FX (2 с, режим таймера), мигание — T_LED_BLINK (250 мс).
static void ledEffect(uint32_t color, bool blink) {
  ledFxColor = color;
  ledFxBlink = blink;
  ledFxOn = true;
  set_led_color(color);
  T_LED_BLINK.ready();  //сброс возможного непрочитанного флага прошлого эффекта (старт мигания с свечения)
  T_LED_FX.restart();
  T_LED_BLINK.restart();
}

//true — LED в этом проходе loop() ведёт эффект NFC-операции, статусный цвет трогать нельзя.
static bool ledEffectTick() {
  T_LED_FX.tick();
  T_LED_BLINK.tick();
  if (!T_LED_FX.active()) return false;  //эффект истёк (или не запускался) — дальше статусный цвет
  if (ledFxBlink && T_LED_BLINK.ready()) {
    ledFxOn = !ledFxOn;
    set_led_color(ledFxOn ? ledFxColor : 0);
  }
  return true;
}

//D31: результат записи/стирания метки: успех — PURPLE 2 с, ошибка — мигание RED 2 с.
static void setWriteResult(bool ok) {
  ledEffect(ok ? PURPLE : RED, !ok);
}

void errorServer(String error) {
  Serial.println("Ошибка ответа сервера: " + String(error));
  logger.println(sets::Logger::error() + "Ошибка ответа сервера: " + String(error));
  set_led_color(RED);
}

void resetMFRC() {
  digitalWrite(RST_PIN, LOW);
  delayMicroseconds(100);
  digitalWrite(RST_PIN, HIGH);
  mfrc522.PCD_Init();
  mfrc522.PCD_SetAntennaGain(0x07);
  mfrc522.PCD_AntennaOff();
  mfrc522.PCD_AntennaOn();
  Serial.println(F("Reboot RC522"));
  MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);
  if (mfrc522.PCD_PerformSelfTest()) {
    Serial.println("Найден считыватель RC522: ");
    MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);
    logger.print("Найден считыватель RC522: ");
    MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, logger);
    LED_NFC = true;
  } else {
    Serial.println("Не найден считыватель RC522");
    logger.println(sets::Logger::error() + "Не найден считыватель RC522");
    LED_NFC = false;
  }
  logReaderGain("после самотеста (reset)", true);  //D22
  restoreReaderConfig();  //gain ставился ДО самотеста и был сброшен — восстанавливаем
}

void statusServer() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMoonrakerFail < RETRY_DELAY) return;

  HTTPClient http;
  String geturl = "http://" + String(db[web::serverMoonraker]) + "/server/spoolman/status";
  http.begin(geturl);
  http.setTimeout(1500);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");
  int httpResponseCode = http.GET();

  //P2 (1.3.9): успех — только 200; любой другой код (включая 404/500) раньше красил LED_MN
  //в «сервер доступен» и попадал в парсинг
  if (httpResponseCode == 200) {
    String response = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    http.end();
    if (error) {
      errorServer(error.f_str());
      LED_MN = false;
      return;
    }
    bool statusServer = doc["result"]["spoolman_connected"];
    int newID = doc["result"]["spool_id"];  //null → 0
    bool changed = (newID != curentID);     //reload шлём только при изменении отображаемого
    curentID = newID;
    db[web::id] = curentID;

    if (statusServer) {
      if (!prevMoonrakerOk) {
        Serial.println("Сервер Moonraker доступен");
        logger.println("Сервер Moonraker доступен");
      }
      LED_MN = true;
      prevMoonrakerOk = true;
    } else {
      if (prevMoonrakerOk) {
        Serial.println("Сервер Moonraker не доступен");
        logger.println(sets::Logger::error() + "Сервер Moonraker не доступен");
      }
      LED_MN = false;
      prevMoonrakerOk = false;
    }
    if (changed) sett.reload();
  } else {
    errorServer(String(httpResponseCode));
    http.end();
    lastMoonrakerFail = millis();
    if (prevMoonrakerOk) sett.reload();  //LED_MN гаснет — страница должна перестроиться
    LED_MN = false;
    prevMoonrakerOk = false;
  }
}

void curent_filament_name(int ID) {
  if (WiFi.status() != WL_CONNECTED) {
    db[web::filament] = "0";
    return;
  }
  if (millis() - lastSpoolmanFail < RETRY_DELAY) {
    db[web::filament] = "0";
    return;
  }

  //P2 (1.3.9): ID=0 — активной катушки нет; /api/v1/spool/0 отвечает 404, который раньше
  //трактовался как «сервер доступен» и опрашивался каждый тик. Ноль не опрашиваем.
  if (ID <= 0) {
    if (String(db[web::filament]) != "0") {
      db[web::filament] = "0";
      sett.reload();
    }
    return;
  }

  HTTPClient http;
  String url = "http://" + String(db[web::serverSpoolman]) + "/api/v1/spool/" + String(ID);
  http.begin(url);
  http.setTimeout(1500);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  int httpResponseCode = http.GET();
  //P2 (1.3.9): успех — только 200; 404/500 раньше красили LED_SP в зелёный («Сервер Spoolman
  //доступен») и скармливали парсеру тело ошибки
  if (httpResponseCode == 200) {
    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      errorServer(error.f_str());
      db[web::filament] = "0";
      LED_SP = false;
      lastSpoolmanFail = millis();
      sett.reload();
      return;
    }

    const char* name = doc["filament"]["name"] | "0";

    //Лог имени катушки только при смене
    static String lastName = "";
    bool nameChanged = (lastName != name);
    db[web::filament] = name;
    if (nameChanged) {
      lastName = name;
      Serial.println("ID: " + String(ID) + ", Name: " + String(name));
      logger.println(sets::Logger::warn() + "ID: " + String(ID) + ", Name: " + String(name));
    }

    bool wasOk = prevSpoolmanOk;
    if (!prevSpoolmanOk) {
      Serial.println("Сервер Spoolman доступен");
      logger.println("Сервер Spoolman доступен");
    }
    LED_SP = true;
    prevSpoolmanOk = true;
    //reload только при изменении отображаемого (имя или появление связи): P2 (1.3.9)
    if (nameChanged || !wasOk) sett.reload();
  } else {
    http.end();
    errorServer(String(httpResponseCode));
    db[web::filament] = "0";
    bool wasOk = prevSpoolmanOk;
    if (prevSpoolmanOk) {
      Serial.println("Сервер Spoolman не доступен");
      logger.println(sets::Logger::error() + "Сервер Spoolman не доступен");
    }
    LED_SP = false;
    prevSpoolmanOk = false;
    lastSpoolmanFail = millis();
    if (wasOk) sett.reload();  //LED_SP и имя погасли — страница должна перестроиться
  }
}

bool getIDSpool(int ID) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (millis() - lastSpoolmanFail < RETRY_DELAY) return false;

  HTTPClient http;
  String url = "http://" + String(db[web::serverSpoolman]) + "/api/v1/spool/" + String(ID);
  http.begin(url);
  http.setTimeout(1500);
  int code = http.GET();
  http.end();

  if (code != 200) lastSpoolmanFail = millis();
  return (code == 200);
}

void SetSpool(int SetID) {
  //D10: в режиме записи активация катушки запрещена — печать не прерываем
  if (writeMode) {
    Serial.println("Режим записи: установка катушки ID " + String(SetID) + " заблокирована");
    logger.println(sets::Logger::warn() + "Режим записи: установка катушки ID " + String(SetID) + " заблокирована");
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (getIDSpool(SetID)) {
      HTTPClient http;
      String geturl = "http://" + String(db[web::serverMoonraker]) + "/server/spoolman/spool_id";
      http.begin(geturl);
      http.setTimeout(1500);
      http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
      http.addHeader("Content-Type", "application/json");

      JsonDocument doc_post;
      doc_post["spool_id"] = SetID;
      String requestBody_post;
      serializeJson(doc_post, requestBody_post);
      int httpResponseCode = http.POST(requestBody_post);

      if (httpResponseCode > 0) {
        String response = http.getString();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, response);
        if (error) {
          errorServer(error.f_str());
          http.end();
          return;
        }
        int root_id = doc["result"]["spool_id"];
        Serial.println("Установка катушки с  ID: " + String(root_id));
        logger.println("Установка катушки с  ID: " + String(root_id));
      } else {
        errorServer(String(httpResponseCode));
        lastMoonrakerFail = millis();
      }
      http.end();

      statusServer();
      if (curentID == SetID) {
        Serial.println("Катушка с ID:" + String(SetID) + " успешно установлена");
        logger.println("Катушка с ID:" + String(SetID) + " успешно установлена");
        curent_filament_name(curentID);
        set_led_color(PURPLE);  //D31: успешная установка — PURPLE (лимонный цвет отменён)
      } else {
        Serial.println("Ошибка установки катушки с ID: " + String(SetID));
        logger.println(sets::Logger::error() + "Ошибка установки катушки с ID: " + String(SetID));
        set_led_color(RED);
      }
      sett.reload();
    } else {
      Serial.println("Катушка c ID: " + String(SetID) + " не существует");
      logger.println(sets::Logger::error() + "Катушка c ID: " + String(SetID) + " не существует");
    }
  }
}

void mmu_next_spoolid(int SetID) {
  if (writeMode) return;  //D10: в режиме записи MMU не трогаем
  if (WiFi.status() == WL_CONNECTED) {
    if (getIDSpool(SetID)) {
      HTTPClient http;
      String url = "http://" + String(db[web::serverMoonraker]) + "/printer/gcode/script";
      http.begin(url);
      http.setTimeout(1500);
      http.addHeader("Content-Type", "application/json");
      String httpRequestData = "{\"script\":\"MMU_GATE_MAP NEXT_SPOOLID=" + String(SetID) + "\"}";
      int httpResponseCode = http.POST(httpRequestData);

      if (httpResponseCode > 0) {
        Serial.println("Установка катушки с ID в MMU: " + String(SetID));
        logger.println("Установка катушки с ID в MMU: " + String(SetID));
        set_led_color(PURPLE);  //D31: успешная установка в MMU — PURPLE (лимонный цвет отменён)
      } else {
        Serial.println("Ошибка установки катушки с ID в MMU: " + String(SetID));
        logger.println(sets::Logger::error() + "Ошибка установки катушки с ID в MMU: " + String(SetID));
        set_led_color(RED);
        lastMoonrakerFail = millis();
      }
      http.end();
    }
  }
}

//=== Чтение метки v1.3: NDEF TLV-парсер + UID-first + диспетчер форматов ===

//Память пользователя NTAG21x начинается со страницы 4 (стр.0-1 UID, стр.2 lock, стр.3 CC)
#define NDEF_PAGE_START 4
#define NDEF_MAX_BYTES  888  //NTAG216 — верхняя граница; NTAG215: 496, NTAG213: 144

#define NDEF_TLV_NULL   0x00  //NULL TLV — пропускаем
#define NDEF_TLV_NDEF   0x03  //NDEF Message TLV — то, что нам нужно
#define NDEF_TLV_END    0xFE  //Terminator TLV — дальше TLV-структуры нет
#define NDEF_TLV_LONG   0xFF  //признак 16-битной длины TLV

//Возможные исходы разбора TLV (нужны, чтобы читать память кусками до исчерпания длины)
#define NDEF_SCAN_MORE    0  //TLV/запись ещё не дочитаны — нужен следующий кусок
#define NDEF_SCAN_FOUND   1  //payload найден: *payloadOfs, *payloadLen
#define NDEF_SCAN_STOPPED 2  //0xFE или битая структура — NDEF-записи на метке нет

//Разбор заголовка одной NDEF-записи: flags(TNF|IL|SR|ME), TYPE_LENGTH, PAYLOAD_LENGTH, [ID_LENGTH], TYPE, payload.
//Читаем только первую запись (openspool/FilaMan пишут одну).
static uint16_t ndefParseRecord(const uint8_t* msg, uint16_t msgLen, uint16_t* payloadOfs) {
  if (msgLen < 3) return 0;
  uint8_t flags = msg[0];
  bool shortRecord = flags & 0x10;  //SR: длина payload в 1 байте
  bool idLength = flags & 0x08;     //IL: есть ID_LENGTH
  uint8_t typeLen = msg[1];
  uint16_t pos = 2;

  uint32_t payloadLen;
  if (shortRecord) {
    payloadLen = msg[pos];
    pos += 1;
  } else {
    if ((uint32_t)pos + 4 > msgLen) return 0;
    payloadLen = ((uint32_t)msg[pos] << 24) | ((uint32_t)msg[pos + 1] << 16) | ((uint32_t)msg[pos + 2] << 8) | msg[pos + 3];
    pos += 4;
  }

  if (idLength) {
    if ((uint32_t)pos + 1 > msgLen) return 0;
    uint8_t idLen = msg[pos];
    pos += 1;
    if ((uint32_t)pos + idLen > msgLen) return 0;
    pos += idLen;
  }

  pos += typeLen;
  if (payloadLen == 0 || (uint32_t)pos + payloadLen > msgLen) return 0;
  *payloadOfs = pos;
  return (uint16_t)payloadLen;  //payloadLen <= msgLen <= NDEF_MAX_BYTES — в uint16 влезает
}

//Разбор TLV-структуры: 0x03 + длина (1 байт либо 0xFF + 2 байта) + значение.
//Остальные TLV (Lock/Memory Control/Proprietary) пропускаем по длине.
static uint8_t ndefScanTLV(const uint8_t* mem, uint16_t memLen, uint16_t* payloadOfs, uint16_t* payloadLen) {
  uint16_t pos = 0;
  while (pos < memLen) {
    uint8_t type = mem[pos];
    if (type == NDEF_TLV_END) return NDEF_SCAN_STOPPED;
    if (type == NDEF_TLV_NULL) {
      pos++;
      continue;
    }
    if (pos + 1 >= memLen) return NDEF_SCAN_MORE;

    uint32_t len = mem[pos + 1];
    uint16_t hdrLen = 2;
    if (len == NDEF_TLV_LONG) {
      if ((uint32_t)pos + 4 > memLen) return NDEF_SCAN_MORE;
      len = ((uint32_t)mem[pos + 2] << 8) | mem[pos + 3];
      hdrLen = 4;
    }

    uint32_t valueOfs = (uint32_t)pos + hdrLen;
    if (valueOfs + len > memLen) {
      //TLV-длина ещё не исчерпана: для 0x03 — ждём следующие страницы, для прочих TLV — структура битая
      return (type == NDEF_TLV_NDEF) ? NDEF_SCAN_MORE : NDEF_SCAN_STOPPED;
    }

    if (type == NDEF_TLV_NDEF) {
      uint16_t ofs = 0;
      uint16_t lenPayload = ndefParseRecord(mem + valueOfs, (uint16_t)len, &ofs);
      if (lenPayload == 0) return NDEF_SCAN_STOPPED;
      *payloadOfs = (uint16_t)valueOfs + ofs;
      *payloadLen = lenPayload;
      return NDEF_SCAN_FOUND;
    }
    pos = (uint16_t)(valueOfs + len);
  }
  return NDEF_SCAN_MORE;
}

//Полный UID (4/7/10 байт) в hex UPPERCASE без разделителей — формат Spoolman tag API.
//uint32-хеш для дедупа считается отдельно в readNFC().
String formatUID() {
  String uid;
  uid.reserve(mfrc522.uid.size * 2);
  static const char hex[] = "0123456789ABCDEF";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    uid += hex[(mfrc522.uid.uidByte[i] >> 4) & 0x0F];
    uid += hex[mfrc522.uid.uidByte[i] & 0x0F];
  }
  return uid;
}

//D22: диагностика NFC собирается здесь же (struct и определения — в секции «D22» ниже)
static void nfcDiagNoteRead(uint8_t page, uint8_t status);

//Payload NDEF-записи (application/json у openspool/FilaMan). «» — если TLV/запись не распарсились.
//Читаем страницы с 4-й кусками по 4 страницы (MIFARE_Read отдаёт 16 байт + 2 CRC),
//пока не исчерпана TLV-длина, не встретится 0xFE, не кончится память метки или лимит NDEF_MAX_BYTES.
String ndefPayload() {
  static uint8_t mem[NDEF_MAX_BYTES];  //вне стека loop(): до 888 байт
  uint16_t memLen = 0;
  uint8_t buffer[18];

  for (uint16_t page = NDEF_PAGE_START; memLen < NDEF_MAX_BYTES; page += 4) {
    byte byteCount = sizeof(buffer);
    MFRC522::StatusCode readStatus = mfrc522.MIFARE_Read((byte)page, buffer, &byteCount);
    if (readStatus != MFRC522::StatusCode::STATUS_OK) {
      nfcDiagNoteRead((uint8_t)page, (uint8_t)readStatus);  //D22: первая ошибка чтения за касание
      break;  //дальше страниц нет (NTAG213/215) либо ошибка чтения
    }
    for (byte i = 0; i < 16 && memLen < NDEF_MAX_BYTES; i++) {
      mem[memLen++] = buffer[i];
    }

    uint16_t payloadOfs = 0, payloadLen = 0;
    uint8_t scan = ndefScanTLV(mem, memLen, &payloadOfs, &payloadLen);
    if (scan == NDEF_SCAN_FOUND) {
      String payload;
      payload.reserve(payloadLen);
      for (uint16_t i = 0; i < payloadLen; i++) {
        payload += (char)mem[payloadOfs + i];
      }
      return payload;
    }
    if (scan == NDEF_SCAN_STOPPED) break;
  }
  return "";
}

//=== D22: диагностика NFC — ATQA/SAK, статусы проб типа чипа, первый отказ чтения, CC ===

//Собирается за одно касание и печатается ОДИН раз на UID (анти-спам): только если за касание
//случился отказ MIFARE_Read. Цель — по логам владельца увидеть, почему метка не читается:
//ATQA/SAK показывают, опознана ли карта вообще, статусы проб 226/130/40 — виден ли тип чипа,
//первый отказ чтения и CC — где именно рассыпается обмен.
struct NfcDiag {
  bool triggered;         //был отказ MIFARE_Read (проба типа либо страница данных/CC)
  bool dataFailed;        //отказ именно чтения данных: firstErrPage/firstErrStatus заполнены
  uint8_t firstErrPage;
  uint8_t firstErrStatus;
  uint8_t probePage[3];   //страницы проб tagChipInfo: 226 -> 130 -> 40
  uint8_t probeStatus[3];
  uint8_t probeCnt;
  uint8_t reselectCnt;    //D23: сколько раз восстанавливали выбор карты после отказа чтения
  uint8_t reselectFail;   //из них неудачных (WUPA/SELECT не прошли — карта ушла из поля)
  uint8_t reselectStatus; //статус последней неудачной WUPA
};
static NfcDiag nfcDiag;
static String nfcDiagLoggedUID;  //UID, для которого диагностика уже напечатана

static void nfcDiagReset() {
  nfcDiag = NfcDiag();
}

//Отказ чтения страницы: запоминаем первую ошибку за касание
static void nfcDiagNoteRead(uint8_t page, uint8_t status) {
  if (status == (uint8_t)MFRC522::StatusCode::STATUS_OK) return;
  nfcDiag.triggered = true;
  if (nfcDiag.dataFailed) return;
  nfcDiag.dataFailed = true;
  nfcDiag.firstErrPage = page;
  nfcDiag.firstErrStatus = status;
}

//Проба типа чипа (tagChipInfo): статус каждой попытки идёт в тот же лог
static void nfcDiagProbe(uint8_t page, uint8_t status) {
  if (nfcDiag.probeCnt < 3) {
    nfcDiag.probePage[nfcDiag.probeCnt] = page;
    nfcDiag.probeStatus[nfcDiag.probeCnt] = status;
    nfcDiag.probeCnt++;
  }
  if (status != (uint8_t)MFRC522::StatusCode::STATUS_OK) nfcDiag.triggered = true;
}

//D23: переселекция карты после отказа чтения (HALT -> WUPA -> SELECT). Фиксируем сам факт и
//исход: на NTAG215 проба 226 ожидаемо даёт NAK, и переселекция — нормальный шаг чтения,
//а не отказ. Отказ переселекции = карта ушла из поля, обмен дальше невозможен.
static void nfcDiagNoteReselect(bool selected, uint8_t wakeupStatus) {
  if (nfcDiag.reselectCnt < 0xFF) nfcDiag.reselectCnt++;
  if (selected) return;
  nfcDiag.triggered = true;
  nfcDiag.reselectFail++;
  nfcDiag.reselectStatus = wakeupStatus;
}

//Определение — в секции «Запись метки v1.3»: нужны tagChipInfo()/pageHex()
static void nfcDiagLogOnce(const String& uid);

//UID-first: POST /api/v1/tag/scan {"uid":...,"reader_id":"esp32-to-spoolman"}.
//true — Spoolman нашёл линковку UID -> катушка (*matched = matched_spool_id).
bool spoolIDByUID(const String& uid, int& matched) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!uid.length()) return false;
  if (millis() - lastSpoolmanFail < RETRY_DELAY) return false;

  HTTPClient http;
  String url = "http://" + String(db[web::serverSpoolman]) + "/api/v1/tag/scan";
  http.begin(url);
  http.setTimeout(1500);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");

  JsonDocument request;
  request["uid"] = uid;
  request["reader_id"] = READER_ID;
  String requestBody;
  serializeJson(request, requestBody);

  int httpResponseCode = http.POST(requestBody);
  String response;
  if (httpResponseCode > 0) response = http.getString();
  http.end();  //единственный end(), до разбора JSON

  if (httpResponseCode <= 0) {
    errorServer(String(httpResponseCode));
    lastSpoolmanFail = millis();
    return false;
  }
  if (httpResponseCode != 200) {
    Serial.println("tag/scan: HTTP " + String(httpResponseCode));
    logger.println(sets::Logger::warn() + "tag/scan: HTTP " + String(httpResponseCode));
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    errorServer(error.f_str());
    return false;
  }
  if (doc["matched_spool_id"].isNull() || !doc["matched_spool_id"].is<int>()) return false;

  matched = doc["matched_spool_id"].as<int>();
  return matched > 0;
}

//Ключ есть и содержит ID числом либо числовой строкой
//(у Spool Painter spool_id — число, у FilaMan sm_id — строка).
static bool jsonHasID(JsonDocument& doc, const char* key) {
  return doc[key].is<int>() || doc[key].is<const char*>();
}

//Десериализация payload метки с детектом формата:
//openspool (protocol=="openspool" + spool_id; либо характерный набор ключей) или FilaMan legacy (sm_id).
//true — payload разобран и формат распознан; doc остаётся валидным для чтения полей.
bool parseTagJSON(const String& payload, JsonDocument& doc) {
  doc.clear();
  if (!payload.length()) return false;

  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("Ошибка расшифровки JSON: ");
    Serial.println(error.c_str());
    logger.print(sets::Logger::error() + "Ошибка расшифровки JSON: ");
    logger.println(error.c_str());
    return false;
  }

  //openspool (в т.ч. расширение spool_id от Spool Painter)
  if (doc["protocol"] == "openspool" && jsonHasID(doc, "spool_id")) return true;
  //FilaMan legacy
  if (jsonHasID(doc, "sm_id")) return true;
  //openspool без protocol, но с характерными строковыми ключами формата
  if (jsonHasID(doc, "spool_id") && (doc["brand"].is<const char*>() || doc["color_hex"].is<const char*>() || doc["type"].is<const char*>())) return true;
  return false;
}

//ID катушки из разобранного JSON: сперва spool_id (openspool), затем sm_id (FilaMan). -1 — нет.
static int tagSpoolID(JsonDocument& doc) {
  if (doc["spool_id"].is<int>()) return doc["spool_id"].as<int>();
  if (doc["spool_id"].is<const char*>()) return atoi(doc["spool_id"].as<const char*>());
  if (doc["sm_id"].is<int>()) return doc["sm_id"].as<int>();
  if (doc["sm_id"].is<const char*>()) return atoi(doc["sm_id"].as<const char*>());
  return -1;
}

//Машина записи по касанию метки в WAIT_TAG (определение — в секции «Запись метки v1.3» в конце файла)
static void tagWriteFlow(const String& uid, const String& payloadIn, JsonDocument& tagDoc, bool parsed);

//D34 (v1.3.4): дедуп касаний по гэпу. Одиночный пропуск WUPA (штатная потеря ответа, D23) не
//должен обнулять lastUID — иначе та же метка переобрабатывается, а в WAIT_TAG это перезапускало
//бы таймер подтверждения вечно. Метка считается «ушедшей» только если её не видно дольше DEDUP_GAP.
#define TAG_DEDUP_GAP 2000
static unsigned long lastSeenMs = 0;

void readNFC() {
  if (!mfrc522.PICC_IsNewCardPresent()) {
    //D34: метки нет сейчас — но сбрасываем дедуп только по гэпу (см. выше)
    if (millis() - lastSeenMs > TAG_DEDUP_GAP) lastUID = 0;
    return;
  }
  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  //Дедуп по uint32-хешу UID: полный UID нужен только для HTTP
  uint32_t uidHash = 0;
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    uidHash = (uidHash << 8) | mfrc522.uid.uidByte[i];
  }
  lastSeenMs = millis();  //D34: метка видна — гэп отсчитывается от этого момента
  if (uidHash == lastUID) {
    mfrc522.PICC_HaltA();
    return;
  }
  lastUID = uidHash;

  String uid = formatUID();
  Serial.print("Метка UID: ");
  Serial.println(uid);
  nfcDiagReset();  //D22: диагностика собирается заново на каждое касание (лог — один на UID)

  //Содержимое метки читаем всегда: из него заполняются поля (nfc_* либо w_*)
  String payload = ndefPayload();
  JsonDocument doc;
  bool parsed = parseTagJSON(payload, doc);
  int tagID = parsed ? tagSpoolID(doc) : -1;

  //=== Режим записи (D10): только читаем метку и заполняем поля w_*, активация заблокирована ===
  if (writeMode) {
    db[web::w_id] = tagID > 0 ? tagID : 0;
    db[web::w_uid] = uid;
    db[web::w_type] = doc["type"] | "";
    db[web::w_brand] = doc["brand"] | "";
    db[web::w_color] = doc["color_hex"] | "";  //D28: цвет для сватча в меню записи
    Serial.println("Режим записи: метка " + uid + " ID:" + String(tagID) + " Тип:" + String(doc["type"] | "") + " Бренд:" + String(doc["brand"] | "") + " (активация заблокирована)");
    logger.println("Режим записи: метка " + uid + " ID:" + String(tagID) + " Тип:" + String(doc["type"] | "") + " Бренд:" + String(doc["brand"] | ""));

    //Кнопка «Записать метку»/«Стереть метку» нажата — метка в поле считывателя, запускаем машину (D16).
    //Намерение (erasePending) снимаем здесь же: иначе следующее касание повторило бы операцию.
    if (wstate == WAIT_TAG) {
      bool doErase = erasePending;
      erasePending = false;
      //D34: повторное касание ТОЙ ЖЕ pending-метки — состояние не трогаем: подтверждение всё
      //ещё ждём, таймер не перезапускаем (иначе касание вечно продлевало бы ожидание).
      if (tagConfirm.pending() && uid == pendingUID) {
        mfrc522.PICC_HaltA();
        nfcDiagLogOnce(uid);
        return;
      }
      //D33: новое касание в ожидании — новая операция: прежнее ожидание подтверждения снимается
      //(иначе подтверждение старой метки могло бы сработать уже после записи новой)
      clearPendingConfirm();
      if (doErase) {
        wstate = ERASING;
        setWriteResult(eraseTag());
        wstate = IDLE;
      } else {
        //D33: UID уже залинкован к катушке -> не пишем сразу, ждём подтверждения в вебморде.
        //Проверка по Spoolman (spoolIDByUID) — та же, что на главной: она же ловит «чужая метка».
        //Нет WiFi/Spoolman недоступен — подтверждения не будет, пишем как раньше (D33: не linked).
        int linked = -1;
        if (spoolIDByUID(uid, linked)) {
          //wstate остаётся WAIT_TAG: ожидание подтверждения живёт на том же таймауте и той же
          //индикации, что обычное ожидание метки (D16/D31) — новых состояний не вводим.
          tagConfirm.begin(linked, millis());
          pendingUID = uid;
          pendingPayload = payload;
          Serial.println("Запись метки " + uid + ": UID залинкован к ID " + String(linked) + ", ждём подтверждения в вебморде");
          logger.println(sets::Logger::warn() + "Метка " + uid + " залинкована к ID " + String(linked) + " — подтвердите запись в вебморде (иначе отмена через " + String(WRITE_TAG_TIMEOUT / 1000) + " с)");
        } else {
          wstate = WRITING;
          tagWriteFlow(uid, payload, doc, parsed);
          wstate = IDLE;
        }
      }
    } else {
      //D31: в меню записи светодиод ведёт себя так же, как на главной (раньше не светился вовсе):
      //распознанная метка — YELLOW 2 с, пустая/нераспознанная — мигание RED 2 с (D20-лог выше)
      if (parsed) ledEffect(YELLOW, false);
      else ledEffect(RED, true);
    }

    mfrc522.PICC_HaltA();
    nfcDiagLogOnce(uid);  //D22: один лог на UID при первом отказе чтения
    sett.reload();
    return;
  }

  //UID прочитанной метки — на главную страницу (D18), независимо от того, разобралось ли содержимое
  db[web::nfc_uid] = uid;

  if (parsed) {
    db[web::nfc_id] = tagID > 0 ? tagID : 0;
    db[web::nfc_name] = doc["type"] | "";
    db[web::nfc_brand] = doc["brand"] | "";
    db[web::nfc_color] = doc["color_hex"] | "";
    sett.reload();
    Serial.println("Прочитана метка с ID:" + String(tagID) + " Тип:" + String(doc["type"] | "") + " Бренд:" + String(doc["brand"] | ""));
    logger.println("Прочитана метка с ID:" + String(tagID) + " Тип:" + String(doc["type"] | "") + " Бренд:" + String(doc["brand"] | ""));
  }

  mfrc522.PICC_HaltA();
  nfcDiagLogOnce(uid);  //D22: диагностика до HTTP — лог идёт в порядке касания

  //1) UID-first: линковка UID -> катушка в Spoolman имеет приоритет над содержимым метки (D2)
  int matched = -1;
  if (spoolIDByUID(uid, matched)) {
    //D20: линковка UID есть, а содержимого на метке нет (пусто / JSON невалиден / формат не
    //распознан) — катушку НЕ устанавливаем и MMU не трогаем: привязка UID сама по себе не
    //означает, что метка несёт данные катушки.
    if (!parsed) {
      String why = payload.length() ? " (содержимое не распознано)" : "";
      Serial.println("Метка " + uid + ": метка пустая, UID привязан к spool ID " + String(matched) + ", катушка не установлена" + why);
      logger.println(sets::Logger::warn() + "Метка " + uid + ": метка пустая, UID привязан к spool ID " + String(matched) + ", катушка не установлена" + why);
      //D31: пустая метка — ошибка чтения в терминах владельца: мигание RED 2 с (D20-лог остаётся)
      ledEffect(RED, true);
      return;
    }
    Serial.println("Метка привязана к катушке ID:" + String(matched));
    logger.println("Метка " + uid + " привязана к катушке ID:" + String(matched));
    SetSpool(matched);
    if (mmu == true) {
      mmu_next_spoolid(matched);
    }
    //D31: результат чтения — YELLOW 2 с (после установки: успешная установка красит PURPLE,
    //но для владельца исход операции — «метка прочитана»)
    ledEffect(YELLOW, false);
    return;
  }

  //2) Fallback: содержимое метки — openspool spool_id, затем FilaMan sm_id (D3)
  if (tagID > 0) {
    Serial.println("Установка катушки по содержимому метки ID:" + String(tagID));
    logger.println("Установка катушки по содержимому метки ID:" + String(tagID));
    SetSpool(tagID);
    if (mmu == true) {
      mmu_next_spoolid(tagID);
    }
    ledEffect(YELLOW, false);  //D31: метка прочитана
    return;
  }

  //3) Ни UID-линковки, ни распознанного формата
  Serial.print("Метка не распознана, payload: ");
  Serial.println(payload);
  logger.println(sets::Logger::error() + "Метка " + uid + " не распознана (нет линковки и NDEF-формата)");
  ledEffect(RED, true);  //D31: ошибка чтения — мигание RED 2 с
}

//=== Режим записи: список катушек для Select ===

//D19 (v1.3.1): настоящие группы Select синтаксисом библиотеки «[заголовок]опции».
//Механика сверена с фронтендом Settings 1.3.16 (src/web/settings.h, SelectWidget в сжатом
//script): разбор групп включается, только если строка опций начинается с '['; заголовки ищутся
//по /\[(.+?)\]([^\[]+)/g, тело группы («до следующей '['») режется по ';' и '\n', последняя
//пустая опция отбрасывается. Индекс результата ГЛОБАЛЬНЫЙ по всем опциям всех групп
//(счётчик идёт сквозь группы), заголовки групп индексов не занимают — поэтому spool_ids
//строится по флэт-списку опций.
//D24 (v1.3.2): санитизация данных катушки вынесена в spoolfmt::sanitizeOptionText()
//(заголовок spool_format.h — чистый C++, чтобы правило тестировалось на хосте).
//'['/']' сломали бы разбор групп (и сдвинули индексы), ';'/'\n' добавили бы лишнюю опцию,
//ведущий '~' спрятал бы опцию, а '~' в заголовке — всю группу.
static String spoolTextPart(const char* raw) {
  String s = raw ? raw : "";
  spoolfmt::sanitizeOptionText(s.begin());
  s.trim();
  return s;
}

static const char* spoolMaterial(JsonObjectConst sp) {
  const char* m = sp["filament"]["material"] | "";
  return m ? m : "";
}

static int spoolId(JsonObjectConst sp) {
  return sp["id"] | -1;
}

//Порядок D19: по материалу (без учёта регистра), при равном материале — по id (стабильность)
static bool spoolBefore(JsonObjectConst a, JsonObjectConst b) {
  int cmp = strcasecmp(spoolMaterial(a), spoolMaterial(b));
  if (cmp != 0) return cmp < 0;
  return spoolId(a) < spoolId(b);
}

//Заголовок группы = материал катушки: в подписи опции материал уже не дублируем.
//Пустой материал заголовком быть не может: «[]» не совпадёт с разбором групп и весь список
//рассыпется на одну группу без заголовка (индексы при этом сохранятся, но группировка пропадёт).
static String spoolGroupTitle(JsonObjectConst sp) {
  String title = spoolTextPart(spoolMaterial(sp));
  if (!title.length()) title = "Без материала";
  return title;
}

//Подпись опции (D24 v1.3.2): «ID 37 · бренд · имя цвета». Материал — в заголовке группы,
//ID первым: по нему владелец сверяет опцию с катушкой в Spoolman. В Spoolman имени цвета нет,
//только color_hex, поэтому цвет — имя ближайшего цвета фиксированной палитры
//(spoolfmt::colorNameForHex). Hex не разобран — показываем сам hex (подпись не врёт молчанием),
//пусто — часть опускаем. Санитизация прежняя (spoolTextPart), через неё идёт и имя цвета.
static String spoolOptionLabel(JsonObjectConst sp) {
  String label = "ID ";
  label += String(spoolId(sp));

  String brand = spoolTextPart(sp["filament"]["vendor"]["name"] | "");
  if (brand.length()) {
    label += " · ";
    label += brand;
  }

  const char* colorHex = sp["filament"]["color_hex"] | "";
  const char* colorName = spoolfmt::colorNameForHex(colorHex);
  String color = spoolTextPart(colorName ? colorName : colorHex);
  if (color.length()) {
    label += " · ";
    label += color;
  }
  return label;
}

//GET /api/v1/spool?allow_archived=false -> spool_opts с настоящими группами по материалу
//(«[PETG]бренд · цвет · ID:12;...;[PLA]...», опции одного материала через ';', материал в подписи
//не дублируется) и параллельный csv id в spool_ids: b.Select отдаёт ГЛОБАЛЬНЫЙ индекс опции,
//а не id катушки, поэтому ids строится строго по флэт-списку опций (Review Focus 3).
//noTagOnly — только катушки без tags.
//Больше N_OPT_MAX катушек — берём первые N_OPT_MAX (порядок ответа API) и логируем усечение (Q8);
//усечение считается по катушкам (опциям), заголовки групп лимит не расходуют.
bool fetchSpoolList(bool noTagOnly) {
  db[web::spool_opts] = "";
  db[web::spool_ids] = "";

  if (WiFi.status() != WL_CONNECTED) {
    logger.println(sets::Logger::error() + "Список катушек: нет WiFi");
    return false;
  }
  if (millis() - lastSpoolmanFail < RETRY_DELAY) {
    logger.println(sets::Logger::warn() + "Список катушек: Spoolman недоступен, повтор позже");
    return false;
  }

  HTTPClient http;
  String url = "http://" + String(db[web::serverSpoolman]) + "/api/v1/spool?allow_archived=false";
  http.begin(url);
  http.setTimeout(BULK_HTTP_TIMEOUT_MS);  //D41: bulk-список идёт дольше 2 с
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  int httpResponseCode = http.GET();
  if (httpResponseCode != 200) {
    http.end();
    Serial.println("Список катушек: HTTP " + String(httpResponseCode));
    logger.println(sets::Logger::error() + "Список катушек: HTTP " + String(httpResponseCode));
    lastSpoolmanFail = millis();
    return false;
  }

  //Ответ на проде ~54 КБ — в JsonDocument целиком не влезает: читаем поток с фильтром нужных полей
  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["filament"]["material"] = true;
  filter[0]["filament"]["vendor"]["name"] = true;
  filter[0]["filament"]["color_hex"] = true;
  filter[0]["tags"] = true;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    Serial.print("Список катушек: ошибка разбора ");
    Serial.println(error.c_str());
    logger.println(sets::Logger::error() + "Список катушек: ошибка разбора " + String(error.c_str()));
    lastSpoolmanFail = millis();
    return false;
  }

  //Собираем только валидные катушки (id > 0), сортируем по материалу и лишь затем строим опции:
  //сортировать готовые String-опции дороже по памяти, а JsonObject в массиве — это 2 указателя.
  static JsonObject rows[N_OPT_MAX];
  int cnt = 0, matched = 0;
  for (JsonObject sp : doc.as<JsonArray>()) {
    //Фильтр «только без метки»: у катушки нет ни одной привязанной метки
    if (noTagOnly && sp["tags"].size() > 0) continue;
    if (spoolId(sp) <= 0) continue;
    matched++;
    if (cnt >= N_OPT_MAX) continue;  //дальше только считаем, чтобы залогировать усечение
    rows[cnt++] = sp;
  }

  //Сортировка вставками: список короткий (десятки), массивы фиксированные — без heap-аллокаций
  for (int i = 1; i < cnt; i++) {
    JsonObject cur = rows[i];
    int j = i - 1;
    while (j >= 0 && spoolBefore(cur, rows[j])) {
      rows[j + 1] = rows[j];
      --j;
    }
    rows[j + 1] = cur;
  }

  String opts, ids, groupTitle;
  for (int i = 0; i < cnt; i++) {
    //Граница группы — смена материала (без учёта регистра); заголовок берём у первой катушки
    //группы, катушки группы разделяются ';'. Заголовок индекса не занимает, ids идёт по опциям.
    String title = spoolGroupTitle(rows[i]);
    if (i == 0 || strcasecmp(title.c_str(), groupTitle.c_str()) != 0) {
      opts += '[';
      opts += title;
      opts += ']';
      groupTitle = title;
    } else {
      opts += ';';
    }
    opts += spoolOptionLabel(rows[i]);
    if (i) ids += ',';
    ids += String(spoolId(rows[i]));
  }

  db[web::spool_opts] = opts;
  db[web::spool_ids] = ids;
  int sel = db[web::spool_sel];
  if (sel < 0 || sel >= cnt) db[web::spool_sel] = 0;  //после смены фильтра индексов могло стать меньше

  if (matched > cnt) {
    logger.println(sets::Logger::warn() + "Список сокращён: " + String(cnt) + " из " + String(matched) + " (лимит " + String(N_OPT_MAX) + ")");
  } else if (cnt == 0) {
    logger.println(sets::Logger::warn() + "Список катушек пуст" + String(noTagOnly ? " (фильтр «без метки»)" : ""));
  } else {
    logger.println("Список катушек: " + String(cnt));
  }
  return cnt > 0;
}

//id катушки по индексу выбранной опции Select: id берём из параллельного spool_ids (Review Focus 3).
//-1 — ничего не выбрано либо списка нет.
int selectedSpoolID() {
  int idx = db[web::spool_sel];
  if (idx < 0) return -1;

  String ids = db[web::spool_ids];
  int cur = 0, start = 0;
  while (start <= (int)ids.length()) {
    int end = ids.indexOf(',', start);
    String part = (end < 0) ? ids.substring(start) : ids.substring(start, end);
    if (cur == idx) {
      int id = part.toInt();
      return id > 0 ? id : -1;
    }
    if (end < 0) break;
    start = end + 1;
    cur++;
  }
  return -1;
}

//=== D30: MMU Gate — Gate0..Gate3 из extra.mmu_gate_map (только при включённом MMU) ===

//Значение extra-поля как текст: Spoolman хранит extra словарём строк, но числовые поля
//(mmu_gate_map) в ответе бывают и числом — приводим к строке, не теряя значение.
static String extraText(JsonVariantConst v) {
  if (v.is<const char*>()) return String(v.as<const char*>());
  if (v.is<int>()) return String(v.as<int>());
  if (v.is<bool>()) return v.as<bool>() ? "1" : "0";
  return "";
}

//Подпись слота: «ID 12 · PETG+ Чёрный». Имя — filament.name, как в разделе «Текущая катушка»
//(на проде это «PETG чёрный», «PLA зелёный светящийся»). Если имени нет — собираем из материала
//и имени цвета палитры v1.3.2. Без хвоста остаётся «ID 12».
static String gateText(JsonObjectConst sp) {
  String text = "ID " + String(spoolId(sp));
  String name = sp["filament"]["name"] | "";
  name.trim();
  if (!name.length()) {
    String tail = sp["filament"]["material"] | "";
    tail.trim();
    const char* colorHex = sp["filament"]["color_hex"] | "";
    const char* colorName = spoolfmt::colorNameForHex(colorHex);
    if (colorName && *colorName) {
      if (tail.length()) tail += ' ';
      tail += colorName;
    }
    name = tail;
  }
  if (name.length()) {
    text += " · ";
    text += name;
  }
  return text;
}

//GET /api/v1/spool?limit=1000: катушки с extra.mmu_gate_map = "0".."3" раскладываются по слотам,
//остальные слоты — «пусто». Поле приходит JSON-строкой («-1» по умолчанию), поэтому сравнение —
//по нормализованному String, а не числом. Два спула на один gate: побеждает первый в ответе API
//(в лог — предупреждение). Вызывается с цикла проверки серверов, период — T_GATES.
static void updateMmuGates() {
  if (!mmu) return;  //D30: раздел Gate живёт только при включённом «Устанавливать в MMU»
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastSpoolmanFail < RETRY_DELAY) return;

  HTTPClient http;
  String url = "http://" + String(db[web::serverSpoolman]) + "/api/v1/spool?limit=1000";
  http.begin(url);
  http.setTimeout(BULK_HTTP_TIMEOUT_MS);  //тот же bulk-список, что и в spoolsByCardUid (D41)
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != 200) {
    http.end();
    Serial.println("MMU Gate: список катушек -> HTTP " + String(code));
    logger.println(sets::Logger::warn() + "MMU Gate: список катушек не получен (HTTP " + String(code) + ")");
    if (code <= 0) lastSpoolmanFail = millis();
    return;
  }

  //Список идёт потоком с фильтром нужных полей (как список катушек для Select): целиком он
  //в JsonDocument не влезает
  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["filament"]["name"] = true;  //подпись слота — имя филамента, как на главной
  filter[0]["filament"]["material"] = true;
  filter[0]["filament"]["color_hex"] = true;
  filter[0]["extra"]["mmu_gate_map"] = true;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    Serial.println("MMU Gate: ошибка разбора " + String(error.c_str()));
    logger.println(sets::Logger::warn() + "MMU Gate: ответ не разобран (" + String(error.c_str()) + ")");
    return;
  }

  String slot[4] = { "пусто", "пусто", "пусто", "пусто" };
  bool taken[4] = { false, false, false, false };
  int conflicts = 0;
  for (JsonObject sp : doc.as<JsonArray>()) {
    String gate = extraText(sp["extra"]["mmu_gate_map"]);
    gate.trim();
    //Слот — ровно один символ '0'..'3': «-1» (не назначено), пусто и «10» не слоты
    if (gate.length() != 1 || gate[0] < '0' || gate[0] > '3') continue;
    int idx = gate[0] - '0';
    if (taken[idx]) {
      conflicts++;
      continue;
    }
    taken[idx] = true;
    slot[idx] = gateText(sp);
  }

  db[web::gate0] = slot[0];
  db[web::gate1] = slot[1];
  db[web::gate2] = slot[2];
  db[web::gate3] = slot[3];
  if (conflicts) {
    logger.println(sets::Logger::warn() + "MMU Gate: у " + String(conflicts) + " катушек gate занят другой катушкой — показана первая в ответе");
  }
}

//=== Запись метки v1.3: lock-проверка, NDEF-запись, линковка (D5–D9, D13–D16) ===

#define NDEF_TYPE_JSON     "application/json"  //MIME-тип NDEF-записи openspool/FilaMan
#define NDEF_TYPE_JSON_LEN 16                  //strlen("application/json")

//Служебные адреса NTAG21x (NXP NTAG213/215/216 Rev. 3.2, §8.5): наличие страницы динамических
//lock-байтов определяет тип чипа, страницы конфигурации — AUTH0 и PROT.
struct TagChipInfo {
  const char* name;      //имя чипа для логов
  uint8_t dynLockPage;   //динамические lock-байты: 28h/82h/E2h
  uint8_t cfgPage;       //первая страница конфигурации: байт 3 — AUTH0
  uint8_t accessPage;    //вторая страница конфигурации: байт 0, бит 7 — PROT
  uint16_t ccSize;       //размер данных NDEF по CC (байт)
  uint8_t ccSizeCode;    //CC байт 2 = ccSize / 8
  uint8_t dataLastPage;  //последняя страница данных по CC
};

//D23: переселекция карты после неудачного обмена (HALT -> WUPA -> SELECT). После короткого
//4-битного ответа (NAK) PCD теряет следующий длинный 16-байтовый ответ: MIFARE_Read уходит в
//STATUS_TIMEOUT (диагностика владельца: 226=0xFF(NAK), 130/40=0x03(TIMEOUT), CC читается только
//после переселекции). WUPA/SELECT возвращают карту в ACTIVE, и следующий обмен снова проходит.
//Исход уходит в диагностику (nfcDiagNoteReselect). false — карта в поле не выбирается:
//продолжать обмен бессмысленно, вызывающий код считает чтение неуспешным.
//Внутри nfcDiagLogOnce та же пара вызовов оставлена на месте: там она — часть самой диагностики
//(печатает ATQA и статус WUPA), а не восстановление после отказа, и в счётчик не попадает.
static bool tagReselect() {
  uint8_t atqa[2] = { 0, 0 };
  byte atqaSize = sizeof(atqa);
  mfrc522.PICC_HaltA();
  MFRC522::StatusCode wakeup = mfrc522.PICC_WakeupA(atqa, &atqaSize);
  bool selected = wakeup == MFRC522::StatusCode::STATUS_OK && mfrc522.PICC_ReadCardSerial();
  nfcDiagNoteReselect(selected, (uint8_t)wakeup);
  return selected;
}

//Страница читается (нет NAK по несуществующему адресу) — так определяется тип чипа.
//D22: статус каждой пробы уходит в диагностику (один лог на UID).
//D23: после отказа (NAK — это и есть ответ на несуществующую страницу) карта переселектируется,
//иначе NAK от пробы 226 убивал бы следующие чтения 130/40. Возвращается ИСХОДНЫЙ статус: успешная
//переселекция не делает несуществующую страницу читаемой.
static bool tagPageReadable(byte page) {
  uint8_t buf[18];
  byte n = sizeof(buf);
  MFRC522::StatusCode status = mfrc522.MIFARE_Read(page, buf, &n);
  bool ok = status == MFRC522::StatusCode::STATUS_OK;
  nfcDiagProbe((uint8_t)page, (uint8_t)status);
  if (ok) return true;
  tagReselect();
  return false;
}

//Тип чипа по наличию служебных страниц: 216 -> 226h, 215 -> 82h, 213 -> 28h
static bool tagChipInfo(TagChipInfo& chip) {
  static const TagChipInfo c213 = { "NTAG213", 40, 41, 42, 144, 0x12, 39 };
  static const TagChipInfo c215 = { "NTAG215", 130, 131, 132, 496, 0x3E, 127 };
  static const TagChipInfo c216 = { "NTAG216", 226, 227, 228, 888, 0x6E, 225 };
  if (tagPageReadable(c216.dynLockPage)) {
    chip = c216;
    return true;
  }
  if (tagPageReadable(c215.dynLockPage)) {
    chip = c215;
    return true;
  }
  if (tagPageReadable(c213.dynLockPage)) {
    chip = c213;
    return true;
  }
  return false;
}

//Байты страницы в hex — для логов CC
static String pageHex(const uint8_t* p, uint8_t n) {
  String out;
  static const char hex[] = "0123456789ABCDEF";
  for (uint8_t i = 0; i < n; i++) {
    if (i) out += ' ';
    out += hex[(p[i] >> 4) & 0x0F];
    out += hex[p[i] & 0x0F];
  }
  return out;
}

//Один байт в hex — для логов lock-битов
static String byteHex(uint8_t v) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out += hex[(v >> 4) & 0x0F];
  out += hex[v & 0x0F];
  return out;
}

//D22: один диагностический лог на UID при первом отказе MIFARE_Read за касание.
//ATQA/SAK — опознаётся ли карта вообще; статусы проб 226/130/40 — виден ли тип чипа;
//первая ошибка чтения страницы и CC (стр. 3) — где рассыпается обмен.
//Карта к этому моменту в HALT: HALT -> WUPA (возвращает ATQA) -> SELECT, чтобы пробы и чтение
//CC шли на выбранной карте; в конце снова HALT.
static void nfcDiagLogOnce(const String& uid) {
  if (!uid.length() || !nfcDiag.triggered) return;  //отказов чтения не было — диагностика не нужна
  if (nfcDiagLoggedUID == uid) return;              //анти-спам: один лог на UID
  nfcDiagLoggedUID = uid;

  //D23: счётчик переселекций снимаем ДО собственных проб диагностики — в логе должны быть
  //восстановления пути чтения (tagPageReadable), а не те, что делает сама диагностика ниже.
  uint8_t reselectCnt = nfcDiag.reselectCnt;
  uint8_t reselectFail = nfcDiag.reselectFail;
  uint8_t reselectStatus = nfcDiag.reselectStatus;

  uint8_t sak = mfrc522.uid.sak;
  uint8_t atqa[2] = { 0, 0 };
  byte atqaSize = sizeof(atqa);
  mfrc522.PICC_HaltA();
  uint8_t atqaStatus = (uint8_t)mfrc522.PICC_WakeupA(atqa, &atqaSize);
  bool selected = atqaStatus == (uint8_t)MFRC522::StatusCode::STATUS_OK && mfrc522.PICC_ReadCardSerial();

  //В read-режиме тип чипа иначе не проверяется — пробы 226/130/40 дают ту же картину, что при записи
  if (selected && !nfcDiag.probeCnt) {
    TagChipInfo chip;
    tagChipInfo(chip);
  }

  uint8_t cc[18] = { 0 };
  byte ccSize = sizeof(cc);
  MFRC522::StatusCode ccRead = mfrc522.MIFARE_Read(3, cc, &ccSize);  //CC-байты стр. 3
  mfrc522.PICC_HaltA();

  String line = "NFC диагностика " + uid + ": ATQA=";
  if (atqaStatus == (uint8_t)MFRC522::StatusCode::STATUS_OK) {
    line += byteHex(atqa[0]);
    line += ' ';
    line += byteHex(atqa[1]);
  } else {
    line += "нет (status 0x";
    line += byteHex(atqaStatus);
    line += ')';
  }
  line += " SAK=0x";
  line += byteHex(sak);

  line += " | пробы:";
  if (!nfcDiag.probeCnt) {
    line += " нет (карта не выбрана)";
  } else {
    for (uint8_t i = 0; i < nfcDiag.probeCnt; i++) {
      line += ' ';
      line += String(nfcDiag.probePage[i]);
      line += "=0x";
      line += byteHex(nfcDiag.probeStatus[i]);
    }
  }

  line += " | первая ошибка чтения: ";
  if (nfcDiag.dataFailed) {
    line += "стр. ";
    line += String(nfcDiag.firstErrPage);
    line += " status=0x";
    line += byteHex(nfcDiag.firstErrStatus);
  } else {
    line += "нет (только пробы типа)";
  }

  line += " | переселекция: ";
  if (reselectCnt) {
    line += String(reselectCnt);
    if (reselectFail) {
      line += " (сбой ";
      line += String(reselectFail);
      line += ", WUPA status=0x";
      line += byteHex(reselectStatus);
      line += ')';
    }
  } else {
    line += "нет";
  }

  line += " | CC: ";
  if (ccRead == MFRC522::StatusCode::STATUS_OK) {
    line += pageHex(cc, 4);
    line += " (status=0x00)";
  } else {
    line += "нет (status 0x";
    line += byteHex((uint8_t)ccRead);
    line += ')';
  }

  Serial.println(line);
  logger.println(sets::Logger::warn() + line);
}

//D7: проверка запрета записи ДО любой записи в метку (статическая защита, динамическая, PROT, пароль, CC).
//D21: результат ТРЁХсоставный. «Чип не опознан» — это не read-only, а неизвестное состояние:
//вызывающий код не имеет права ни писать в чип, ни делать HTTP-эффекты (линковку/отвязку).
TagLockState tagLockCheck() {
  TagChipInfo chip;
  if (!tagChipInfo(chip)) {
    Serial.println("Метка: тип чипа не определён (не NTAG21x)");
    logger.println(sets::Logger::error() + "Метка: тип чипа не определён (не NTAG21x), состояние чипа неизвестно");
    return TAG_LOCK_CHIP_UNKNOWN;
  }

  //Статическая защита (стр. 2, байты 2-3): Lx запирает страницу x (3..15); CC — стр. 3 из того же чтения
  uint8_t buf[18];
  byte n = sizeof(buf);
  MFRC522::StatusCode lockRead = mfrc522.MIFARE_Read(2, buf, &n);
  if (lockRead != MFRC522::StatusCode::STATUS_OK) {
    nfcDiagNoteRead(2, (uint8_t)lockRead);
    //Без lock-битов и CC запись может испортить метку: состояние неизвестно (не read-only!)
    logger.println(sets::Logger::error() + "Метка " + String(chip.name) + ": не читается стр. 2 (lock/CC) — состояние чипа неизвестно");
    return TAG_LOCK_CHIP_UNKNOWN;
  }
  uint8_t lock0 = buf[2];
  uint8_t lock1 = buf[3];
  bool ccLocked = (lock0 & 0x08) != 0;                //L_CC — страница 3 (CC)
  bool staticLocked = (lock0 & 0xF0) || lock1;        //L4..L15 — страницы данных
  bool ccValid = buf[4] == 0xE1 && buf[5] == 0x10 && buf[6] == chip.ccSizeCode && buf[7] == 0x00;

  //Динамическая защита (стр. 40/130/226, байты 0-2): страницы 16h и далее
  bool dynLocked = false;
  n = sizeof(buf);
  MFRC522::StatusCode dynRead = mfrc522.MIFARE_Read(chip.dynLockPage, buf, &n);
  if (dynRead == MFRC522::StatusCode::STATUS_OK) {
    dynLocked = buf[0] || buf[1] || buf[2];
  } else {
    nfcDiagNoteRead(chip.dynLockPage, (uint8_t)dynRead);
  }

  //AUTH0 (байт 3 первой страницы конфигурации): FFh — пароль не используется
  uint8_t auth0 = 0xFF;
  n = sizeof(buf);
  bool cfgOk = mfrc522.MIFARE_Read(chip.cfgPage, buf, &n) == MFRC522::StatusCode::STATUS_OK;
  if (cfgOk) auth0 = buf[3];

  //PROT (ACCESS, байт 0, бит 7): 1 — чтение и запись только после PWD_AUTH
  bool prot = false;
  n = sizeof(buf);
  bool accessOk = mfrc522.MIFARE_Read(chip.accessPage, buf, &n) == MFRC522::StatusCode::STATUS_OK;
  if (accessOk) prot = (buf[0] & 0x80) != 0;

  bool passworded = auth0 <= chip.dataLastPage;  //защита начинается внутри области данных

  if (!cfgOk || !accessOk) {
    //Страницы конфигурации не читаются: пароль/PROT проверить нельзя. Молча писать не будем —
    //но и блокировать нельзя (запись в защищённую область всё равно вернёт NAK, порчи не будет).
    logger.println(sets::Logger::warn() + "Метка " + String(chip.name) + ": не читаются страницы конфигурации (AUTH0/PROT) — считаем защиту выключенной");
  }

  if (staticLocked || dynLocked) {
    logger.println(sets::Logger::warn() + "Метка " + String(chip.name) + " заперта (lock-биты " + byteHex(lock0) + " " + byteHex(lock1) + "), запись запрещена");
  } else if (prot) {
    logger.println(sets::Logger::warn() + "Метка " + String(chip.name) + ": PROT=1 (доступ под паролем), запись запрещена");
  } else if (passworded) {
    logger.println(sets::Logger::warn() + "Метка " + String(chip.name) + ": AUTH0=" + String(auth0) + " (парольная защита), запись запрещена");
  } else if (ccLocked && !ccValid) {
    logger.println(sets::Logger::warn() + "Метка " + String(chip.name) + ": CC заперт и не NDEF, запись запрещена");
  }

  bool blocked = staticLocked || dynLocked || prot || passworded || (ccLocked && !ccValid);
  return blocked ? TAG_LOCK_LOCKED : TAG_LOCK_WRITABLE;
}

//NDEF-сообщение для записи: TLV 0x03 (+ 0xFF len16 при длине > 254) + запись application/json
//(SR при payload <= 255 байт, иначе 4-байтная длина) + Terminator 0xFE + паддинг до страницы.
//Возвращает длину буфера (кратно 4) либо 0, если не влезает в cap.
static uint16_t buildNdefMessage(const String& payload, uint8_t* out, uint16_t cap) {
  uint16_t payloadLen = (uint16_t)payload.length();
  if (payloadLen == 0) return 0;

  bool shortRecord = payloadLen <= 255;
  uint16_t msgLen = 3 + (shortRecord ? 0 : 3) + NDEF_TYPE_JSON_LEN + payloadLen;
  bool longTlv = msgLen > 254;                  //0xFF — признак 16-битной длины TLV
  uint16_t total = (longTlv ? 4 : 2) + msgLen + 1;  //+ Terminator
  uint16_t padded = (uint16_t)((total + 3) & ~3);
  if (padded > cap) return 0;

  uint16_t ofs = 0;
  if (longTlv) {
    out[ofs++] = NDEF_TLV_NDEF;
    out[ofs++] = NDEF_TLV_LONG;
    out[ofs++] = (uint8_t)(msgLen >> 8);
    out[ofs++] = (uint8_t)(msgLen & 0xFF);
  } else {
    out[ofs++] = NDEF_TLV_NDEF;
    out[ofs++] = (uint8_t)msgLen;
  }

  //Заголовок записи: MB|ME|TNF=Media (0x02), SR — только если payload влезает в 1 байт длины
  out[ofs++] = (uint8_t)(0x80 | 0x40 | (shortRecord ? 0x10 : 0x00) | 0x02);
  out[ofs++] = (uint8_t)NDEF_TYPE_JSON_LEN;
  if (shortRecord) {
    out[ofs++] = (uint8_t)payloadLen;
  } else {
    out[ofs++] = (uint8_t)(payloadLen >> 24);
    out[ofs++] = (uint8_t)(payloadLen >> 16);
    out[ofs++] = (uint8_t)(payloadLen >> 8);
    out[ofs++] = (uint8_t)(payloadLen & 0xFF);
  }
  memcpy(out + ofs, NDEF_TYPE_JSON, NDEF_TYPE_JSON_LEN);
  ofs += NDEF_TYPE_JSON_LEN;
  for (uint16_t i = 0; i < payloadLen; i++) out[ofs++] = (uint8_t)payload[i];
  out[ofs++] = NDEF_TLV_END;
  while (ofs < padded) out[ofs++] = 0x00;
  return padded;
}

//D7/Review Focus 5: проверка CC (стр. 3: E1 10 <size/8> 00) + TLV + NDEF-запись по страницам + Terminator.
//Подтверждение — перечитывание: ndefPayload() должен вернуть ровно записанное.
bool writeNDEF(const String& payload) {
  if (!payload.length()) {
    logger.println(sets::Logger::error() + "Запись метки: пустой payload");
    return false;
  }

  TagChipInfo chip;
  if (!tagChipInfo(chip)) {
    logger.println(sets::Logger::error() + "Запись метки: тип чипа не определён (не NTAG21x)");
    return false;
  }

  //1) CC: без него телефон не увидит NDEF. Нестандартный CC переписываем и логируем
  uint8_t cc[18];
  byte n = sizeof(cc);
  if (mfrc522.MIFARE_Read(3, cc, &n) != MFRC522::StatusCode::STATUS_OK) {
    logger.println(sets::Logger::error() + "Запись метки " + String(chip.name) + ": не читается CC (стр. 3)");
    return false;
  }
  if (cc[0] != 0xE1 || cc[1] != 0x10 || cc[2] != chip.ccSizeCode || cc[3] != 0x00) {
    logger.println(sets::Logger::warn() + "CC метки " + String(chip.name) + " нестандартен (" + pageHex(cc, 4) + ") — переписываем на E1 10 " + pageHex(&chip.ccSizeCode, 1) + " 00");
    uint8_t fixed[4] = { 0xE1, 0x10, chip.ccSizeCode, 0x00 };
    if (mfrc522.MIFARE_Ultralight_Write(3, fixed, 4) != MFRC522::StatusCode::STATUS_OK) {
      logger.println(sets::Logger::error() + "Запись метки: не удалось переписать CC (стр. 3)");
      return false;
    }
  }

  //2) NDEF-сообщение: TLV + запись + Terminator, паддинг до границы страницы
  static uint8_t msg[NDEF_MAX_BYTES + 16];  //вне стека: до 904 байт
  uint16_t len = buildNdefMessage(payload, msg, chip.ccSize);
  if (!len) {
    logger.println(sets::Logger::error() + "Запись метки: payload " + String(payload.length()) + " байт не влезает в " + String(chip.name) + " (CC " + String(chip.ccSize) + " байт)");
    return false;
  }

  //3) Постраничная запись 4-байтными страницами
  uint16_t pages = (uint16_t)(len / 4);
  for (uint16_t i = 0; i < pages; i++) {
    uint16_t page = (uint16_t)(NDEF_PAGE_START + i);
    if (page > chip.dataLastPage) {
      logger.println(sets::Logger::error() + "Запись метки: выход за границу данных " + String(chip.name) + " (стр. " + String(page) + ")");
      return false;
    }
    if (mfrc522.MIFARE_Ultralight_Write((byte)page, &msg[i * 4], 4) != MFRC522::StatusCode::STATUS_OK) {
      logger.println(sets::Logger::error() + "Запись метки: ошибка записи страницы " + String(page));
      return false;
    }
  }

  //4) Подтверждение перечитыванием: TLV-парсер должен вернуть ровно записанное
  String check = ndefPayload();
  if (check != payload) {
    logger.println(sets::Logger::error() + "Запись метки: проверка чтением не совпала (записано " + String(payload.length()) + ", прочитано " + String(check.length()) + " байт)");
    return false;
  }
  return true;
}

//D8/D13: POST /api/v1/spool/{id}/tag {"uid":...,"format":"ntag"}.
//true — UID привязан. false: holderID — "<id>" спула-владельца (409, возможна перелинковка),
//"F<id>" — владелец филамент (через API спулов не перецепляется), "" — иная ошибка (залогирована).
bool linkTagUID(const String& uid, int spoolID, String& holderID) {
  holderID = "";
  if (!uid.length() || spoolID <= 0) return false;
  if (WiFi.status() != WL_CONNECTED) {
    logger.println(sets::Logger::error() + "Линковка метки " + uid + ": нет WiFi");
    return false;
  }

  HTTPClient http;
  String url = "http://" + String(db[web::serverSpoolman]) + "/api/v1/spool/" + String(spoolID) + "/tag";
  http.begin(url);
  http.setTimeout(1500);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");

  JsonDocument request;
  request["uid"] = uid;
  request["format"] = "ntag";
  String body;
  serializeJson(request, body);

  int code = http.POST(body);
  String response;
  if (code > 0) response = http.getString();
  http.end();

  if (code == 200 || code == 201) return true;

  if (code == 409) {
    //Тело 409: {"message":..., "spool_id":X|null, "filament_id":Y|null} — владелец метки
    JsonDocument doc;
    if (!deserializeJson(doc, response)) {
      int holder = doc["spool_id"] | -1;
      if (holder > 0) {
        holderID = String(holder);
      } else if (!doc["filament_id"].isNull()) {
        holderID = "F" + String((int)(doc["filament_id"] | 0));
      }
    }
    Serial.println("Линковка метки " + uid + ": HTTP 409, владелец " + holderID);
    logger.println(sets::Logger::warn() + "Линковка метки " + uid + ": UID уже привязан (" + holderID + ")");
    return false;
  }

  Serial.println("Линковка метки " + uid + ": HTTP " + String(code));
  if (code <= 0) {
    errorServer(String(code));
    lastSpoolmanFail = millis();
  } else {
    logger.println(sets::Logger::error() + "Линковка метки " + uid + ": HTTP " + String(code));
  }
  return false;
}

//DELETE /api/v1/spool/{id}/tag/{uid} — код ответа HTTP (204/200 — снято, 404 — такой линковки нет,
//<=0 — сеть/таймаут). Вынесено из unlinkTagUID(), чтобы стирание (D9) отличало 404 («не линкована,
//не ошибка») от реального сбоя и могло проверить линковку на филамент.
static int unlinkTagRequest(const String& uid, int spoolID) {
  if (!uid.length() || spoolID <= 0) return -1;
  if (WiFi.status() != WL_CONNECTED) {
    logger.println(sets::Logger::error() + "Отвязка метки " + uid + ": нет WiFi");
    return -1;
  }

  HTTPClient http;
  String url = "http://" + String(db[web::serverSpoolman]) + "/api/v1/spool/" + String(spoolID) + "/tag/" + uid;
  http.begin(url);
  http.setTimeout(1500);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  int code = http.sendRequest("DELETE");  //в HTTPClient ESP32 нет метода DELETE()
  http.end();

  if (code <= 0) {
    errorServer(String(code));
    lastSpoolmanFail = millis();
  }
  return code;
}

//D13/D9: DELETE /api/v1/spool/{id}/tag/{uid}. 204 — отвязано; 404 — такой линковки нет.
bool unlinkTagUID(const String& uid, int spoolID) {
  int code = unlinkTagRequest(uid, spoolID);
  if (code == 200 || code == 204) return true;
  if (code == 404) {
    logger.println(sets::Logger::warn() + "Отвязка метки " + uid + ": линковки на ID:" + String(spoolID) + " нет (404)");
    return false;
  }
  Serial.println("Отвязка метки " + uid + ": HTTP " + String(code));
  if (code > 0) logger.println(sets::Logger::error() + "Отвязка метки " + uid + ": HTTP " + String(code));
  return false;
}

//Число из JSON метки/ответа Spoolman: целое либо дробное (в API встречаются оба вида).
//ArduinoJson::is<int>() не считает 240.0 целым, поэтому читаем и float.
static int jsonInt(JsonVariantConst v, int def) {
  if (v.is<int>()) return v.as<int>();
  if (v.is<float>()) return (int)(v.as<float>() + 0.5f);
  return def;
}

//D6: данные катушки из Spoolman (GET /api/v1/spool/{id}) для сборки openspool-JSON
bool readSpoolForTag(int spoolID, String& material, String& brand, String& colorHex, int& extruderTemp, int& bedTemp, String& subtype) {
  material = "";
  brand = "";
  colorHex = "";
  subtype = "";
  extruderTemp = 0;
  bedTemp = 0;
  if (spoolID <= 0) return false;
  if (WiFi.status() != WL_CONNECTED) {
    logger.println(sets::Logger::error() + "Запись метки: нет WiFi (данные катушки не получены)");
    return false;
  }

  HTTPClient http;
  String url = "http://" + String(db[web::serverSpoolman]) + "/api/v1/spool/" + String(spoolID);
  http.begin(url);
  http.setTimeout(1500);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  int code = http.GET();
  String response;
  if (code > 0) response = http.getString();
  http.end();

  if (code != 200) {
    Serial.println("Катушка ID:" + String(spoolID) + ": HTTP " + String(code));
    logger.println(sets::Logger::error() + "Запись метки: катушка ID:" + String(spoolID) + " не получена (HTTP " + String(code) + ")");
    if (code <= 0) lastSpoolmanFail = millis();
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    errorServer(error.f_str());
    return false;
  }
  material = doc["filament"]["material"] | "";
  brand = doc["filament"]["vendor"]["name"] | "";
  colorHex = doc["filament"]["color_hex"] | "";
  extruderTemp = jsonInt(doc["filament"]["settings_extruder_temp"], 0);
  bedTemp = jsonInt(doc["filament"]["settings_bed_temp"], 0);
  subtype = extraText(doc["filament"]["extra"]["subtype"]);
  //D44: extra.subtype хранит значение в кавычках-обёртке формата SpoolPainter — не данные.
  //Снимаем только парные кавычки по краям; одиночная кавычка остаётся частью значения.
  {
    int b = 0, e = subtype.length();
    while (b < e && subtype[b] == ' ') b++;
    while (e > b && subtype[e - 1] == ' ') e--;
    if (e - b >= 2 && subtype[b] == '"' && subtype[e - 1] == '"') {
      subtype = subtype.substring(b + 1, e - 1);
      subtype.trim();
    }
  }
  return true;
}

//D43/D44: openspool-JSON всегда собирается из данных Spoolman (вендорские метки больше не читаются).
//Значения — строки, как в эталонной метке Spool Painter (nfc_spoolpainter.md: "min_temp":"220",
//"spool_id":"37"); bed_min_temp/bed_max_temp — ключи расширения openspool, которыми пишет
//Spool Painter (в D6 они названы bed_min/bed_max). subtype берётся из filament.extra.subtype
//Spoolman (D44), без значения — Basic (пустой subtype в метку не пишем).
static String buildOpenspoolJSON(int spoolID, const String& material, const String& brand, const String& colorHex, int extruderTemp, int bedTemp, const String& subtype) {
  JsonDocument doc;
  doc["protocol"] = "openspool";
  doc["version"] = "1.0";
  if (material.length()) doc["type"] = material;
  if (colorHex.length()) doc["color_hex"] = colorHex;
  if (brand.length()) doc["brand"] = brand;
  if (extruderTemp > 0) {
    doc["min_temp"] = String(extruderTemp);
    doc["max_temp"] = String(extruderTemp);
  } else {
    logger.println(sets::Logger::warn() + "Запись метки: settings_extruder_temp не задан — температуры сопла не пишем");
  }
  if (bedTemp > 0) {
    doc["bed_min_temp"] = String(bedTemp);
    doc["bed_max_temp"] = String(bedTemp);
  } else {
    logger.println(sets::Logger::warn() + "Запись метки: settings_bed_temp не задан — температуры стола не пишем");
  }
  doc["spool_id"] = String(spoolID);
  doc["subtype"] = subtype.length() ? subtype : "Basic";

  String out;
  serializeJson(doc, out);
  return out;
}

//D43: формат прежнего содержимого метки — только для журнала записи («метка вендора …»,
//«метка openspool …»); на сборку payload больше не влияет — запись всегда полными данными
//из Spoolman (readSpoolForTag + buildOpenspoolJSON).
static int tagPayloadFormat(JsonDocument& doc, bool parsed) {
  if (!parsed) return 0;
  if (doc["protocol"].is<const char*>()) return 1;
  if (jsonHasID(doc, "sm_id")) return 2;
  if (doc["type"].is<const char*>() || jsonHasID(doc, "spool_id")) return 1;
  return 0;
}

//Цвет результата записи/стирания — эффект D31 (setWriteResult: PURPLE 2 с / мигание RED 2 с).
//Определение — в секции «D31: семантика светодиода NFC-операций» выше.

//=== D29: card_uids в spool.extra (формат SpoolPainter) ===

//D38: найти все катушки, у которых в extra.card_uids есть этот UID (формат SpoolPainter,
//значение может прийти с кавычками-обёрткой и без — разбор через кодек carduids.h).
//GET /api/v1/spool?limit=1000 (как в updateMmuGates), фильтр полей: id + extra.card_uids.
//Возвращает число найденных id. Ошибка сети/HTTP/разбора — 0 и запись в Serial+логгер:
//очистка card_uids при стирании не должна прерывать сам стирание (симметрия D29).
#define SPOOLS_BY_CARD_UID_MAX 16  //достаточно с запасом: UID держат единицы катушек
static int spoolsByCardUid(const String& uid, int* outIds, int maxIds) {
  if (outIds && maxIds > 0) outIds[0] = 0;
  int found = 0;
  if (!uid.length() || !outIds || maxIds <= 0) return 0;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("card_uids: нет WiFi (поиск катушек по UID " + uid + ")");
    return 0;
  }

  HTTPClient http;
  String url = "http://" + String(db[web::serverSpoolman]) + "/api/v1/spool?limit=1000";
  http.begin(url);
  http.setTimeout(BULK_HTTP_TIMEOUT_MS);  //D41: полный список идёт дольше 2 с — из-за 2 с bulk-GET списки обрывались
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  uint32_t tGet = millis();
  int code = http.GET();
  if (code != 200) {
    http.end();
    uint32_t dt = millis() - tGet;
    Serial.println("card_uids: список катушек -> HTTP " + String(code) + " за " + String(dt) + " мс");
    logger.println(sets::Logger::warn() + "card_uids: список катушек не получен (HTTP " + String(code) + " за " + String(dt) + " мс), поиск по UID " + uid + " не выполнен");
    if (code <= 0) lastSpoolmanFail = millis();
    return 0;
  }

  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["extra"]["card_uids"] = true;
  JsonDocument doc;
  int bytes = http.getSize();  //D41: размер ответа; chunked у ESP32-клиента может дать -1
  DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    Serial.println("card_uids: список катушек: ошибка разбора " + String(error.c_str()) + " (байт: " + String(bytes) + ")");
    logger.println(sets::Logger::warn() + "card_uids: список катушек не разобран (" + String(error.c_str()) + ", байт: " + String(bytes) + "), поиск по UID " + uid + " не выполнен");
    return 0;
  }

  char norm[carduids::kUidLen];
  if (!carduids::normalizeUid(uid.c_str(), norm, sizeof(norm))) return 0;

  int spools = 0;  //D41: фактический объём списка после фильтра
  for (JsonObject sp : doc.as<JsonArray>()) {
    spools++;
    int id = spoolId(sp);
    if (id <= 0) continue;
    String raw = extraText(sp["extra"]["card_uids"]);
    if (!raw.length()) continue;
    carduids::List list;
    if (!carduids::parse(raw.c_str(), list)) continue;  //чужой/битый формат — не трогаем
    for (int i = 0; i < list.count; i++) {
      if (!carduids::equalsNoCase(list.uid[i], norm)) continue;
      bool dup = false;  //одна катушка — один результат: защита от повторных id в ответе API
      for (int j = 0; j < found; j++) {
        if (outIds[j] == id) { dup = true; break; }
      }
      if (!dup && found < maxIds) outIds[found++] = id;
      break;  //UID в поле этой катушки найден, дальше токены не смотрим
    }
  }
  Serial.println("card_uids: список катушек " + String(spools) + " шт., владельцы UID " + uid + ": " + String(found));
  return found;
}

//GET /api/v1/spool/{id} -> PATCH /api/v1/spool/{id} {"extra": {...}} с мержем extra.
//add=true — дописать UID в card_uids (без дублей), false — убрать (стирание/отвязка).
//Формат значения — как у SpoolPainter: кавычки ВНУТРИ значения, hex верхним регистром
//(кодек — card_uids.h, host-тест workflows/<task>/host-tests/test_card_uids.cpp).
//Любая ошибка (сеть/HTTP/формат) — только Serial+логгер: линковка UID в Spoolman и запись
//метки от этого не зависят (D29) и не прерываются.
static bool cardUidsUpdate(int spoolID, const String& uid, bool add) {
  if (spoolID <= 0 || !uid.length()) return false;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("card_uids: нет WiFi (катушка ID:" + String(spoolID) + ", UID " + uid + ")");
    return false;
  }

  String url = "http://" + String(db[web::serverSpoolman]) + "/api/v1/spool/" + String(spoolID);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(1500);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  //Фильтр: чужие поля катушки не нужны — PATCH всё равно шлём мержем только extra
  JsonDocument filter;
  filter["extra"] = true;
  int code = http.GET();
  if (code != 200) {
    http.end();
    Serial.println("card_uids: катушка ID:" + String(spoolID) + " -> HTTP " + String(code));
    logger.println(sets::Logger::warn() + "card_uids: катушка ID:" + String(spoolID) + " не прочитана (HTTP " + String(code) + "), поле не изменено");
    return false;
  }
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    Serial.println("card_uids: катушка ID:" + String(spoolID) + ": ошибка разбора " + String(error.c_str()));
    logger.println(sets::Logger::warn() + "card_uids: катушка ID:" + String(spoolID) + ": ответ не разобран, поле не изменено");
    return false;
  }

  String raw = extraText(doc["extra"]["card_uids"]);
  static char newValue[carduids::kValueMax];  //вне стека loop(): до ~800 байт
  int st = add ? carduids::add(raw.c_str(), uid.c_str(), newValue, sizeof(newValue))
               : carduids::drop(raw.c_str(), uid.c_str(), newValue, sizeof(newValue));
  if (st == carduids::REFUSED) {
    Serial.println("card_uids: поле катушки ID:" + String(spoolID) + " не разобрано/переполнено — не меняю");
    logger.println(sets::Logger::warn() + "card_uids: катушка ID:" + String(spoolID) + ": формат поля не позволяет правку, значение оставлено как есть");
    return false;
  }
  if (st == carduids::UNCHANGED) {
    Serial.println("card_uids: UID " + uid + (add ? " уже в поле" : " отсутствует в поле") + " катушки ID:" + String(spoolID));
    return true;
  }

  //PATCH: extra целиком (чужие ключи extra сохраняем как пришли) + новое card_uids
  JsonDocument patch;
  JsonObject extra = patch["extra"].to<JsonObject>();
  for (JsonPairConst kv : doc["extra"].as<JsonObjectConst>()) extra[kv.key()] = kv.value();
  extra["card_uids"] = newValue;
  String body;
  serializeJson(patch, body);

  //Отдельный клиент для PATCH: переиспользование после end() в ESP32-ядре не гарантировано
  HTTPClient httpPatch;
  httpPatch.begin(url);
  httpPatch.setTimeout(1500);
  httpPatch.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpPatch.addHeader("Content-Type", "application/json");
  int patchCode = httpPatch.PATCH(body);  //в HTTPClient ESP32 PATCH есть отдельным методом
  httpPatch.end();

  if (patchCode < 200 || patchCode >= 300) {
    Serial.println("card_uids: PATCH катушки ID:" + String(spoolID) + " -> HTTP " + String(patchCode));
    logger.println(sets::Logger::warn() + "card_uids: катушка ID:" + String(spoolID) + ": поле не сохранено (HTTP " + String(patchCode) + "), линковка UID не отменена");
    return false;
  }
  Serial.println("card_uids: катушка ID:" + String(spoolID) + " -> " + String(newValue));
  logger.println("card_uids: катушка ID:" + String(spoolID) + " -> " + String(newValue));
  return true;
}

//D39: после добавления UID новому владельцу снять его со «старых» — фактические
//владельцы поля (spoolsByCardUid) минус новый владелец (планировщик planCardUidUpdates
//в card_uids.h, host-тест test_card_uid_updates.cpp). Ошибки отдельных снятий не
//прерывают остальные и не влияют на результат линковки (симметрия D29/D41).
static void cardUidsUnlinkStale(const String& uid, int newOwner) {
  if (newOwner <= 0 || !uid.length()) return;
  int holders[SPOOLS_BY_CARD_UID_MAX];
  int found = spoolsByCardUid(uid, holders, SPOOLS_BY_CARD_UID_MAX);
  carduids::UnlinkPlan plan = carduids::planCardUidUpdates(newOwner, holders, found);
  if (!plan.count) return;
  for (int i = 0; i < plan.count; i++) {
    Serial.println("card_uids: снятие UID " + uid + " со старой катушки ID:" + String(plan.ids[i]));
    if (!cardUidsUpdate(plan.ids[i], uid, false)) {
      Serial.println("card_uids: снятие с ID:" + String(plan.ids[i]) + " не выполнено — останется в поле");
    }
  }
}

//D8/D13: линковка с двухшаговым подтверждением конфликта. true — UID привязан к spoolID.
static bool linkWithConflict(const String& uid, int spoolID) {
  String holder;
  if (linkTagUID(uid, spoolID, holder)) {
    conflictUID = "";
    conflictSpool = 0;
    Serial.println("Метка " + uid + " привязана к катушке ID:" + String(spoolID));
    logger.println("Метка " + uid + " привязана к катушке ID:" + String(spoolID));
    //D29: UID в spool.extra.card_uids (ошибка — только лог)
    cardUidsUpdate(spoolID, uid, true);
    //D39: UID мог остаться у старых катушек (переиспользованная метка, ручные правки
    //в Spoolman) — снимаем со всех фактических владельцев, кроме нового
    cardUidsUnlinkStale(uid, spoolID);
    return true;
  }
  if (!holder.length()) return false;  //не 409 либо владелец без id — уже залогировано

  if (holder[0] == 'F') {
    logger.println(sets::Logger::error() + "Метка " + uid + " привязана к филаменту ID:" + holder.substring(1) + " — перелинковка вручную");
    return false;
  }

  int holderSpool = holder.toInt();
  if (conflictUID != uid) {
    //Первое нажатие: только сообщаем владельца, ничего не удаляем
    conflictUID = uid;
    conflictSpool = holderSpool;
    Serial.println("Метка " + uid + " уже привязана к ID:" + String(holderSpool));
    logger.println(sets::Logger::warn() + "UID уже привязан к ID " + String(holderSpool) + ", нажмите Записать ещё раз для перелинковки");
    return false;
  }

  //Второе нажатие на том же UID — подтверждённая перелинковка: DELETE у старого владельца + POST
  if (!unlinkTagUID(uid, holderSpool)) {
    logger.println(sets::Logger::error() + "Перелинковка " + String(holderSpool) + "→" + String(spoolID) + " не выполнена: отвязка не удалась");
    return false;
  }
  String holder2;
  if (!linkTagUID(uid, spoolID, holder2)) {
    logger.println(sets::Logger::error() + "Перелинковка " + String(holderSpool) + "→" + String(spoolID) + " не выполнена: повторная привязка не удалась");
    return false;
  }
  conflictUID = "";
  conflictSpool = 0;
  Serial.println("Метка " + uid + " переехала " + String(holderSpool) + "→" + String(spoolID));
  logger.println("метка переехала " + String(holderSpool) + "→" + String(spoolID));
  //D29: симметрия перелинковки — UID уходит из card_uids старой катушки и появляется у новой
  cardUidsUpdate(holderSpool, uid, false);
  cardUidsUpdate(spoolID, uid, true);
  //D39: перелинковка — проверяем и остальных: UID мог числиться ещё где-то
  //(дубли в card_uids от ручных правок Spoolman или прошлых сбоев)
  cardUidsUnlinkStale(uid, spoolID);
  return true;
}

//Step 2 плана: машина записи по касанию метки в WAIT_TAG (D5–D8, D13–D16).
//Порядок: payload (D5/D6/D15) -> lock (D7; D14 — только линковка) -> запись -> подтверждение -> линковка.
static void tagWriteFlow(const String& uid, const String& payloadIn, JsonDocument& tagDoc, bool parsed) {
  int spoolID = selectedSpoolID();
  if (spoolID <= 0) {
    Serial.println("Запись метки: катушка не выбрана");
    logger.println(sets::Logger::error() + "Запись метки: катушка не выбрана (обновите список катушек)");
    setWriteResult(false);
    return;
  }
  logger.println("Запись метки " + uid + " -> катушка ID:" + String(spoolID));

  //1) D7/D21: сначала lock-проверка (как в спецификации: UID + lock -> потом payload).
  //D21: UID-линковка возможна ТОЛЬКО после подтверждённой записи NDEF, поэтому любой отказ
  //lock-проверки завершает операцию до HTTP (readSpoolForTag/linkWithConflict не вызываются).
  TagLockState lock = tagLockCheck();
  if (lock == TAG_LOCK_CHIP_UNKNOWN) {
    //«Чип не опознан» — состояние неизвестно: ни записи, ни привязки (D21)
    Serial.println("Запись метки " + uid + ": чип не опознан, запись не выполнена, UID не привязан");
    logger.println(sets::Logger::error() + "Запись метки " + uid + ": чип не опознан — запись не выполнена, UID не привязан (катушка ID:" + String(spoolID) + " не тронута)");
    setWriteResult(false);
    return;
  }
  if (lock == TAG_LOCK_LOCKED) {
    //D21: содержимое не записано — значит и UID не линкуем (привязка без записи запрещена)
    Serial.println("Запись метки " + uid + ": метка read-only, запись не выполнена, UID не привязан");
    logger.println(sets::Logger::warn() + "Запись метки " + uid + ": метка read-only — содержимое не записано, UID не привязан к ID:" + String(spoolID));
    setWriteResult(false);
    return;  //поля w_* уже отражают фактическое содержимое метки (её не меняли)
  }

  //2) D43: payload всегда собирается заново из Spoolman — режим «вендорской метки»
  //(дописка spool_id в существующий JSON) убран; fmt — только для журнала.
  int fmt = tagPayloadFormat(tagDoc, parsed);
  String material, brand, colorHex, subtype;
  int extruderTemp = 0, bedTemp = 0;
  if (!readSpoolForTag(spoolID, material, brand, colorHex, extruderTemp, bedTemp, subtype)) {
    setWriteResult(false);
    return;
  }
  String payloadOut = buildOpenspoolJSON(spoolID, material, brand, colorHex, extruderTemp, bedTemp, subtype);
  if (fmt == 2) {
    Serial.println("FilaMan-метка: полная перезапись из Spoolman");
    logger.println("FilaMan-метка (sm_id): перезаписана данными катушки ID:" + String(spoolID) + " из Spoolman (было: FilaMan sm_id)");
  } else if (fmt == 1) {
    Serial.println("Метка вендора/openspool: полная перезапись из Spoolman");
    logger.println("Метка вендора/openspool: перезаписана данными катушки ID:" + String(spoolID) + " из Spoolman (было: вендорский/openspool JSON)");
  } else {
    Serial.println("Метка пустая/нераспознана: openspool JSON собран из Spoolman");
    logger.println("Метка пустая/нераспознана: openspool JSON собран из Spoolman ID:" + String(spoolID));
    if (payloadIn.length()) logger.println(sets::Logger::warn() + "Содержимое метки не распознано и будет перезаписано: " + payloadIn);
  }

  //3) Запись + подтверждение перечитыванием
  if (!writeNDEF(payloadOut)) {
    Serial.println("Запись метки " + uid + " не удалась");
    logger.println(sets::Logger::error() + "Запись метки " + uid + " не удалась, причина в логах выше");
    setWriteResult(false);
    return;
  }
  Serial.println("Метка " + uid + " записана: " + String(payloadOut.length()) + " байт");
  logger.println("Метка " + uid + " записана: " + String(payloadOut.length()) + " байт");

  //4) D8: авто-линковка; ошибка линковки не отменяет запись
  bool linked = linkWithConflict(uid, spoolID);

  //Поля показывают то, что теперь лежит на метке
  JsonDocument post;
  if (parseTagJSON(payloadOut, post)) {
    int tagID = tagSpoolID(post);
    db[web::w_id] = tagID > 0 ? tagID : spoolID;
    db[web::w_type] = post["type"] | "";
    db[web::w_brand] = post["brand"] | "";
    db[web::w_color] = post["color_hex"] | "";  //D28: сватч меню записи показывает цвет с метки
  } else {
    db[web::w_id] = spoolID;
  }

  //D31: запись удалась и UID привязан — PURPLE 2 с; UID не привязан — мигание RED 2 с
  //(для владельца операция не завершена, причина уже в логе линковки)
  setWriteResult(linked);
}

//=== Стирание метки v1.3: zero-fill + пустой NDEF + отвязка UID (D9/D14) ===

//GET /api/v1/spool?tag=<uid> — владелец линковки UID в Spoolman (D9: сначала спул, потом филамент).
//>0 — id катушки, 0 — линковки нет, -1 — запрос не выполнен (ошибка залогирована).
static int ownerSpoolByTag(const String& uid) {
  if (!uid.length()) return 0;
  if (WiFi.status() != WL_CONNECTED) {
    logger.println(sets::Logger::error() + "Стирание метки " + uid + ": нет WiFi (линковка не проверена)");
    return -1;
  }

  HTTPClient http;
  String url = "http://" + String(db[web::serverSpoolman]) + "/api/v1/spool?tag=" + uid;
  http.begin(url);
  http.setTimeout(1500);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != 200) {
    http.end();
    Serial.println("Поиск владельца UID " + uid + ": HTTP " + String(code));
    logger.println(sets::Logger::error() + "Стирание метки " + uid + ": поиск владельца не выполнен (HTTP " + String(code) + ")");
    if (code <= 0) {
      errorServer(String(code));
      lastSpoolmanFail = millis();
    }
    return -1;
  }

  //Ответ — массив катушек с этим тегом (обычно 0 или 1): берём id первой
  JsonDocument filter;
  filter[0]["id"] = true;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    logger.println(sets::Logger::error() + "Стирание метки " + uid + ": ошибка разбора ответа поиска владельца (" + String(error.c_str()) + ")");
    return -1;
  }
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull() || arr.size() == 0) return 0;
  int owner = arr[0]["id"] | -1;
  return owner > 0 ? owner : 0;
}

//GET /api/v1/filament?tag=<uid> — метка линкована на филамент, а не на катушку.
//Через API спулов такую линковку не снять (design.md, риск «метка на филаменте») — только лог.
static bool tagLinkedToFilament(const String& uid) {
  if (!uid.length() || WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = "http://" + String(db[web::serverSpoolman]) + "/api/v1/filament?tag=" + uid;
  http.begin(url);
  http.setTimeout(1500);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != 200) {
    http.end();
    if (code <= 0) {
      errorServer(String(code));
      lastSpoolmanFail = millis();
    }
    return false;
  }

  JsonDocument filter;
  filter[0]["id"] = true;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error) return false;
  JsonArray arr = doc.as<JsonArray>();
  return !arr.isNull() && arr.size() > 0;
}

//D9: zero-fill страниц данных 4..(последняя перед динамическими lock-байтами — 129 у NTAG215, 225 у
//NTAG216, 39 у NTAG213) и пустой NDEF «03 00 FE» в странице 4. CC (стр. 3) в область данных не входит,
//но без валидного CC телефон не увидит даже пустой NDEF — нестандартный CC приводим к E1 10 <size/8> 00.
static bool wipeTagChip(const TagChipInfo& chip) {
  uint8_t cc[18];
  byte n = sizeof(cc);
  if (mfrc522.MIFARE_Read(3, cc, &n) != MFRC522::StatusCode::STATUS_OK) {
    logger.println(sets::Logger::error() + "Стирание метки " + String(chip.name) + ": не читается CC (стр. 3)");
    return false;
  }
  if (cc[0] != 0xE1 || cc[1] != 0x10 || cc[2] != chip.ccSizeCode || cc[3] != 0x00) {
    logger.println(sets::Logger::warn() + "CC метки " + String(chip.name) + " нестандартен (" + pageHex(cc, 4) + ") — переписываем на E1 10 " + pageHex(&chip.ccSizeCode, 1) + " 00");
    uint8_t fixed[4] = { 0xE1, 0x10, chip.ccSizeCode, 0x00 };
    if (mfrc522.MIFARE_Ultralight_Write(3, fixed, 4) != MFRC522::StatusCode::STATUS_OK) {
      logger.println(sets::Logger::error() + "Стирание метки: не удалось переписать CC (стр. 3)");
      return false;
    }
  }

  //Zero-fill всей области данных, включая страницу 4 (её перезапишем пустым NDEF ниже)
  uint8_t zero[4] = { 0x00, 0x00, 0x00, 0x00 };
  uint8_t lastDataPage = (uint8_t)(chip.dynLockPage - 1);
  for (uint16_t page = NDEF_PAGE_START; page <= lastDataPage; page++) {
    if (mfrc522.MIFARE_Ultralight_Write((byte)page, zero, 4) != MFRC522::StatusCode::STATUS_OK) {
      logger.println(sets::Logger::error() + "Стирание метки: ошибка записи страницы " + String(page));
      return false;
    }
  }

  //D9: пустой NDEF — TLV 0x03 с нулевой длиной и терминатор 0xFE в первой странице области NDEF
  uint8_t empty[4] = { NDEF_TLV_NDEF, 0x00, NDEF_TLV_END, 0x00 };
  if (mfrc522.MIFARE_Ultralight_Write(NDEF_PAGE_START, empty, 4) != MFRC522::StatusCode::STATUS_OK) {
    logger.println(sets::Logger::error() + "Стирание метки: не удалось записать пустой NDEF (стр. " + String(NDEF_PAGE_START) + ")");
    return false;
  }

  //Подтверждение: страница 4 == 03 00 FE, и NDEF-парсер больше не находит payload
  uint8_t check[18];
  n = sizeof(check);
  if (mfrc522.MIFARE_Read(NDEF_PAGE_START, check, &n) != MFRC522::StatusCode::STATUS_OK) {
    logger.println(sets::Logger::error() + "Стирание метки: не читается стр. " + String(NDEF_PAGE_START) + " после записи");
    return false;
  }
  if (check[0] != NDEF_TLV_NDEF || check[1] != 0x00 || check[2] != NDEF_TLV_END) {
    logger.println(sets::Logger::error() + "Стирание метки: проверка пустого NDEF не совпала (" + pageHex(check, 4) + ")");
    return false;
  }
  if (ndefPayload().length()) {
    logger.println(sets::Logger::error() + "Стирание метки: NDEF-парсер всё ещё находит payload после зачистки");
    return false;
  }
  return true;
}

//D9/D14: стереть метку. Вызывается по касанию в WAIT_TAG — UID уже прочитан в readNFC().
//Порядок: lock (D7; read-only -> чип не трогаем, D14) -> zero-fill + пустой NDEF -> подтверждение
//-> owner-lookup -> DELETE /spool/{id}/tag/{uid} (404 — не линкована, не ошибка).
//true — цель достигнута: чип очищен, либо чип read-only и линковка снята.
bool eraseTag() {
  String uid = formatUID();
  if (!uid.length()) {
    logger.println(sets::Logger::error() + "Стирание метки: UID не прочитан");
    return false;
  }
  Serial.println("Стирание метки: UID " + uid);
  logger.println("Стирание метки " + uid);

  //1) D7/D14/D21: трёхсоставный lock-результат.
  //CHIP_UNKNOWN -> полный abort: ни wipe, ни отвязки (состояние чипа неизвестно, D21).
  //LOCKED (read-only) -> wipe невозможен, отвязка UID выполняется с честным логом (D14).
  TagLockState lock = tagLockCheck();
  if (lock == TAG_LOCK_CHIP_UNKNOWN) {
    Serial.println("Стирание метки " + uid + ": чип не опознан, стирание и отвязка отменены");
    logger.println(sets::Logger::error() + "Стирание метки " + uid + ": чип не опознан — содержимое не стёрто, UID не отвязан");
    return false;
  }
  bool chipWiped = false;
  if (lock == TAG_LOCK_LOCKED) {
    logger.println(sets::Logger::warn() + "Стирание метки " + uid + ": чип read-only (lock-биты), wipe невозможен — отвязываем UID");
  } else {
    TagChipInfo chip;
    tagChipInfo(chip);
    chipWiped = wipeTagChip(chip);
    if (!chipWiped) {
      //Метку не очистили — линковку не снимаем: иначе UID-first останется без содержимого,
      //а старая катушка в Spoolman «потеряет» метку при живом spool_id на чипе.
      logger.println(sets::Logger::error() + "Стирание метки " + uid + " не удалось: чип не очищен, линковка не снята");
      return false;
    }
    logger.println("Стирание метки " + uid + ": чип очищен, NDEF пуст (стр. " + String(NDEF_PAGE_START) + " = 03 00 FE)");
  }

  //2) D9: отвязка UID — владелец по тегу, затем DELETE. 404 = линковки нет (не ошибка).
  bool done = true;
  bool unlinkDone = false;   //D38: 200/204 — отвязка выполнена
  bool unlinkFailed = false; //D38: -1 (запрос не выполнен) либо HTTP-ошибка — состояние неизвестно
  int owner = ownerSpoolByTag(uid);
  if (owner > 0) {
    int code = unlinkTagRequest(uid, owner);
    if (code == 200 || code == 204) {
      unlinkDone = true;
      if (chipWiped) {
        Serial.println("Стирание метки " + uid + ": линковка с катушкой ID:" + String(owner) + " снята");
        logger.println("Стирание метки " + uid + ": линковка с катушкой ID:" + String(owner) + " снята");
      } else {
        //D14: чип не тронут, но UID отвязан — ровно то, что предписывает решение
        Serial.println("Метка " + uid + " read-only, содержимое не стёрто, линковка снята (ID:" + String(owner) + ")");
        logger.println(sets::Logger::warn() + "Метка " + uid + " read-only, содержимое не стёрто, линковка снята (ID:" + String(owner) + ")");
      }
    } else if (code == 404) {
      //На этой катушке линковки нет: метка могла быть привязана к филаменту
      if (tagLinkedToFilament(uid)) {
        logger.println(sets::Logger::warn() + "Стирание метки " + uid + ": линковка на филамент, снимите вручную");
        done = false;
      } else {
        logger.println("Стирание метки " + uid + ": линковки на катушке ID:" + String(owner) + " нет (404)");
      }
    } else {
      unlinkFailed = true;
      logger.println(sets::Logger::error() + "Стирание метки " + uid + ": отвязка от катушки ID:" + String(owner) + " не удалась (HTTP " + String(code) + ")");
      done = false;
    }
  } else if (owner == 0) {
    if (tagLinkedToFilament(uid)) {
      logger.println(sets::Logger::warn() + "Стирание метки " + uid + ": линковка на филамент, снимите вручную");
      done = false;
    } else {
      logger.println("Стирание метки " + uid + ": метка не линкована");
    }
  } else {
    //-1: запрос владельца не выполнен (залогировано) — состояние линковки неизвестно
    unlinkFailed = true;
    done = false;
  }

  //3) D38: UID уходит из card_uids ВСЕХ катушек, где он найден. Владелец из tag/scan может
  //отсутствовать (404/нет линковки/чип read-only) — это больше не блокирует очистку: катушки
  //ищем по extra.card_uids через spoolsByCardUid и объединяем с owner без дублей вызовов.
  //Очистка НЕ выполняется, если чип не очищен И отвязка не выполнена (wipe-отказ выше уже
  //вернул false; здесь остаётся случай HTTP-ошибки отвязки / неизвестного состояния линковки).
  if (chipWiped || unlinkDone || !unlinkFailed) {
    int spools[SPOOLS_BY_CARD_UID_MAX];
    int nSpools = spoolsByCardUid(uid, spools, SPOOLS_BY_CARD_UID_MAX);
    int ids[1 + SPOOLS_BY_CARD_UID_MAX];
    int nIds = 0;
    if (owner > 0) ids[nIds++] = owner;
    for (int i = 0; i < nSpools; i++) {
      bool dup = false;
      for (int j = 0; j < nIds; j++) {
        if (ids[j] == spools[i]) { dup = true; break; }
      }
      if (!dup) ids[nIds++] = spools[i];
    }
    if (nIds) {
      String list;
      for (int i = 0; i < nIds; i++) {
        if (i) list += ", ";
        list += "ID:" + String(ids[i]);
        cardUidsUpdate(ids[i], uid, false);  //D29/D38: UID уходит из card_uids (ошибка — только лог)
      }
      Serial.println("Стирание метки " + uid + ": owner " + (owner > 0 ? String("ID:") + owner : String("не найден")) +
                     ", card_uids на катушках " + list + " -> PATCH");
      logger.println("Стирание метки " + uid + ": card_uids очищен на катушках " + list);
    } else {
      Serial.println("Стирание метки " + uid + ": owner не найден, card_uids на катушках не найдены");
      logger.println("Стирание метки " + uid + ": катушек с этим UID в card_uids нет");
    }
  }

  //4) Финал: содержимого на чипе больше нет, поэтому поля «Метка в поле считывателя» очищаем.
  //Если чип read-only, поля оставляем — они отражают то, что реально лежит на метке (как в T3).
  if (chipWiped) {
    db[web::w_id] = 0;
    db[web::w_uid] = "";
    db[web::w_type] = "";
    db[web::w_brand] = "";
    db[web::w_color] = "";  //D28: сватч меню записи гаснет — цвета на метке больше нет
  }
  return done;
}

//D33: подтверждение получено — продолжаем запись обычным путём. К этому моменту метка уже
//отпущена считывателем (readNFC делает PICC_HaltA), поэтому сначала переселекция (D23):
//без карты в поле писать некуда. Это отдельный случай «метку убрали», а не сбой подтверждения.
static void confirmWriteRun() {
  String uid = pendingUID;
  String payload = pendingPayload;
  clearPendingConfirm();
  //Переселекция обязательна (после PICC_HaltA карта в HALT), но её мало: владелец мог за это
  //время заменить метку на другую. UID сверяется — писать чужой UID'ом чужую метку нельзя.
  if (!tagReselect() || formatUID() != uid) {
    Serial.println("Запись метки " + uid + ": метка не в поле считывателя (или заменена), подтверждение не применено");
    logger.println(sets::Logger::error() + "Запись метки " + uid + ": метка убрана от считывателя или заменена — подтверждение не применено, запись отменена");
    setWriteResult(false);
    sett.reload();
    return;
  }
  wstate = WRITING;
  JsonDocument doc;
  bool parsed = parseTagJSON(payload, doc);
  tagWriteFlow(uid, payload, doc, parsed);
  wstate = IDLE;
  sett.reload();  //окно подтверждения и поля метки перерисовываются по фактическому итогу
}

//D16: тик ожидания метки — таймаут 60 с и мигание жёлтым (приоритет над статусным LED).
//true — LED в этом тике ведёт ожидание метки, статусный updateLED() вызывать не нужно.
bool writeTick() {
  T_LED_WRITE.tick();

  if (wstate != IDLE && !writeMode) {  //вышли из подменю — ожидание не продолжается
    wstate = IDLE;
    erasePending = false;
    //D33: неподтверждённая запись на залинкованную метку умирает вместе с ожиданием
    clearPendingConfirm();
  }

  //D33: метка залинкована — ждём клика в вебморде. Срок отсчитывается от касания метки
  //(tagConfirm.begin), а не от нажатия кнопки: у владельца полный WRITE_TAG_TIMEOUT на реакцию.
  if (wstate == WAIT_TAG && tagConfirm.pending()) {
    if (tagConfirm.confirmed()) {
      confirmWriteRun();
    } else if (tagConfirm.expired(millis(), WRITE_TAG_TIMEOUT)) {
      String uid = pendingUID;
      wstate = IDLE;
      erasePending = false;
      clearPendingConfirm();
      Serial.println("Запись метки " + uid + ": подтверждение не получено, отменено");
      logger.println(sets::Logger::warn() + "Запись метки " + uid + ": подтверждение записи на залинкованную метку не получено за " + String(WRITE_TAG_TIMEOUT / 1000) + " с, отменено");
      sett.reload();  //окно подтверждения должно исчезнуть из вебморды
    }
  }

  //Обычный таймаут ожидания метки: при ожидании подтверждения он не действует — у этого
  //ожидания свой срок (от касания, см. выше).
  if (wstate == WAIT_TAG && !tagConfirm.pending() && millis() - writeStart > WRITE_TAG_TIMEOUT) {
    //Ожидание было либо записью, либо стиранием (D9) — в логе называем операцию так, как её запускали
    String op = erasePending ? "Стирание метки" : "Запись метки";
    wstate = IDLE;
    erasePending = false;
    Serial.println(op + ": таймаут ожидания, отменено");
    logger.println(sets::Logger::warn() + op + ": метка не приложена за " + String(WRITE_TAG_TIMEOUT / 1000) + " с, отменено");
    sett.reload();  //D36 (v1.3.4): UI не должен висеть в «ожидании» — перерисовка как в ветке истечения подтверждения
  }

  if (wstate != WAIT_TAG) return false;
  if (T_LED_WRITE.ready()) set_led_color(YELLOW);
  else set_led_color(0);
  return true;
}
