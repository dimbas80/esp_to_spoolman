/*
Установка по NFC метке катушки в очередь печати Spoolman, Moonraker
автор: Dimbas80
*/
#define VER "1.0"
////////////////////////
//Библиотеки
///////////////////////

//ПИНЫ
#define SW_WIFI 6  //Пин Кнопки перевода в режим АР при загрузке

#include <WiFi.h>
#include <HTTPClient.h>
#include <StringUtils.h>
#include <Arduino.h>
#include <ArduinoJson.h>

//Светодиод WS2812
#include <FastLED.h>
#define NUMLEDS 1
#define PIN_DATA 0
#define BRIGHTNESS 100
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB
#define RED 0
#define BLUE 150
#define GREEN 100
#define YELLOW 30

CRGB leds[NUMLEDS];

//============= NFC MFRC522 ===============
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#define RST_PIN 1  // Configurable, see typical pin layout above
#define SS_PIN 5   // Configurable, see typical pin layout above

MFRC522DriverPinSimple ss_pin(SS_PIN);  // Create pin driver. See typical pin layout above.
MFRC522DriverSPI driver{ ss_pin };      // Create SPI driver.
MFRC522 mfrc522{ driver };              // Create MFRC522 instance.

//=================== DB ==================
#include <GyverDBFile.h>
#include <LittleFS.h>
GyverDBFile db(&LittleFS, "/data.db");

#include <SettingsGyver.h>
SettingsGyver sett("ESP to Spoolman", &db);

sets::Logger logger(2000);  // размер буфера

WiFiServer server(80);

//имена ячеек базы данных
DB_KEYS(
  web,
  wifi_ssid,
  wifi_pass,
  serverSpoolman,
  serverMoonraker,
  apply1,
  apply2,
  led_sp,
  led_mn,
  led_nfc,
  id,
  filament,
  nfc_id,
  nfc_name,
  nfc_brand,
  nfc_color,
  nfc_par);

//Переменные
bool LED_SP = false;        // WEB светодиод доступности спулман
bool LED_MN = false;        // WEB светодиод доступности Moonraker
bool LED_NFC = false;       // WEB светодиод доступности NFC считывателя
int curentID;               // ID текущей катушки
const char* filament_name;  // Имя филамента
bool wifiSettingMode = 0;   //Перемнная режима WIFI. 0 - SSID, 1 - AP
bool fl_status = false;     //флаг статуса
bool fl_led = false;        //флаг led


//===========Таймеры=====================
#include <TimerMs.h>
TimerMs T_LED_WIFI(500, 1, 0);   //Периодичность мигания светодиода WIFI
TimerMs ResetLED(1000, 1, 0);    //Таймер сброса светодиода
TimerMs RebootNFC(60000, 1, 0);  //Таймер сброса NFC против зависания

//Интерфейс настроек
void build(sets::Builder& b) {
  {
    sets::Group g(b, "WiFi");
    b.Input(web::wifi_ssid, "SSID");
    b.Pass(web::wifi_pass, "Password");
  }
  {
    sets::Group g(b, "Сервера");
    if (b.Input(web::serverSpoolman, "IP:Port Spoolman")) {  //, db[web::serverSpoolman], sets::Code::regex = ("Введите IP адрес сервера Spoolman с указанием порта"));) {
      logger.println("Set IP:Port Spoolman:" + String(db[web::serverSpoolman]));
      db.update();
      //statusServer();
      //curent_filament_name(curentID);
    }
    if (b.Input(web::serverMoonraker, "IP:Port Moonraker(Принтер)")) {
      logger.println("Set IP:Port Moonraker(Принтер):" + String(db[web::serverMoonraker]));
      db.update();
      //statusServer();
    }
    if (b.Button(web::apply1, "Save & Restart")) {
      db.update();  // сохраняем БД не дожидаясь таймаута
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
    //b.Label(web::nfc_color, "Цвет:");
  }

  {
    sets::Group g(b, "Логгер");
    b.Log(H(log), logger);
  }
}


void update(sets::Updater& upd) {
  // отправить лог
  upd.update(H(log), logger);
}


/*==================================
            S E T U P 
====================================
*/
void setup() {
  Serial.begin(115200);
  T_LED_WIFI.setPeriodMode();
  RebootNFC.setPeriodMode();
  ResetLED.setPeriodMode();

  //============Светодиод=============
  FastLED.addLeds<LED_TYPE, PIN_DATA, COLOR_ORDER>(leds, NUMLEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();

  //============= WIFI ===============

  WiFi.mode(WIFI_AP_STA);

  //============= Web интерфейс ===============

  sett.begin();
  sett.onBuild(build);
  sett.onUpdate(update);
  sett.config.updateTout = 1000;
  sett.setVersion(VER);


  //Инициализация базы данных настроек
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

  db[web::nfc_id] = 0;
  db[web::nfc_name] = "";
  db[web::nfc_brand] = "";
  db[web::nfc_color] = "";
  db[web::id] = 0;
  db[web::filament] = "";

  //Входы выходы
  pinMode(SW_WIFI, INPUT_PULLUP);  //Кнопка сброса WIFI


  //============ WIFI AP ========
  //Включаем точку доступа при нажатой кнопке или если не задан логин
  if (digitalRead(SW_WIFI) == 0 or !db[web::wifi_ssid].length()) {
    wifiSettingMode = 1;
    // ======= AP =======
    WiFi.softAP("AP ESP_to_Spoolman");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }


  // ======= STA =======
  // если логин задан - подключаемся
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
  }

  if (WiFi.status() == WL_CONNECTED) {
    statusServer();
    curent_filament_name(curentID);
  }

  //============= инициализация считывателя RC522 ===============

  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, HIGH);

  mfrc522.PCD_Init();                                      // Init MFRC522
  mfrc522.PCD_SetAntennaGain(0x07);                        // Установка усиления антенны
  mfrc522.PCD_AntennaOff();                                // Перезагружаем антенну
  mfrc522.PCD_AntennaOn();                                 // Включаем антенну
  MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);  // Show details of PCD - MFRC522 Card Reader details.
  delay(100);

  if (mfrc522.PCD_PerformSelfTest()) {
    Serial.print("Найден считыватель RC522: ");
    MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);  // Show version of PCD - MFRC522 Card Reader.
    //MFRC522Debug::PCD_DumpVersionToSerial();  // Show version of PCD - MFRC522 Card Reader.

    logger.print("Найден считыватель RC522: ");
    MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, logger);  // Show version of PCD - MFRC522 Card Reader.
    LED_NFC = true;
  } else {
    Serial.print("Не найден считыватель RC522");
    logger.println("Не найден считыватель RC522");
    LED_NFC = false;
  }
}

/*=================================
         L O O P
===================================         
*/
void loop() {
  sett.tick();
  T_LED_WIFI.tick();
  ResetLED.tick();
  RebootNFC.tick();


  //Переодический ребут считывателя и проверка статуса серверов
  if (RebootNFC.ready()) {
    resetMFRC();
    statusServer();
    curent_filament_name(curentID);
  }

  //Переодический сброс флага светодиода
  if (ResetLED.ready()) {
    fl_led = false;
  }

  //Читаем метку
  if (mfrc522.PCD_PerformSelfTest()) {
    readNFC();
  } else resetMFRC();

  //Мигаем светодиодом в режиме АР
  if (wifiSettingMode == 1) {
    if (T_LED_WIFI.ready()) {
      leds[0].setHue(BLUE);
      FastLED.show();
    } else {
      FastLED.clear();
      FastLED.show();
    }
  }
  //При подключении WIFI и доступности серверов включаем светодиод
  if (fl_led == false) {
    if (wifiSettingMode == 0 and WiFi.status() == WL_CONNECTED) {
      leds[0].setHue(BLUE);
      FastLED.show();
      if (LED_MN == 1 and LED_SP == 1) {
        leds[0].setHue(GREEN);
        FastLED.show();
      }
    } else {
      leds[0].setHue(RED);
      FastLED.show();
    }
  }

  //Читаем Serial если "status", то выводим статус серверов и текующую установленную катушку, если число, то устанавливаем катушку с числом если найдена
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


/*===============================
============ ФУНКЦИИ ============
=================================
*/
//Светодиод WS2812
void set_led_color(int color) {
  leds[0].setHue(color);
  FastLED.show();
  fl_led = true;
}

// Ошибка ответа сервера
void errorServer(String error) {
  Serial.println("Ошибка ответа сервера: " + String(error));
  logger.println(sets::Logger::error() + "Ошибка ответа сервера: " + String(error));
  set_led_color(RED);
  return;
}

//Сброс считываетля
void resetMFRC() {
  digitalWrite(RST_PIN, LOW);        // Сбрасываем модуль
  delayMicroseconds(100);            // Ждем
  digitalWrite(RST_PIN, HIGH);       // Отпускаем сброс
  mfrc522.PCD_Init();                // Инициализируем заного
  mfrc522.PCD_SetAntennaGain(0x07);  // Установка усиления антенны
  mfrc522.PCD_AntennaOff();          // Перезагружаем антенну
  mfrc522.PCD_AntennaOn();           // Включаем антенну
  Serial.println(F("Reboot RC522"));
  MFRC522Debug::PCD_DumpVersionToSerial(mfrc522, Serial);
  if (mfrc522.PCD_PerformSelfTest()) {
    LED_NFC = true;
  } else LED_NFC = false;
}

//Статус сервера
void statusServer() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String geturl = "http://" + String(db[web::serverMoonraker]) + "/server/spoolman/status";

    http.begin(geturl);

    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.GET();

    if (httpResponseCode > 0) {
      String response = http.getString();
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, response);
      http.end();
      //Проверка на ошибки ответа от сервера
      if (error) {
        String err = error.f_str();
        errorServer(err);
        http.end();
        LED_MN = false;
        return;
      }

      bool statusServer = doc["result"]["spoolman_connected"];
      curentID = doc["result"]["spool_id"];
      db[web::id] = curentID;

      if (statusServer) {
        Serial.println("Сервер Moonraker доступен");
        logger.println("Сервер Moonraker доступен");
        LED_MN = true;
        sett.reload();
      } else {
        Serial.println("Сервер Moonraker не доступен");
        logger.println(sets::Logger::error() + "Сервер Moonraker не доступен");
        LED_MN = false;
        sett.reload();
        return;
      }
    } else {
      errorServer(String(httpResponseCode));
      http.end();
      LED_MN = false;
      return;
    }
  }
}

// Запрос имени катушки
const char* curent_filament_name(int ID) {

  // Block until we are able to connect to the WiFi access point
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String geturl = "http://" + String(db[web::serverSpoolman]) + "/api/v1/spool";

    http.begin(geturl);

    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.GET();
    if (httpResponseCode > 0) {
      String payload = http.getString();
      JsonDocument doc;

      DeserializationError error = deserializeJson(doc, payload);
      http.end();
      //Проверка на ошибки ответа от сервера
      if (error) {
        String err = error.f_str();
        errorServer(err);
        filament_name = "0";
        db[web::filament] = filament_name;
        http.end();
        Serial.println("Сервер Spoolman не доступен");
        logger.println(sets::Logger::error() + "Сервер Spoolman не доступен");
        LED_SP = false;
        sett.reload();
        return filament_name;
      }
      //Максимум 1000 катушек
      for (int i = 0; i < 1000; i++) {
        int root_id = doc[i]["id"];
        if (root_id == 0) {
          filament_name = "0";
          db[web::filament] = filament_name;
          break;
        }
        if (root_id == ID) {
          filament_name = doc[i]["filament"]["name"];
          db[web::filament] = filament_name;
          Serial.println("ID: " + String(root_id) + ", Name: " + String(filament_name));
          logger.println(sets::Logger::warn() + "ID: " + String(root_id) + ", Name: " + String(filament_name));
          break;
        }
      }
      Serial.println("Сервер Spoolman доступен");
      logger.println("Сервер Spoolman доступен");
      LED_SP = true;
      sett.reload();
      return filament_name;
    } else {
      errorServer(String(httpResponseCode));
      http.end();
      filament_name = "0";
      db[web::filament] = filament_name;
      Serial.println("Сервер Spoolman не доступен");
      logger.println(sets::Logger::error() + "Сервер Spoolman не доступен");
      LED_SP = false;
      sett.reload();
      return filament_name;
    }
  }
  sett.reload();
  return filament_name;
}

//Запрос ID существующих катушек у Spoolman
bool getIDSpool(int ID) {
  bool IDexists = false;
  // Block until we are able to connect to the WiFi access point
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String geturl = "http://" + String(db[web::serverSpoolman]) + "/api/v1/spool";

    http.begin(geturl);

    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.GET();
    if (httpResponseCode > 0) {
      String payload = http.getString();
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);
      http.end();
      //Проверка на ошибки ответа от сервера
      if (error) {
        String err = error.f_str();
        errorServer(err);
        http.end();
        return IDexists;
      }
      //Максимум 1000 катушек
      for (int i = 0; i < 1000; i++) {
        int root_id = doc[i]["id"];
        if (root_id == 0) break;
        if (root_id == ID) IDexists = true;
      }
    } else {
      errorServer(String(httpResponseCode));
      http.end();
      return IDexists;
    }
  }
  sett.reload();
  return IDexists;
}

//Задание катушки с ID
void SetSpool(int SetID) {
  // Block until we are able to connect to the WiFi access point
  if (WiFi.status() == WL_CONNECTED) {
    if (getIDSpool(SetID)) {
      HTTPClient http;
      String geturl = "http://" + String(db[web::serverMoonraker]) + "/server/spoolman/spool_id";

      http.begin(geturl);

      http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
      http.addHeader("Content-Type", "application/json");

      //Формируем документ JSON для отправки на сервер
      JsonDocument doc_post;  //Create json document
      doc_post["spool_id"] = SetID;

      String requestBody_post;
      serializeJson(doc_post, requestBody_post);
      //Отправка
      int httpResponseCode = http.POST(requestBody_post);
      //Ответ от сервера об отправке
      if (httpResponseCode > 0) {
        String response = http.getString();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, response);
        if (error) {
          String err = error.f_str();
          errorServer(err);
          http.end();
          return;
        }
        int root_id = doc["result"]["spool_id"];
        Serial.println("Установка катушки с  ID: " + String(root_id));
        logger.println("Установка катушки с  ID: " + String(root_id));
        http.end();
      } else {
        errorServer(String(httpResponseCode));
        http.end();
      }
      http.end();
      statusServer();
      if (curentID == SetID) {
        Serial.println("Катушка с ID:" + String(SetID) + " успешно установлена");
        logger.println("Катушка с ID:" + String(SetID) + " успешно установлена");
        curent_filament_name(curentID);
      } else {
        Serial.println("Ошибка установки катушки с ID: " + String(SetID));
        logger.println(sets::Logger::error() + "Ошибка установки катушки с ID: " + String(SetID));
      }
      sett.reload();
    } else {
      Serial.println("Катушка c ID: " + String(SetID) + " не существует");
      logger.println(sets::Logger::error() + "Катушка c ID: " + String(SetID) + " не существует");
      return;
    }
  }
  return;
}

// //Чтение NFC метки
void readNFC() {

  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }

  // Select one of the cards
  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  MFRC522::StatusCode status;
  byte byteCount;
  uint8_t buffer[60];
  String payload;
  String payload_data;
  byte i;



  for (byte page = 8; page <= 60; page += 4) {  // Read returns data for 4 pages at a time.
    // Read pages
    byteCount = 18;  //sizeof(buffer);
    status = mfrc522.MIFARE_Read(page, buffer, &byteCount);

    if (status != MFRC522::StatusCode::STATUS_OK) {
      Serial.println(F("Ошибка чтения метки"));
      logger.println(sets::Logger::error() + "Ошибка чтения метки");
      set_led_color(RED);
      return;
    }

    for (byte offset = 0; offset < 4; offset++) {
      i = page + offset;
      for (byte index = 0; index < 4; index++) {
        i = 4 * offset + index;
        payload_data += char(buffer[i]);
      }
    }

    if (page == 60) {
      //Serial.println();
      //Serial.println("Чтение закончено");
      break;
    }
  }
  //Serial.print("payload_data: ");
  //Serial.println(payload_data);
  for (int i = 5; i < payload_data.length(); i++) {
    payload += payload_data[i];
  }
  Serial.print("payload: ");
  Serial.println(payload);
  mfrc522.PICC_HaltA();
  //Расшифровываем JSON документ из метки
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.print("Ошибка расшифровки JSON: ");
    Serial.println(error.c_str());
    logger.print(sets::Logger::error() + "Ошибка расшифровки JSON: ");
    logger.println(error.c_str());
    set_led_color(RED);
    resetMFRC();
    return;
  }
  set_led_color(YELLOW);  //Включаем желтый светодиод
  //Перемнные метки NFC
  const char* sm_id;      // "ID филамента"
  const char* color_hex;  // "Цвет в HEX"
  const char* type;       // "Тип филамента"
  const char* min_temp;   // "Мин температураа"
  const char* max_temp;   // "Макс температура"
  const char* brand;      // "Производитель "

  sm_id = doc["sm_id"];          // "8"
  color_hex = doc["color_hex"];  // "0015ff"
  type = doc["type"];            // "PETG"
  min_temp = doc["min_temp"];    // "175"
  max_temp = doc["max_temp"];    // "275"
  brand = doc["brand"];          // "АБС Мейкер"
  db[web::nfc_id] = atoi(sm_id);
  db[web::nfc_name] = type;
  db[web::nfc_brand] = brand;
  db[web::nfc_color] = color_hex;
  Serial.println("Прочитана метка с ID:" + String(sm_id) + " Тип:" + String(type));
  logger.println("Прочитана метка с ID:" + String(sm_id) + " Тип:" + String(type));

  SetSpool(atoi(sm_id));  //Устанавливаем активной катушку из метки
  return;
}