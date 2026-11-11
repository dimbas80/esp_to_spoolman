// spool_format.h — форматирование подписей опций Select (D24 v1.3.2).
//
// Заголовок намеренно без Arduino: логика имён цветов и санитизации текста должна
// тестироваться на хосте (workflows/<task>/host-tests/test_palette.cpp) без шимов.
// В прошивке подключается из esp32_spoolman.cpp.
#pragma once

#include <stdint.h>
#include <string.h>

namespace spoolfmt {

// Именованный цвет фиксированной RU-палитры. В Spoolman имён цветов нет — только
// color_hex, поэтому имя подбирается по ближайшему цвету палитры (RGB-расстояние).
struct NamedColor {
  const char* name;
  uint8_t r, g, b;
};

// ~24 базовых цвета, которые реально встречаются у филамента. Порядок массива не влияет
// на результат: при равном расстоянии выбирается имя, минимальное по strcmp (детерминизм).
// Имена не содержат ';' '[' ']' '~' — символов синтаксиса строки опций Select.
static const NamedColor kPalette[] = {
  { "Чёрный",       0x00, 0x00, 0x00 },
  { "Тёмно-серый",  0x40, 0x40, 0x40 },
  { "Серый",        0x80, 0x80, 0x80 },
  { "Серебристый",  0xC0, 0xC0, 0xC0 },
  { "Белый",        0xFF, 0xFF, 0xFF },
  { "Бежевый",      0xF5, 0xDE, 0xB3 },
  { "Коричневый",   0x8B, 0x45, 0x13 },
  { "Тёмно-красный", 0x8B, 0x00, 0x00 },
  { "Красный",      0xFF, 0x00, 0x00 },
  { "Розовый",      0xFF, 0x69, 0xB4 },
  { "Пурпурный",    0xC7, 0x15, 0x85 },
  { "Сиреневый",    0xC8, 0xA2, 0xC8 },
  { "Фиолетовый",   0x80, 0x00, 0x80 },
  { "Тёмно-синий",  0x00, 0x00, 0x80 },
  { "Синий",        0x00, 0x00, 0xFF },
  { "Голубой",      0x00, 0xB0, 0xFF },
  { "Бирюзовый",    0x40, 0xE0, 0xD0 },
  { "Тёмно-зелёный", 0x00, 0x64, 0x00 },
  { "Зелёный",      0x00, 0x80, 0x00 },
  { "Лаймовый",     0x00, 0xFF, 0x00 },
  { "Оливковый",    0x80, 0x80, 0x00 },
  { "Жёлтый",       0xFF, 0xFF, 0x00 },
  { "Золотой",      0xFF, 0xD7, 0x00 },
  { "Оранжевый",    0xFF, 0x80, 0x00 },
};

constexpr int kPaletteSize = (int)(sizeof(kPalette) / sizeof(kPalette[0]));

// Шестнадцатеричная цифра -> значение; -1 — не hex.
inline int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "RRGGBB", "#RRGGBB" (Spoolman) либо "RRGGBBAA" (альфа игнорируется) -> r/g/b.
// false — не hex: пусто, nullptr, чужая длина, не-hex символ. Молча «додумывать» цвет
// нельзя: подпись опции не должна врать про катушку.
inline bool parseHexColor(const char* hex, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (!hex) return false;
  if (*hex == '#') hex++;
  size_t len = strlen(hex);
  if (len != 6 && len != 8) return false;
  int d[8];
  for (size_t i = 0; i < len; i++) {
    d[i] = hexDigit(hex[i]);
    if (d[i] < 0) return false;
  }
  r = (uint8_t)((d[0] << 4) | d[1]);
  g = (uint8_t)((d[2] << 4) | d[3]);
  b = (uint8_t)((d[4] << 4) | d[5]);
  return true;
}

// Имя ближайшего цвета палитры (квадрат евклидова расстояния в RGB). nullptr — hex не разобран.
// Равное расстояние -> имя, минимальное по strcmp: результат не зависит от порядка палитры.
inline const char* colorNameForHex(const char* hex) {
  uint8_t r, g, b;
  if (!parseHexColor(hex, r, g, b)) return nullptr;

  const NamedColor* best = nullptr;
  uint32_t bestDist = 0xFFFFFFFFu;
  for (int i = 0; i < kPaletteSize; i++) {
    const NamedColor& c = kPalette[i];
    int32_t dr = (int32_t)r - c.r;
    int32_t dg = (int32_t)g - c.g;
    int32_t db = (int32_t)b - c.b;
    uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
    if (dist < bestDist || (dist == bestDist && strcmp(c.name, best->name) < 0)) {
      best = &c;
      bestDist = dist;
    }
  }
  return best ? best->name : nullptr;
}

// Санитизация текста для строки опций Select (in place). Строка опций — плоский формат
// «[группа]опция;опция[группа2]...», поэтому данные катушки не должны нести его синтаксис:
// ';' добавила бы опцию, '[' ']' сдвинули бы группы (и индексы -> запись не той катушке),
// ведущий '~' спрятал бы опцию, '~' в заголовке — всю группу, '\n' '\r' тоже режут опции.
inline void sanitizeOptionText(char* s) {
  if (!s) return;
  for (char* p = s; *p; p++) {
    switch (*p) {
      case ';': *p = ','; break;
      case '\n':
      case '\r': *p = ' '; break;
      case '[': *p = '('; break;
      case ']': *p = ')'; break;
      case '~': *p = '-'; break;
    }
  }
}

}  // namespace spoolfmt
