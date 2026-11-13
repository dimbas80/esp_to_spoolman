// esp32_spoolman.h — все объявления и глобальные переменные
#pragma once

//Принудительно включаем USB Serial на ESP32-C3
#ifndef ARDUINO_USB_CDC_ON_BOOT
#define ARDUINO_USB_CDC_ON_BOOT 1
#endif
#ifndef ARDUINO_USB_MODE
#define ARDUINO_USB_MODE 1
#endif

#define VER "1.3.11"
#define SW_WIFI 6

//Фиксированный идентификатор читателя для Spoolman POST /api/v1/tag/scan
#define READER_ID "esp32-to-spoolman"

//Отключаем double в ArduinoJson для экономии flash — define ДО include: библиотека потребляет
//макрос в момент включения заголовков, поставленный после include он не действует (P2 ревью 1.3.9)
#define ARDUINOJSON_USE_DOUBLE 0

#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino.h>
#include <ArduinoJson.h>
//Отключаем таблицы в Settings для экономии flash (SettingsGyver включается ниже)
#define SETT_NO_TABLE
//Цвета для светодиода (hex RGB) — strip.setPixelColor(0, color) на пин PIN_DATA
#define PIN_DATA 0
#define NUMPIXELS 1  //для Adafruit NeoPixel
#define RED   0xFF0000
#define BLUE  0x0000FF
#define GREEN 0x00FF00
#define YELLOW 0xFFFF00
#define PURPLE 0x8B00FF //D31: успешная установка катушки (вместо прежнего лимонного цвета)

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
#include "log_lines.h"     //D32: разбивка потока логгера на строки (host-тестируемо)
#include "confirm_state.h" //D33: автомат подтверждения записи на залинкованную метку (host-тестируемо)
extern GyverDBFile db;

#include <SettingsGyver.h>
extern SettingsGyver sett;

//=== D32: лог в файл /log.txt ===
//FileLogger дописывает каждую строку логгера в файл на LittleFS. Файл живёт одну сессию:
//в setup() после LittleFS.begin(true) он усекается (решение владельца — лог не растёт между
//перезагрузками). RAM-буфер sets::Logger сохраняется: виджет b.Log продолжает работать.
//Файл открывается и закрывается на строку: открытый на сессию File стал бы висячим дескриптором
//при LittleFS.end() («Save & Restart») и при OTA-обновлении фронтенда.
#define LOG_PATH "/log.txt"

class FileLogger : public sets::Logger {
   public:
    FileLogger(size_t size) : sets::Logger(size) {}

    //Print::write (виртуальный) -> сюда: байт уходит и в RAM-буфер логгера, и в разбивку на строки
    size_t write(uint8_t v) override;

   private:
    void appendLine(const char* data, size_t len);
    logline::Splitter _split;
};

extern FileLogger logger;

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
  spool_sel,  //ГЛОБАЛЬНЫЙ индекс выбранной опции Select (НЕ id катушки — см. spool_ids)
  spool_ids,  //csv id катушек строго параллельно флэт-списку опций Select
  spool_opts, //опции Select: «[материал]подписи через ';'[материал2]...» (группы библиотеки)
  f_notag,    //фильтр «только катушки без метки»
  w_id,
  w_uid,
  w_type,
  w_brand,
  w_color,    //D28: color_hex метки в поле считывателя (для LED-сватча в меню записи)
  led_tag,    //D28: LED-сватч цвета метки на главной (кружок; updateColor красит кружок)
  led_tag_w,  //D28: тот же сватч в меню «Записать метку» (id обязан быть уникальным)
  btn_write,
  btn_erase,
  confirm_write, //D35 (v1.3.4): кнопка подтверждения записи на залинкованную метку (Set-ответ)
  confirm_cancel, //D35 (v1.3.4): кнопка отмены записи на залинкованную метку (Set-ответ)
  //D30: слоты MMU Gate (Label на главной): «ID 12 · PETG+ Чёрный» либо «пусто»
  gate0,
  gate1,
  gate2,
  gate3
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
//Максимум опций (катушек) в списке Select: больше — усечение + лог. Заголовки групп по материалу
//опциями не считаются (фронтенд индексов им не даёт).
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
//D33: подтверждение записи на залинкованную метку (переходы — confirm_state.h).
//Пока оно ждёт клика в вебморде, wstate остаётся WAIT_TAG: тот же таймаут и та же индикация
//ожидания, что у ожидания метки — новых состояний не вводим.
extern wconfirm::State tagConfirm;
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
extern TimerMs T_LED_AP;     //мигание синим в AP-режиме (500 мс; экс-T_LED_WIFI — имя врал, P2 ревью 1.3.9)
extern TimerMs T_SERVERS;    //опрос Moonraker/Spoolman (3 с; было 500 мс блокирующего HTTP — P2 ревью 1.3.9)
extern TimerMs ResetLED;
extern TimerMs WatchdogNFC;
extern TimerMs T_LED_WRITE;  //мигание «ждём метку» (250 мс, D16)
extern TimerMs T_GATES;      //D30: период опроса слотов MMU Gate (список в 1000 катушек — не каждый цикл)

#define RETRY_DELAY 30000  //Пауза между повторными попытками HTTP (мс)
//Единый таймаут bulk-GET списка катушек: полный список идёт дольше 2 с (D41), фильтр ArduinoJson
//экономит только память парсера, а не время передачи — все три потребителя берут эту константу
#define BULK_HTTP_TIMEOUT_MS 10000
//D31: срок свечения результата NFC-операции (чтение/запись/стирание) и период мигания ошибки
#define LED_EFFECT_MS 2000
#define LED_BLINK_MS  250

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

//Режим записи v1.3: список катушек для Select (группы по материалу) и id выбранной катушки
bool fetchSpoolList(bool noTagOnly);  //GET /api/v1/spool?allow_archived=false -> spool_opts (группы) + spool_ids
int selectedSpoolID();               //id из spool_ids по глобальному индексу spool_sel; -1 если ничего не выбрано

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
bool readSpoolForTag(int spoolID, String& material, String& brand, String& colorHex, int& extruderTemp, int& bedTemp, String& subtype);  //GET /spool/{id} для D43 (subtype из extra, D44)
bool writeTick();  //таймаут ожидания метки + мигание (true — LED ведёт ожидание, updateLED не нужен)
