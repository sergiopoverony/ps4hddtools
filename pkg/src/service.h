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
bool service_send_payload(std::string& err);

// Доступен ли BinLoader GoldHEN (порт 9090).
bool loader_reachable();

// Просит службу выключиться (POST /quit).
bool service_quit();

// Данные дисков (разбор /api/disks).
struct DiskView {
    std::string id, label, model, bridge;
    uint64_t bytes = 0;
    int verdict = -1;    // 0 здоров, 1 внимание, 2 плохо, -1 нет данных SMART
    int temp = 0;
    bool hasSmart = false;
};
struct DisksView {
    bool loaded = false;
    std::string sig;
    std::vector<DiskView> disks;
};
bool service_disks(DisksView& out);
std::string service_sig();            // пусто при ошибке
bool service_refresh_disks();         // POST /api/refresh
