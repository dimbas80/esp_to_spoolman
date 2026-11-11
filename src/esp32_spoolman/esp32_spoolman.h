// esp32_spoolman.h — все объявления и глобальные переменные
#pragma once

//Принудительно включаем USB Serial на ESP32-C3
#ifndef ARDUINO_USB_CDC_ON_BOOT
#define ARDUINO_USB_CDC_ON_BOOT 1
#endif
#ifndef ARDUINO_USB_MODE
#define ARDUINO_USB_MODE 1
#endif

#define VER "1.2"
#define SW_WIFI 6

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
  nfc_color
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
extern unsigned long lastSpoolmanFail;
extern unsigned long lastMoonrakerFail;
extern bool prevSpoolmanOk;   //предыдущее состояние Spoolman (для лога изменений)
extern bool prevMoonrakerOk;  //предыдущее состояние Moonraker (для лога изменений)

#include <TimerMs.h>
extern TimerMs T_LED_WIFI;
extern TimerMs ResetLED;
extern TimerMs WatchdogNFC;

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
