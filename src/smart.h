#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SmartAttr {
    uint8_t id = 0, flags = 0, current = 0, worst = 0, thresh = 0;
    uint8_t raw[6] = {0};
    uint64_t raw64 = 0;
    int sev = 0;  // 0 норма, 1 внимание, 2 плохо
};

struct SmartReason {
    int sev;       // 1 внимание, 2 плохо
    std::string code;  // status_fail, thr, attr, temp
    int id;        // номер показателя (или 0)
    int64_t value; // значение для подстановки в текст
};

struct SmartReport {
    bool have_identify = false, have_smart = false;
    std::string model, firmware, serial, bridge;
    std::string proto;       // "SAT", "ATA"
    uint64_t sectors = 0;    // ёмкость в секторах по 512 Б (IDENTIFY)
    int rpm = -1;            // -1 неизвестно, 1 = SSD, иначе обороты
    int status = -1;         // SMART RETURN STATUS: 0 исправен, 1 скоро откажет, -1 неизвестно
    std::vector<SmartAttr> attrs;
    int temp_c = -1;
    int64_t power_on_hours = -1, power_cycles = -1;
    int64_t host_read_bytes = -1, host_write_bytes = -1;  // только для известных моделей
    int verdict = -1;        // -1 нет данных, 0 здоров, 1 внимание, 2 плохо
    std::vector<SmartReason> reasons;
    std::string error;       // если не удалось прочитать
};

// Читает со служебного устройства /dev/passN: IDENTIFY, SMART DATA, пороги, статус.
// Только команды чтения (0xEC, 0xB0/0xD0, 0xB0/0xD1, 0xB0/0xDA). Ничего не записывает.
// Возвращает true, если получен хотя бы IDENTIFY. SONY-устройства пропускаются без команд.
bool smart_query(const char* pass_path, SmartReport& out);

// Разбор 512 байт SMART DATA (+ необязательно 512 байт порогов) и оценка. Не трогает диск.
void smart_parse(const uint8_t smart[512], const uint8_t* thresh, SmartReport& rep);
void smart_assess(SmartReport& rep);
const char* smart_attr_name(uint8_t id);
