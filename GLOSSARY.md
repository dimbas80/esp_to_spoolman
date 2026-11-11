# Глоссарий проекта ESP32_to_Spoolman

| Термин | Значение |
|---|---|
| **Spoolman** | Сервис учёта катушек (http://192.168.1.67:7912, v0.27.1). API `/api/v1/...`. |
| **Moonraker** | Сервер Klipper на принтере (Qidi Q1). Активная катушка: `POST /server/spoolman/spool_id`. |
| **Happy Hare / MMU** | Мультиматериальный блок; затворы: `MMU_GATE_MAP NEXT_SPOOLID=<id>`. |
| **openspool-формат** | NDEF-запись `application/json` с payload `{"protocol":"openspool","version":"1.0","type","color_hex","brand","min_temp","max_temp",...}`. Наше расширение (как у Spool Painter): ключ **`spool_id`** — ID катушки в Spoolman. |
| **FilaMan-формат (legacy)** | Прежний формат меток: JSON с ключом `sm_id`. Читаем для совместимости, не пишем. |
| **OpenTag3D** | Бинарный формат меток (NDEF `application/opentag3d`, карта памяти с фиксированными смещениями). Отложен до v1.4. |
| **UID-линковка** | Привязка UID метки к катушке в Spoolman: `POST /api/v1/spool/{id}/tag` (или UI «Add tag»). Работает даже с запертыми вендорскими метками. |
| **tag/scan** | `POST /api/v1/tag/scan` — читатель POSTит UID, Spoolman отвечает `matched_spool_id` и броадкастит в браузеры (auto-navigate). С v0.27.0. |
| **lock-биты / PROT** | NTAG-защита от записи (стр. 2 статическая, стр. 130 NTAG215 / 226 NTAG216 динамическая + пароль). Проверяются писателем перед записью. |
| **Spool Painter** | Android-приложение, читает/пишет openspool-формат (включая `spool_id`). Точка совместимости. |
| **MFRC522** | Ридер на устройстве (SPI, ISO14443A). Пины: SCK=2, MISO=3, MOSI=4, SS=5, RST=1. |
