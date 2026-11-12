#!/usr/bin/env bash
# Сборка релиза ESP32_to_Spoolman: в build/ остаются ровно два файла с версией в имени.
#   build/esp32_spoolman_v<VER>_full.bin  — полная прошивка (прошивать с 0x0: esptool write_flash 0x0 ...)
#   build/esp32_spoolman_v<VER>_ota.bin   — образ приложения для OTA-обновления
# Версия берётся из src/esp32_spoolman/esp32_spoolman.h (VER "..."), промежуточные
# артефакты arduino-cli живут в .build/ (в git не попадают).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/src/esp32_spoolman"
OUT="$ROOT/build"
WORK="$ROOT/.build"

VER="$(grep -oP '#define\s+VER\s+"\K[^"]+' "$SRC/esp32_spoolman.h" | head -1)"
[ -n "$VER" ] || { echo "не найдена версия (VER в esp32_spoolman.h)" >&2; exit 1; }

FQBN="esp32:esp32:esp32c3:PartitionScheme=no_ota,CDCOnBoot=cdc"

echo "== сборка v$VER (кастомная OTA-разметка) =="
arduino-cli compile \
  --fqbn "$FQBN" \
  --build-property "build.partitions=custom" \
  --build-path "$WORK" \
  "$SRC"

FULL="$WORK/esp32_spoolman.ino.merged.bin"
OTA="$WORK/esp32_spoolman.ino.bin"
[ -s "$FULL" ] || { echo "нет $FULL — полная прошивка не собралась" >&2; exit 1; }
[ -s "$OTA" ]  || { echo "нет $OTA — образ приложения не собрался" >&2; exit 1; }

# в build/ только два файла текущей версии
mkdir -p "$OUT"
rm -f "$OUT"/*.bin
cp "$FULL" "$OUT/esp32_spoolman_v${VER}_full.bin"
cp "$OTA"  "$OUT/esp32_spoolman_v${VER}_ota.bin"

echo
echo "== build/ =="
ls -l "$OUT"
sha256sum "$OUT"/*.bin
