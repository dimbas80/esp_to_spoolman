// confirm_state.h — D33 (v1.3.3): конечный автомат подтверждения записи на залинкованную метку.
//
// Чистый C++ без Arduino: переходы pending/confirm/timeout тестируются на хосте.
// Смысл: если UID метки уже привязан к катушке в Spoolman, запись не начинается сама —
// владелец подтверждает её в вебморде. Пока подтверждения нет, ожидание ограничено тем же
// WRITE_TAG_TIMEOUT, что и ожидание метки (D16): тишина = отмена.
//
// Переходы:
//   IDLE --begin(spoolID)--> WAIT_CONFIRM --confirm()--> CONFIRMED --(запись)--> IDLE
//                                  \--expired(now, timeout)--> IDLE (отмена)
//                                  \--reply(false), т.е. «Отмена»--> IDLE (отмена, без записи)
//   clear() из любого состояния — отмена (выход из меню, новая метка, отмена по таймауту).
//
// Ответ из вебморды разбирает reply(bool): он отделяет OK от «Отмены». Это не формальность:
// Builder::_isSet (Settings v1.3.16, core/builder.h:771) возвращает true на ЛЮБОЕ Set-действие с
// совпавшим id и само значение не смотрит — «Отмена» приходит тем же Set-пакетом со значением 0.
// D35 (v1.3.4): окно заменено ВИДИМЫМИ кнопками b.Button(id, ...) — ответ приходит тем же
// Set-действием, значение читается из b.build.value явно.
#pragma once

namespace wconfirm {

//Ответ владельца из вебморды (окно Confirm): 1 — OK, 0 — «Отмена».
//Ignore — отвечать нечему: подтверждения не ждём либо оно уже получено (второй ответ не должен
//ни отменять, ни повторять запись). Значение читается вызывающим кодом из Set-пакета явно
//(D35 v1.3.4: b.Button + b.build.value) — сам факт Set-действия подтверждением не является.
enum class Reply { Ignore, Confirm, Cancel };

class State {
   public:
    //Метка прочитана и уже залинкована (spoolID > 0). Отсчёт подтверждения — с now.
    void begin(int spoolID, unsigned long now) {
        _pending = true;
        _confirmed = false;
        _spoolID = spoolID;
        _startedAt = now;
    }

    void clear() {
        _pending = false;
        _confirmed = false;
        _spoolID = 0;
        _startedAt = 0;
    }

    bool pending() const { return _pending; }
    //Подтверждение получено и ещё не отработано: пора писать метку
    bool confirmed() const { return _pending && _confirmed; }
    int spoolID() const { return _spoolID; }

    //Клик по Confirm. false — подтверждать нечего (не ждём) или уже подтверждено:
    //повторный клик не должен запускать запись дважды.
    bool confirm() {
        if (!_pending || _confirmed) return false;
        _confirmed = true;
        return true;
    }

    //Ответ владельца из вебморды (окно Confirm): 1 — OK, 0 — «Отмена» (см. enum Reply).
    //Значение читается вызывающим кодом из Set-пакета явно — D35 (v1.3.4): b.Button + b.build.value,
    //сам факт Set-действия подтверждением не является.
    Reply reply(bool ok) const {
        if (!_pending || _confirmed) return Reply::Ignore;
        return ok ? Reply::Confirm : Reply::Cancel;
    }

    //Ожидание подтверждения истекло (отсчёт от begin, а не от нажатия кнопки: у владельца
    //должен быть полный срок на реакцию в вебморде). Подтверждённое ожидание не истекает —
    //запись уже разрешена, таймаут её не отменяет.
    bool expired(unsigned long now, unsigned long timeout) const {
        if (!_pending || _confirmed) return false;
        return (unsigned long)(now - _startedAt) > timeout;
    }

    unsigned long startedAt() const { return _startedAt; }

   private:
    bool _pending = false;
    bool _confirmed = false;
    int _spoolID = 0;
    unsigned long _startedAt = 0;
};

}  // namespace wconfirm
