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
uint32_t lastUID = 0;
unsigned long lastSpoolmanFail = 0;
unsigned long lastMoonrakerFail = 0;
bool prevSpoolmanOk = false;
bool prevMoonrakerOk = false;

TimerMs T_LED_WIFI(500, 1, 0);
TimerMs ResetLED(1000, 1, 0);
TimerMs WatchdogNFC(30000, 1, 0);

//Интерфейс настроек
void build(sets::Builder& b) {
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
  mmu = db[web::sw_mmu]; 
  db[web::nfc_id] = 0;
  db[web::nfc_name] = "";
  db[web::nfc_brand] = "";
  db[web::nfc_color] = "";
  db[web::id] = 0;
  db[web::filament] = "";

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

void readNFC() {
  if (!mfrc522.PICC_IsNewCardPresent()) {
    lastUID = 0;
    return;
  }
  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  uint32_t uidHash = 0;
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    uidHash = (uidHash << 8) | mfrc522.uid.uidByte[i];
  }
  if (uidHash == lastUID) {
    mfrc522.PICC_HaltA();
    return;
  }
  lastUID = uidHash;

  MFRC522::StatusCode status;
  byte byteCount;
  uint8_t buffer[60];
  String payload, payload_data;

  for (byte page = 8; page <= 60; page += 4) {
    byteCount = 18;
    status = mfrc522.MIFARE_Read(page, buffer, &byteCount);
    if (status != MFRC522::StatusCode::STATUS_OK) {
      Serial.println(F("Ошибка чтения метки"));
      logger.println(sets::Logger::error() + "Ошибка чтения метки");
      set_led_color(RED);
      return;
    }
    for (byte offset = 0; offset < 4; offset++) {
      for (byte index = 0; index < 4; index++) {
        payload_data += char(buffer[4 * offset + index]);
      }
    }
    if (page == 60) break;
  }

  for (int i = 5; i < payload_data.length(); i++) {
    payload += payload_data[i];
  }
  Serial.print("payload: ");
  Serial.println(payload);
  mfrc522.PICC_HaltA();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("Ошибка расшифровки JSON: ");
    Serial.println(error.c_str());
    logger.print(sets::Logger::error() + "Ошибка расшифровки JSON: ");
    logger.println(error.c_str());
    set_led_color(RED);
    return;
  }
  set_led_color(YELLOW);

  const char* sm_id = doc["sm_id"] | "";
  const char* color_hex = doc["color_hex"] | "";
  const char* type = doc["type"] | "";
  const char* brand = doc["brand"] | "";

  db[web::nfc_id] = atoi(sm_id);
  db[web::nfc_name] = type;
  db[web::nfc_brand] = brand;
  db[web::nfc_color] = color_hex;
  Serial.println("Прочитана метка с ID:" + String(sm_id) + " Тип:" + String(type));
  logger.println("Прочитана метка с ID:" + String(sm_id) + " Тип:" + String(type));

  SetSpool(atoi(sm_id));
  if (mmu == true) {
    mmu_next_spoolid(atoi(sm_id));
  }
}
