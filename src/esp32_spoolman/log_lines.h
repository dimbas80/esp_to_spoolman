// log_lines.h — D32 (v1.3.3): разбивка потока Logger::write(uint8_t) на строки.
//
// Чистый C++ без Arduino/LittleFS: логика «где заканчивается строка» тестируется на хосте,
// а класс FileLogger только дописывает готовую строку в файл. Правила:
//   '\n'  — конец строки (пустая строка — тоже строка: "\n\n" даёт две);
//   '\r'  — отбрасывается (CRLF и одиночный CR не создают пустых строк);
//   переполнение буфера — накопленная часть отдаётся как отдельная строка, а не обрезается
//   молча: потерять хвост лога хуже, чем увидеть его разорванным.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace logline {

class Splitter {
   public:
    //Больше — уже не строка: буфер RAM-логгера 1024, реальные строки проекта короче 200
    static const size_t kLineMax = 200;

    //Байт принят. true — накоплена готовая строка: *data/*len — её текст во внутреннем буфере,
    //буфер очищен (строка отдаётся «на месте», без копирования).
    bool feed(uint8_t v, const char*& data, size_t& len) {
        if (v == '\r') return false;
        if (v == '\n') {
            data = _buf;
            len = _len;
            _len = 0;
            return true;
        }
        if (_len == kLineMax) {
            //Переполнение: текущий байт — первый байт следующей строки
            data = _buf;
            len = _len;
            _len = 0;
            _buf[_len++] = (char)v;
            return true;
        }
        _buf[_len++] = (char)v;
        return false;
    }

    //Незавершённый хвост (логгер замолчал без '\n') — отдаём как строку; вызывается при
    //явном сбросе (сейчас не используется: файл и так закрывается на каждой строке).
    bool flush(const char*& data, size_t& len) {
        if (!_len) return false;
        data = _buf;
        len = _len;
        _len = 0;
        return true;
    }

    void clear() { _len = 0; }
    size_t length() const { return _len; }

   private:
    char _buf[kLineMax];
    size_t _len = 0;
};

}  // namespace logline
