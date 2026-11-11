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
//   clear() из любого состояния — отмена (выход из меню, новая метка, отмена по таймауту).
#pragma once

namespace wconfirm {

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
