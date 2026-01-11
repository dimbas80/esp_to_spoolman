# ESP_TO_SPOOLMAN
Данный проект реализует считывание NFC меток типа NTAG2xx установленных на катушках с филаментом, и установку активной катушки в сервис Spoolman через Moonraker.
## Используемые компоненты
1. Контроллер ESP32-C3 super mini
2. Считыватель NFC меток RFID-RC522
3. Светодиод WS2812B
4. Кнопка
5. Метки NTAG215, NTAG216
   
Проект выполнен для ESP32-C3 но можно установить на другой тип ESP32  с заменой пинов на соотвествующие.
Имеется web-интерфейс для настройки wifi и адресов серверов Spoolman и Moonraker. Так же там отображается состояние доступности серверов, считывателя и данные полученные с метки.

## Запись меток
Запись меток осуществляется с помощью проекта [FilaMan](https://github.com/ManuelW77/Filaman)
Метки можно применять такие NTAG215 <img width="3000" height="1998" alt="shem" src="img/ntag215.webp" />

# СХЕМА
Схема подключения модуля и ESP32

<img width="3000" height="1998" alt="shem" src="shems/shem.png" />

## Pinout ESP32-C3 super mini
<img width="3000" height="1998" alt="shem" src="shems/esp32-c3 super mini pinout.png" />
