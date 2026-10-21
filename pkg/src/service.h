#pragma once
#include <string>
#include <vector>
#include <stdint.h>

// Служба ps4hddtools (payload) живёт отдельно от приложения: приложение только запускает её
// (отправкой ELF в BinLoader GoldHEN на этой же консоли) и разговаривает с ней по HTTP.

extern const char* SERVICE_HOST;     // "127.0.0.1"
extern const int   SERVICE_PORT;     // 8686

// Спрашивает /api/state. true, если служба ответила; version = её версия.
bool service_query(std::string& version, bool& busy);

// Отправляет ELF из PKG (/app0/assets/misc/ps4hddtools.elf) в BinLoader (127.0.0.1:9090).
// Если старая копия ещё работает, новая сама попросит её завершиться.
// connectFailed (необязателен): true, если не удалось именно подключиться к порту (BinLoader выключен).
bool service_send_payload(std::string& err, bool* connectFailed = nullptr);

// Копирует payload в папку автозапуска GoldHEN (/user/data/payloads/ps4hddtools.elf) и
// прописывает его в /data/GoldHEN/payloads.ini под [AutoRun] = 1, если его там ещё нет -
// после этого GoldHEN сам поднимает службу через несколько секунд после своей активации,
// без необходимости открывать это приложение. Файл копируется заново при каждом запуске
// (чтобы на диске всегда была версия из текущего PKG); строка в ini добавляется только один
// раз - если пользователь сам её потом выключит (= 0), повторно не включаем.
bool service_install_autorun(std::string& err);

// Просит службу выключиться (POST /quit).
bool service_quit();

// Один показатель SMART (как в /api/disks: smart.attrs[]).
struct SmartAttrView {
    int id = 0;
    std::string name;   // человекочитаемое имя с сервера (n)
    int cur = 0, worst = 0, thresh = 0;
    std::string raw;    // hex-строка (rx)
    int sev = 0;        // 0 норма, 1 внимание, 2 плохо
};

// Причина вердикта (reasons[]).
struct SmartReasonView {
    int sev = 0;
    std::string code;   // status_fail, thr, attr, temp
    int id = 0;
    int64_t value = 0;
};

// Данные дисков (разбор /api/disks). Полный набор SMART-полей для экрана диска.
struct DiskView {
    std::string id, path, label, model, bridge, firmware, serial, proto;
    uint64_t bytes = 0;
    int verdict = -1;    // 0 здоров, 1 внимание, 2 плохо, -1 нет данных SMART
    int temp = 0;
    int rpm = -1;        // -1 неизвестно, 1 = SSD, иначе об/мин
    int64_t poh = -1, cycles = -1, hostRead = -1, hostWrite = -1;
    bool hasSmart = false;
    std::vector<SmartAttrView> attrs;
    std::vector<SmartReasonView> reasons;
};
struct DisksView {
    bool loaded = false;
    std::string sig;
    std::vector<DiskView> disks;
};
bool service_disks(DisksView& out);
std::string service_sig();            // пусто при ошибке
bool service_refresh_disks();         // POST /api/refresh

// Состояние сканирования поверхности (разбор /api/state: job + scan + est).
struct ScanView {
    bool present = false;
    std::string disk, state, msg;
    uint64_t from = 0, to = 0, done = 0;
    double speed = 0;
    int eta = -1;
    double elapsed = 0;
    uint64_t bad = 0;
    bool approx = false;
    uint64_t hist[8] = {0};              // индексы 1..7
    std::vector<uint64_t> badlba;
    std::string map;                     // 500 символов '0'..'7'
};
struct EstView {
    bool present = false;
    std::string disk;
    bool ok = false;
    uint64_t from = 0, to = 0;
    double speed = 0;
    int seconds = -1;
};
struct StateView {
    bool ok = false;        // true, если служба ответила и JSON разобрался
    std::string job;        // "idle", "refresh", "bench", "scan"
    bool loaded = false;
    ScanView scan;
    EstView est;
};
bool service_state(StateView& out);

// Запускает замер скорости перед сканированием (POST /api/scan/estimate).
bool service_scan_estimate(const std::string& disk, uint64_t fromGb, uint64_t toGb);
// Запускает само сканирование (POST /api/scan/start).
bool service_scan_start(const std::string& disk, uint64_t fromGb, uint64_t toGb);
bool service_scan_pause();
bool service_scan_resume();
bool service_scan_stop();

// Журнал службы (как /api/log на веб-странице): последние maxBytes байт, или "" при ошибке/недоступности.
std::string service_log(size_t maxBytes = 4096);
