// esp32_spoolman.cpp — все глобальные переменные и тела функций
#include "esp32_spoolman.h"
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
uint32_t lastUID = 0;
unsigned long lastSpoolmanFail = 0;
unsigned long lastMoonrakerFail = 0;
bool prevSpoolmanOk = false;
bool prevMoonrakerOk = false;

TimerMs T_LED_WIFI(500, 1, 0);
TimerMs ResetLED(1000, 1, 0);
TimerMs WatchdogNFC(30000, 1, 0);

//Сброс write-состояния: вызывается на ЛЮБОЙ переход таба (Review Focus 4),
//чтобы выход из подменю не оставлял активного ожидания метки.
static void resetWriteState() {
  db[web::w_id] = 0;
  db[web::w_uid] = "";
  db[web::w_type] = "";
  db[web::w_brand] = "";
  lastUID = 0;  //метка, прочитанная до входа в режим, должна читаться заново
}

//Интерфейс настроек
void build(sets::Builder& b) {
  //Табы: 0 — настройки, 1 — «Записать метку» (b.Tabs отдаёт индекс опции)
  int tab = db[web::tab];
  if (b.Tabs(web::tab, "Настройки;Записать метку")) {
    tab = db[web::tab];  //SettingsBase пишет значение в БД до вызова build()
    resetWriteState();
    if (tab == 1) {
      logger.println("Режим записи: активация катушки заблокирована");
      fetchSpoolList((bool)db[web::f_notag]);
    } else {
      logger.println("Режим настроек: активация катушки разрешена");
    }
  }

  if (tab == 1) {
    //==== Подменю «Записать метку» (D10: только чтение метки, активация заблокирована) ====
    {
      sets::Group g(b, "Катушка для записи");
      if (b.Switch(web::f_notag, "Только катушки без метки")) {
        db[web::spool_sel] = 0;  //после смены фильтра индексы опций другие
        fetchSpoolList((bool)db[web::f_notag]);
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
      //Обработчики кнопок — машина записи/стирания (Task 3/4)
      b.Button(web::btn_write, "Записать метку");
      b.Button(web::btn_erase, "Стереть метку", sets::Colors::Red);
    }
    {
      sets::Group g(b, "Логгер");
      b.Log(H(log), logger);
    }
    return;
  }

  {
    sets::Group g(b, "WiFi");
    b.Input(web::wifi_ssid, "SSID");
    b.Pass(web::wifi_pass, "Password");
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
    if (b.Button(web::apply2, "Проверка статуса серверов")) {
      fl_status = true;
    }
    b.LED(web::led_mn, "Статус Moonraker", LED_MN, sets::Colors::Yellow, sets::Colors::Green);
    b.LED(web::led_sp, "Статус Spoolman", LED_SP, sets::Colors::Yellow, sets::Colors::Green);
    b.LED(web::led_nfc, "NFC считыватель", LED_NFC, sets::Colors::Yellow, sets::Colors::Green);
  }
  {
    sets::Group g(b, "Текущая катушка установленная в SpoolMan");
    b.Label(web::id, "ID:");
    b.Label(web::filament, "Имя:");
  }
  {
    sets::Group g(b, "NFC Метка");
    b.Label(web::nfc_id, "ID:");
    b.Label(web::nfc_name, "Имя:");
    b.Label(web::nfc_brand, "Производитель:");
  }
  {
    sets::Group g(b, "Логгер");
    b.Log(H(log), logger);
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
  db.init(web::id, 0);
  db.init(web::filament, "");
  //Ключи подменю «Записать метку» (v1.3 Task 2)
  db.init(web::tab, 0);
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
  db[web::id] = 0;
  db[web::filament] = "";
  //Режим записи не переживает перезагрузку: иначе блокировка активации «залипнет»
  db[web::tab] = 0;
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

  //Режим записи — производное от активного таба (0 — настройки, 1 — записать метку)
  writeMode = ((int)db[web::tab] == 1);

  readNFC();

  //Обновляем светодиод по состоянию системы
  updateLED();

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

//Payload NDEF-записи (application/json у openspool/FilaMan). «» — если TLV/запись не распарсились.
//Читаем страницы с 4-й кусками по 4 страницы (MIFARE_Read отдаёт 16 байт + 2 CRC),
//пока не исчерпана TLV-длина, не встретится 0xFE, не кончится память метки или лимит NDEF_MAX_BYTES.
String ndefPayload() {
  static uint8_t mem[NDEF_MAX_BYTES];  //вне стека loop(): до 888 байт
  uint16_t memLen = 0;
  uint8_t buffer[18];

  for (uint16_t page = NDEF_PAGE_START; memLen < NDEF_MAX_BYTES; page += 4) {
    byte byteCount = sizeof(buffer);
    if (mfrc522.MIFARE_Read((byte)page, buffer, &byteCount) != MFRC522::StatusCode::STATUS_OK) {
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
    mfrc522.PICC_HaltA();
    sett.reload();
    Serial.println("Режим записи: метка " + uid + " ID:" + String(tagID) + " Тип:" + String(doc["type"] | "") + " Бренд:" + String(doc["brand"] | "") + " (активация заблокирована)");
    logger.println("Режим записи: метка " + uid + " ID:" + String(tagID) + " Тип:" + String(doc["type"] | "") + " Бренд:" + String(doc["brand"] | ""));
    return;
  }

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

  //1) UID-first: линковка UID -> катушка в Spoolman имеет приоритет над содержимым метки (D2)
  int matched = -1;
  if (spoolIDByUID(uid, matched)) {
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

//GET /api/v1/spool?allow_archived=false -> опции «{id} — {filament.name}» через ';' в spool_opts
//и параллельный csv id в spool_ids: b.Select отдаёт индекс опции, а не id катушки (Review Focus 3).
//noTagOnly — только катушки без tags. Больше N_OPT_MAX опций — усечение + лог (Q8).
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
  filter[0]["filament"]["name"] = true;
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

  String opts, ids;
  int matched = 0, cnt = 0;
  for (JsonObject sp : doc.as<JsonArray>()) {
    //Фильтр «только без метки»: у катушки нет ни одной привязанной метки
    if (noTagOnly && sp["tags"].size() > 0) continue;
    matched++;
    if (cnt >= N_OPT_MAX) continue;  //дальше только считаем, чтобы залогировать усечение

    int spoolID = sp["id"] | -1;
    if (spoolID <= 0) continue;
    String name = sp["filament"]["name"] | "";
    //';' и перевод строки — разделители опций Select: подменяем в имени, иначе список «съедет»
    name.replace(';', ',');
    name.replace('\n', ' ');

    if (cnt) {
      opts += ';';
      ids += ',';
    }
    opts += String(spoolID) + " — " + name;
    ids += String(spoolID);
    cnt++;
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
