// esp32_spoolman.cpp — все глобальные переменные и тела функций
#include "esp32_spoolman.h"
#include "spool_format.h"  //D24: палитра имён цветов и санитизация подписи опции (host-тестируемо)
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

sets::Logger logger(1024);  // уменьшен буфер
WiFiServer server(80);

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
unsigned long ledHoldUntil = 0;  //до этого момента статусный updateLED() не затирает цвет результата записи
String conflictUID = "";  //D13: UID, по которому уже сообщили владельца (повтор = перелинковка)
int conflictSpool = 0;    //D13: спул-владелец UID из 409
unsigned long lastSpoolmanFail = 0;
unsigned long lastMoonrakerFail = 0;
bool prevSpoolmanOk = false;
bool prevMoonrakerOk = false;

//D22 (диагностика): PCD_PerformSelfTest() начинается с PCD_Reset(), который возвращает RFCfgReg
//к дефолту (0x40 = 33 dB) и сбрасывает выставленный при старте 0x70 (48 dB). Гипотеза отказа
//MIFARE_Read — деградация усиления приёма после самотеста; без лога gain это не проверить.
//always=false — печатать только при смене значения (тик watchdog раз в 30 с не должен спамить).
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

TimerMs T_LED_WIFI(500, 1, 0);
TimerMs ResetLED(1000, 1, 0);
TimerMs WatchdogNFC(30000, 1, 0);
TimerMs T_LED_WRITE(250, 1, 0);

//Сброс write-состояния: вызывается на ЛЮБОЙ переход таба (Review Focus 4),
//чтобы выход из подменю не оставлял активного ожидания метки.
static void resetWriteState() {
  db[web::w_id] = 0;
  db[web::w_uid] = "";
  db[web::w_type] = "";
  db[web::w_brand] = "";
  lastUID = 0;  //метка, прочитанная до входа в режим, должна читаться заново
  //Выход из подменю отменяет ожидание метки немедленно (D16), конфликт 409 тоже сбрасывается
  wstate = IDLE;
  erasePending = false;
  conflictUID = "";
  conflictSpool = 0;
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
        writeStart = millis();
        wstate = WAIT_TAG;
        T_LED_WRITE.restart();
        Serial.println("Стирание метки: ожидание метки");
        logger.println("Стирание метки: приложите метку к считывателю (ожидание " + String(WRITE_TAG_TIMEOUT / 1000) + " с)");
      }
    }
    {
      //D26: логгер повторяем внутри меню записи — результат записи/стирания виден там же, где
      //нажимали кнопку, не уходя на главную. b.Log — обычный виджет, вызывается с тем же
      //буфером (logger) и тем же ключом: страницы разные, поэтому id не конфликтуют.
      sets::Group g(b, "Логгер");
      b.Log(H(log), logger);
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
  {
    sets::Group g(b, "NFC Метка");
    b.Label(web::nfc_id, "ID:");
    b.Label(web::nfc_uid, "UID:");
    b.Label(web::nfc_name, "Материал:");
    b.Label(web::nfc_brand, "Производитель:");
    b.Label(web::nfc_color, "Цвет:");
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
  upd.update(H(log), logger);
}

//Обновление светодиода по текущему состоянию
void updateLED() {
  if (wifiSettingMode == 1) {
    //AP режим — мигаем синим
    if (T_LED_WIFI.ready()) set_led_color(BLUE);
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
  Serial.println("Версия " VER);
  logger.println("Версия " VER);
  strip.begin();
  strip.setBrightness(50);
  strip.clear();
  strip.show();
  T_LED_WIFI.setPeriodMode();
  ResetLED.setPeriodMode();
  WatchdogNFC.setPeriodMode();
  T_LED_WRITE.setPeriodMode();
  set_led_color(0);  //гасим светодиод

  WiFi.mode(WIFI_AP_STA);

  sett.begin();
  sett.onBuild(build);
  sett.onUpdate(update);
  sett.config.updateTout = 1000;
  sett.setVersion(VER);

  LittleFS.begin(true);
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
  db.init(web::btn_write, 0);
  db.init(web::btn_erase, 0);
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
    server.begin();
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
}

void loop() {
  sett.tick();
  T_LED_WIFI.tick();
  ResetLED.tick();
  WatchdogNFC.tick();

  if (T_LED_WIFI.ready() && WiFi.status() == WL_CONNECTED) {
    statusServer();
    curent_filament_name(curentID);
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

  //LED: ожидание метки (мигание жёлтым) имеет приоритет над статусным обновлением (D16);
  //результат записи (LIME/RED) держится ~1.5 с, иначе статусный цвет затирал бы его сразу же
  if (!writeTick()) {
    if ((int32_t)(millis() - ledHoldUntil) >= 0) updateLED();
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

  if (httpResponseCode > 0) {
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
    curentID = doc["result"]["spool_id"];
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
    sett.reload();
  } else {
    errorServer(String(httpResponseCode));
    http.end();
    lastMoonrakerFail = millis();
    LED_MN = false;
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

  HTTPClient http;
  String url = "http://" + String(db[web::serverSpoolman]) + "/api/v1/spool/" + String(ID);
  http.begin(url);
  http.setTimeout(1500);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  int httpResponseCode = http.GET();
  if (httpResponseCode > 0) {
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
    db[web::filament] = name;

    //Лог имени катушки только при смене
    static String lastName = "";
    if (lastName != name) {
      lastName = name;
      Serial.println("ID: " + String(ID) + ", Name: " + String(name));
      logger.println(sets::Logger::warn() + "ID: " + String(ID) + ", Name: " + String(name));
    }

    if (!prevSpoolmanOk) {
      Serial.println("Сервер Spoolman доступен");
      logger.println("Сервер Spoolman доступен");
    }
    LED_SP = true;
    prevSpoolmanOk = true;
  } else {
    http.end();
    errorServer(String(httpResponseCode));
    db[web::filament] = "0";
    if (prevSpoolmanOk) {
      Serial.println("Сервер Spoolman не доступен");
      logger.println(sets::Logger::error() + "Сервер Spoolman не доступен");
    }
    LED_SP = false;
    prevSpoolmanOk = false;
    lastSpoolmanFail = millis();
  }
  sett.reload();
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
        set_led_color(LIME);
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
        set_led_color(LIME);
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
//Цвет результата записи/стирания (определение — в секции «Запись метки v1.3»)
static void setWriteResult(uint32_t color);

void readNFC() {
  if (!mfrc522.PICC_IsNewCardPresent()) {
    lastUID = 0;
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
    Serial.println("Режим записи: метка " + uid + " ID:" + String(tagID) + " Тип:" + String(doc["type"] | "") + " Бренд:" + String(doc["brand"] | "") + " (активация заблокирована)");
    logger.println("Режим записи: метка " + uid + " ID:" + String(tagID) + " Тип:" + String(doc["type"] | "") + " Бренд:" + String(doc["brand"] | ""));

    //Кнопка «Записать метку»/«Стереть метку» нажата — метка в поле считывателя, запускаем машину (D16).
    //Намерение (erasePending) снимаем здесь же: иначе следующее касание повторило бы операцию.
    if (wstate == WAIT_TAG) {
      bool doErase = erasePending;
      erasePending = false;
      wstate = doErase ? ERASING : WRITING;
      if (doErase) {
        setWriteResult(eraseTag() ? LIME : RED);
      } else {
        tagWriteFlow(uid, payload, doc, parsed);
      }
      wstate = IDLE;
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
      set_led_color(YELLOW);  //D20: жёлтый, не зелёный — катушка не установлена
      return;
    }
    Serial.println("Метка привязана к катушке ID:" + String(matched));
    logger.println("Метка " + uid + " привязана к катушке ID:" + String(matched));
    set_led_color(YELLOW);
    SetSpool(matched);
    if (mmu == true) {
      mmu_next_spoolid(matched);
    }
    return;
  }

  //2) Fallback: содержимое метки — openspool spool_id, затем FilaMan sm_id (D3)
  if (tagID > 0) {
    Serial.println("Установка катушки по содержимому метки ID:" + String(tagID));
    logger.println("Установка катушки по содержимому метки ID:" + String(tagID));
    set_led_color(YELLOW);
    SetSpool(tagID);
    if (mmu == true) {
      mmu_next_spoolid(tagID);
    }
    return;
  }

  //3) Ни UID-линковки, ни распознанного формата
  Serial.print("Метка не распознана, payload: ");
  Serial.println(payload);
  logger.println(sets::Logger::error() + "Метка " + uid + " не распознана (нет линковки и NDEF-формата)");
  set_led_color(RED);
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
  http.setTimeout(1500);
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
bool readSpoolForTag(int spoolID, String& material, String& brand, String& colorHex, int& extruderTemp, int& bedTemp) {
  material = "";
  brand = "";
  colorHex = "";
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
  return true;
}

//D6: openspool-JSON из данных Spoolman. Значения — строки, как в эталонной метке Spool Painter
//(nfc_spoolpainter.md: "min_temp":"220", "spool_id":"37"); bed_min_temp/bed_max_temp — ключи
//расширения openspool, которыми пишет Spool Painter (в D6 они названы bed_min/bed_max).
static String buildOpenspoolJSON(int spoolID, const String& material, const String& brand, const String& colorHex, int extruderTemp, int bedTemp) {
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
  doc["subtype"] = "Basic";

  String out;
  serializeJson(doc, out);
  return out;
}

//D5/Review Focus 2: в вендорский JSON дописываем ТОЛЬКО spool_id; re-serialize через ArduinoJson
//сохраняет все ключи вендора (температуры и subtype не трогаем).
static String augmentVendorJSON(JsonDocument& doc, int spoolID) {
  doc["spool_id"] = String(spoolID);
  String out;
  serializeJson(doc, out);
  return out;
}

//Формат содержимого метки: 0 — пусто/не распознано (пишем openspool из Spoolman, D6),
//1 — вендорский/openspool (дописываем spool_id, D5), 2 — FilaMan legacy (полная замена, D15).
static int tagPayloadFormat(JsonDocument& doc, bool parsed) {
  if (!parsed) return 0;
  if (doc["protocol"].is<const char*>()) return 1;
  if (jsonHasID(doc, "sm_id")) return 2;
  if (doc["type"].is<const char*>() || jsonHasID(doc, "spool_id")) return 1;
  return 0;
}

//Цвет результата записи (LIME/RED) держится ~1.5 с: иначе статусный updateLED() в том же
//проходе loop() сразу затрёт его (см. ledHoldUntil в loop()).
#define LED_HOLD_MS 1500

static void setWriteResult(uint32_t color) {
  set_led_color(color);
  ledHoldUntil = millis() + LED_HOLD_MS;
}

//D8/D13: линковка с двухшаговым подтверждением конфликта. true — UID привязан к spoolID.
static bool linkWithConflict(const String& uid, int spoolID) {
  String holder;
  if (linkTagUID(uid, spoolID, holder)) {
    conflictUID = "";
    conflictSpool = 0;
    Serial.println("Метка " + uid + " привязана к катушке ID:" + String(spoolID));
    logger.println("Метка " + uid + " привязана к катушке ID:" + String(spoolID));
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
  return true;
}

//Step 2 плана: машина записи по касанию метки в WAIT_TAG (D5–D8, D13–D16).
//Порядок: payload (D5/D6/D15) -> lock (D7; D14 — только линковка) -> запись -> подтверждение -> линковка.
static void tagWriteFlow(const String& uid, const String& payloadIn, JsonDocument& tagDoc, bool parsed) {
  int spoolID = selectedSpoolID();
  if (spoolID <= 0) {
    Serial.println("Запись метки: катушка не выбрана");
    logger.println(sets::Logger::error() + "Запись метки: катушка не выбрана (обновите список катушек)");
    setWriteResult(RED);
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
    setWriteResult(RED);
    return;
  }
  if (lock == TAG_LOCK_LOCKED) {
    //D21: содержимое не записано — значит и UID не линкуем (привязка без записи запрещена)
    Serial.println("Запись метки " + uid + ": метка read-only, запись не выполнена, UID не привязан");
    logger.println(sets::Logger::warn() + "Запись метки " + uid + ": метка read-only — содержимое не записано, UID не привязан к ID:" + String(spoolID));
    setWriteResult(RED);
    return;  //поля w_* уже отражают фактическое содержимое метки (её не меняли)
  }

  //2) Что пишем: вендорский JSON + spool_id (D5), FilaMan — полная замена (D15), пусто — из Spoolman (D6)
  int fmt = tagPayloadFormat(tagDoc, parsed);
  String payloadOut;
  if (fmt == 1) {
    payloadOut = augmentVendorJSON(tagDoc, spoolID);
    if (!payloadOut.length()) {
      logger.println(sets::Logger::error() + "Запись метки: не удалось пересобрать вендорский JSON");
      setWriteResult(RED);
      return;
    }
    Serial.println("Вендорская метка: дописан spool_id=" + String(spoolID) + ", ключи сохранены");
    logger.println("Вендорская метка: дописан spool_id=" + String(spoolID) + ", остальные ключи сохранены");
  } else {
    String material, brand, colorHex;
    int extruderTemp = 0, bedTemp = 0;
    if (!readSpoolForTag(spoolID, material, brand, colorHex, extruderTemp, bedTemp)) {
      setWriteResult(RED);
      return;
    }
    payloadOut = buildOpenspoolJSON(spoolID, material, brand, colorHex, extruderTemp, bedTemp);
    if (fmt == 2) {
      Serial.println("FilaMan-метка: полная перезапись в openspool+spool_id");
      logger.println("FilaMan-метка (sm_id): полная перезапись в openspool+spool_id");
    } else {
      Serial.println("Метка пустая/нераспознана: openspool JSON собран из Spoolman");
      logger.println("Метка пустая/нераспознана: openspool JSON собран из Spoolman ID:" + String(spoolID));
      if (payloadIn.length()) logger.println(sets::Logger::warn() + "Содержимое метки не распознано и будет перезаписано: " + payloadIn);
    }
  }

  //3) Запись + подтверждение перечитыванием
  if (!writeNDEF(payloadOut)) {
    Serial.println("Запись метки " + uid + " не удалась");
    logger.println(sets::Logger::error() + "Запись метки " + uid + " не удалась, причина в логах выше");
    setWriteResult(RED);
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
  } else {
    db[web::w_id] = spoolID;
  }

  setWriteResult(linked ? LIME : RED);
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
  int owner = ownerSpoolByTag(uid);
  if (owner > 0) {
    int code = unlinkTagRequest(uid, owner);
    if (code == 200 || code == 204) {
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
    done = false;
  }

  //3) Финал: содержимого на чипе больше нет, поэтому поля «Метка в поле считывателя» очищаем.
  //Если чип read-only, поля оставляем — они отражают то, что реально лежит на метке (как в T3).
  if (chipWiped) {
    db[web::w_id] = 0;
    db[web::w_uid] = "";
    db[web::w_type] = "";
    db[web::w_brand] = "";
  }
  return done;
}

//D16: тик ожидания метки — таймаут 60 с и мигание жёлтым (приоритет над статусным LED).
//true — LED в этом тике ведёт ожидание метки, статусный updateLED() вызывать не нужно.
bool writeTick() {
  T_LED_WRITE.tick();

  if (wstate != IDLE && !writeMode) {  //вышли из подменю — ожидание не продолжается
    wstate = IDLE;
    erasePending = false;
  }

  if (wstate == WAIT_TAG && millis() - writeStart > WRITE_TAG_TIMEOUT) {
    //Ожидание было либо записью, либо стиранием (D9) — в логе называем операцию так, как её запускали
    String op = erasePending ? "Стирание метки" : "Запись метки";
    wstate = IDLE;
    erasePending = false;
    Serial.println(op + ": таймаут ожидания, отменено");
    logger.println(sets::Logger::warn() + op + ": метка не приложена за " + String(WRITE_TAG_TIMEOUT / 1000) + " с, отменено");
  }

  if (wstate != WAIT_TAG) return false;
  if (T_LED_WRITE.ready()) set_led_color(YELLOW);
  else set_led_color(0);
  return true;
}
