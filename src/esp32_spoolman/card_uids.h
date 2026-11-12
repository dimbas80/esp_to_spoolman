// card_uids.h — кодек поля spool.extra["card_uids"] (D29 v1.3.3).
//
// Формат — как у SpoolPainter (проверено на живом Spoolman 0.27.1): значение поля
// САМО содержит кавычки по краям, UID — hex верхним регистром, разделитель — запятая
// без пробелов:
//     extra["card_uids"] == "\"04CA7EAF732681,04A1B2C3D4E5F6\""
// Кавычки здесь — часть значения поля, а не кавычки JSON: Spoolman хранит extra как
// словарь строк, поэтому «кавычки внутри значения» и есть тот формат, который читает
// и пишет SpoolPainter.
//
// Заголовок намеренно без Arduino (как spool_format.h): кодек — чистая логика строки
// поля, он должен тестироваться на хосте без шимов
// (workflows/<task>/host-tests/test_card_uids.cpp).
//
// Правила (закреплены host-тестом):
//  * чтение отбрасывает кавычки-обёртку, режет по ',', срезает пробелы, пропускает
//    пустые токены; значение без кавычек (чужой формат) разбирается так же;
//  * чужие токены сохраняются КАК ЕСТЬ (регистр не навязываем) — правка не ломает
//    данные другого инструмента;
//  * дописываемый UID нормализуется: пробелы срезаны, hex в верхнем регистре;
//  * дубль ищется без учёта регистра: повторный append ничего не меняет;
//  * если структуру нельзя переписать без потерь (токенов больше kMaxUids, токен
//    длиннее kUidLen-1) — правка ОТКЛОНЯЕТСЯ, значение не меняется.
#pragma once

#include <stdint.h>
#include <string.h>

namespace carduids {

// Сколько UID держим в одном поле. Больше — отказ от правки (не усечение!).
constexpr int kMaxUids = 32;
// 10-байтовый UID = 20 hex-символов, +1 под NUL, +запас на чужие разделители.
constexpr int kUidLen = 24;
// Значение поля целиком: токены + запятые + кавычки + NUL.
constexpr int kValueMax = kMaxUids * kUidLen + 8;

// Исход правки поля.
enum Status {
  REFUSED = -1,   // структура не разобрана/переполнена: поле не меняем
  UNCHANGED = 0,  // значение уже в нужном состоянии: PATCH не нужен
  CHANGED = 1,    // out содержит новое значение поля
};

struct List {
  char uid[kMaxUids][kUidLen];
  int count;
};

inline bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Сравнение без учёта регистра (только ASCII: UID — hex, регистр не значим).
inline bool equalsNoCase(const char* a, const char* b) {
  while (*a && *b) {
    char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 'a' + 'A') : *a;
    char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 'a' + 'A') : *b;
    if (ca != cb) return false;
    a++;
    b++;
  }
  return *a == 0 && *b == 0;
}

// Канонический UID: пробелы по краям срезаны, hex-буквы в верхнем регистре.
// false — пусто либо не влезает в буфер.
inline bool normalizeUid(const char* in, char* out, int cap) {
  if (!in || !out || cap <= 1) return false;
  const char* p = in;
  while (*p && isSpace(*p)) p++;
  const char* e = p + strlen(p);
  while (e > p && isSpace(e[-1])) e--;
  int len = (int)(e - p);
  if (len <= 0 || len >= cap) return false;
  for (int i = 0; i < len; i++) {
    char c = p[i];
    if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
    out[i] = c;
  }
  out[len] = 0;
  return true;
}

// Разбор значения поля. false — структура не разбирается без потерь (переполнение
// списка либо слишком длинный токен); в этом случае list не используется.
inline bool parse(const char* raw, List& list) {
  list.count = 0;
  if (!raw) return true;  // поля нет — пустой список

  int len = (int)strlen(raw);
  int begin = 0, end = len;
  while (begin < end && isSpace(raw[begin])) begin++;
  while (end > begin && isSpace(raw[end - 1])) end--;

  // Кавычки-обёртка формата SpoolPainter — не данные. Одиночная кавычка остаётся
  // частью токена: чинить чужое значение «на глаз» нельзя.
  if (end - begin >= 2 && raw[begin] == '"' && raw[end - 1] == '"') {
    begin++;
    end--;
  }
  if (begin >= end) return true;

  int start = begin;
  for (int i = begin; i <= end; i++) {
    if (i != end && raw[i] != ',') continue;
    int tb = start, te = i;
    while (tb < te && isSpace(raw[tb])) tb++;
    while (te > tb && isSpace(raw[te - 1])) te--;
    if (te > tb) {
      if (list.count >= kMaxUids) return false;
      if (te - tb >= kUidLen) return false;
      memcpy(list.uid[list.count], raw + tb, (size_t)(te - tb));
      list.uid[list.count][te - tb] = 0;
      list.count++;
    }
    start = i + 1;
  }
  return true;
}

// Запись в формат SpoolPainter: кавычки по краям, токены через ','. Пустой список —
// ПАРА КАВЫЧЕК ("" — валидная JSON-строка), не пустое значение: Spoolman проверяет,
// что значение extra — валидный JSON, и отвечает 400 «Value is not valid JSON» на "".
// Пустая пара кавычек читается как «меток нет» и самим кодеком (parse -> count 0).
inline bool serialize(const List& list, char* out, int cap) {
  if (!out || cap <= 2) return false;
  int needed = 2;  // две кавычки
  for (int i = 0; i < list.count; i++) needed += (int)strlen(list.uid[i]) + (i ? 1 : 0);
  if (needed + 1 > cap) return false;

  int ofs = 0;
  out[ofs++] = '"';
  for (int i = 0; i < list.count; i++) {
    if (i) out[ofs++] = ',';
    int n = (int)strlen(list.uid[i]);
    memcpy(out + ofs, list.uid[i], (size_t)n);
    ofs += n;
  }
  out[ofs++] = '"';
  out[ofs] = 0;
  return true;
}

// Дописать UID (без дублей).
inline Status add(const char* raw, const char* uid, char* out, int cap) {
  char norm[kUidLen];
  if (!normalizeUid(uid, norm, sizeof(norm))) return REFUSED;
  List list;
  if (!parse(raw, list)) return REFUSED;
  for (int i = 0; i < list.count; i++) {
    if (equalsNoCase(list.uid[i], norm)) return UNCHANGED;
  }
  if (list.count >= kMaxUids) return REFUSED;
  memcpy(list.uid[list.count], norm, strlen(norm) + 1);
  list.count++;
  return serialize(list, out, cap) ? CHANGED : REFUSED;
}

// Убрать UID (симметрия add: при стирании/отвязке).
inline Status drop(const char* raw, const char* uid, char* out, int cap) {
  char norm[kUidLen];
  if (!normalizeUid(uid, norm, sizeof(norm))) return REFUSED;
  List list;
  if (!parse(raw, list)) return REFUSED;
  int i = 0;
  while (i < list.count && !equalsNoCase(list.uid[i], norm)) i++;
  if (i == list.count) return UNCHANGED;  // такого UID в поле нет
  for (int j = i; j + 1 < list.count; j++) memcpy(list.uid[j], list.uid[j + 1], kUidLen);
  list.count--;
  return serialize(list, out, cap) ? CHANGED : REFUSED;
}

}  // namespace carduids
