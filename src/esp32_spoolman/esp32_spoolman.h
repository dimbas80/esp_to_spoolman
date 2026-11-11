// esp32_spoolman.h — все объявления и глобальные переменные
#pragma once

//Принудительно включаем USB Serial на ESP32-C3
#ifndef ARDUINO_USB_CDC_ON_BOOT
#define ARDUINO_USB_CDC_ON_BOOT 1
#endif
#ifndef ARDUINO_USB_MODE
#define ARDUINO_USB_MODE 1
#endif

#define VER "1.3.1"
#define SW_WIFI 6

//Фиксированный идентификатор читателя для Spoolman POST /api/v1/tag/scan
#define READER_ID "esp32-to-spoolman"

#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino.h>
#include <ArduinoJson.h>
//Отключаем double в ArduinoJson и таблицы в Settings для экономии flash
#define ARDUINOJSON_USE_DOUBLE 0
#define SETT_NO_TABLE
//Цвета для светодиода (hex RGB) — strip.setPixelColor(0, color) на пин PIN_DATA
#define PIN_DATA 0
#define NUMPIXELS 1  //для Adafruit NeoPixel
#define RED   0xFF0000
#define BLUE  0x0000FF
#define GREEN 0x00FF00
#define YELLOW 0xFFFF00
#define LIME  0x64FF00  // 100, 255, 0

#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#define RST_PIN 1
#define SS_PIN 5

extern MFRC522DriverPinSimple ss_pin;
extern MFRC522DriverSPI driver;
extern MFRC522 mfrc522;

#include <GyverDBFile.h>
#include <LittleFS.h>
extern GyverDBFile db;

#include <SettingsGyver.h>
extern SettingsGyver sett;

extern sets::Logger logger;
extern WiFiServer server;

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
  sw_mmu,
  id,
  filament,
  nfc_id,
  nfc_name,
  nfc_brand,
  nfc_color,
  nfc_uid,    //полный UID последней прочитанной метки (D18: поле «UID» на главной)
  //Подменю «Записать метку» (v1.3): список катушек и поля режима записи
  spool_sel,  //индекс выбранной опции Select (НЕ id катушки — см. spool_ids)
  spool_ids,  //csv id катушек параллельно опциям Select
  spool_opts, //опции Select через ';'
  f_notag,    //фильтр «только катушки без метки»
  w_id,
  w_uid,
  w_type,
  w_brand,
  btn_write,
  btn_erase
);

extern bool LED_SP;
extern bool LED_MN;
extern bool LED_NFC;
extern int curentID;
extern bool wifiSettingMode;
extern bool fl_status;
extern bool fl_led;
extern bool mmu;
extern uint32_t lastUID;
//Режим записи (D10): true — активация катушки, MMU и tag/scan заблокированы
extern bool writeMode;
//Страницы веб-интерфейса v1.3.1 (D18): главная страница + два меню. Tabs не используются —
//переключение меню мгновенное, режим записи включается входом в меню «Записать метку».
#define PAGE_MAIN 0
#define PAGE_WRITE 1
#define PAGE_SETTINGS 2
extern uint8_t activePage;  //страница, открытая в вебморде (держит writeMode)
//Максимум опций в списке катушек (Select): больше — усечение + лог
#define N_OPT_MAX 60

//Машина записи v1.3 (D16): IDLE — ничего не ждём; WAIT_TAG — ждём метку после кнопки;
//WRITING/ERASING — идёт запись/стирание.
enum WriteState { IDLE, WAIT_TAG, WRITING, ERASING };
extern WriteState wstate;
//Намерение ожидания: false — «Записать метку» (T3), true — «Стереть метку» (D9/D14);
//кнопка кладёт wstate=WAIT_TAG, а по касанию метки решает, какую машину запускать.
extern bool erasePending;
//Таймаут ожидания метки после нажатия кнопки (D16)
#define WRITE_TAG_TIMEOUT 60000
//Состояние конфликта 409 (D13): UID, по которому уже сообщили владельца, и спул-владелец.
//Повторное нажатие «Записать» на том же UID = подтверждение перелинковки.
extern String conflictUID;
extern int conflictSpool;
extern unsigned long writeStart;  //millis() начала ожидания метки
extern unsigned long lastSpoolmanFail;
extern unsigned long lastMoonrakerFail;
extern bool prevSpoolmanOk;   //предыдущее состояние Spoolman (для лога изменений)
extern bool prevMoonrakerOk;  //предыдущее состояние Moonraker (для лога изменений)

#include <TimerMs.h>
extern TimerMs T_LED_WIFI;
extern TimerMs ResetLED;
extern TimerMs WatchdogNFC;
extern TimerMs T_LED_WRITE;  //мигание «ждём метку» (250 мс, D16)

#define RETRY_DELAY 30000  //Пауза между повторными попытками HTTP (мс)

//Объявления функций
void build(sets::Builder& b);
void update(sets::Updater& upd);
void set_led_color(uint32_t color);
void errorServer(String error);
void resetMFRC();
void statusServer();
void curent_filament_name(int ID);
bool getIDSpool(int ID);
void SetSpool(int SetID);
void mmu_next_spoolid(int SetID);
void readNFC();

//Чтение метки v1.3: UID-first + NDEF TLV-парсер + диспетчер форматов
String formatUID();                                          //полный UID hex UPPERCASE (04CA7EAF732681)
String ndefPayload();                                        //payload NDEF-записи («» если TLV/запись не распарсились)
bool spoolIDByUID(const String& uid, int& matched);           //POST /api/v1/tag/scan; true если matched_spool_id — число
bool parseTagJSON(const String& payload, JsonDocument& doc);  //openspool (protocol/spool_id) либо FilaMan (sm_id)

//Режим записи v1.3: список катушек для Select и id выбранной катушки
bool fetchSpoolList(bool noTagOnly);  //GET /api/v1/spool?allow_archived=false -> spool_opts + spool_ids
int selectedSpoolID();               //id из spool_ids по индексу spool_sel; -1 если ничего не выбрано

//Запись метки v1.3 (D5–D9, D13–D16)
//D21: результат lock-проверки — ТРИ состояния. «Чип не опознан» — это не read-only:
//состояние чипа неизвестно, поэтому ни записи, ни HTTP-эффектов (линковка/отвязка) быть не может.
enum TagLockState {
  TAG_LOCK_WRITABLE,     //чип NTAG21x опознан, запретов записи не найдено
  TAG_LOCK_LOCKED,       //чип опознан, но запись запрещена (lock-биты / PROT / AUTH0 / CC)
  TAG_LOCK_CHIP_UNKNOWN  //тип чипа либо lock/CC не читаются — состояние неизвестно
};
TagLockState tagLockCheck();  //lock-биты/CC/PROT до записи (D7); D21 — трёхсоставный результат
bool writeNDEF(const String& payload);  //CC + TLV + запись по страницам + перечитывание для подтверждения
bool linkTagUID(const String& uid, int spoolID, String& holderID);  //POST /spool/{id}/tag; 409 -> holderID
bool unlinkTagUID(const String& uid, int spoolID);                  //DELETE /spool/{id}/tag/{uid} (D13/D9)
bool eraseTag();                                                    //D9/D14: zero-fill + пустой NDEF + отвязка UID
bool readSpoolForTag(int spoolID, String& material, String& brand, String& colorHex, int& extruderTemp, int& bedTemp);  //GET /spool/{id} для D6
bool writeTick();  //таймаут ожидания метки + мигание (true — LED ведёт ожидание, updateLED не нужен)
