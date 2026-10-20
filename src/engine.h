#pragma once
#include <string>

// Единая очередь заданий: одновременно выполняется только одно (чтение дисков/SMART,
// замер скорости, проверка поверхности). В покое потоков нет.
void engine_init();
void engine_shutdown();

bool engine_refresh();  // найти диски и прочитать SMART
// from_gb/to_gb: диапазон в ГБ (1 ГБ = 10^9 Б); to_gb <= 0 означает «до конца диска».
bool engine_estimate(const std::string& disk, double from_gb, double to_gb, std::string& err);
bool engine_scan_start(const std::string& disk, double from_gb, double to_gb, std::string& err);
void engine_scan_pause();
void engine_scan_resume();
void engine_scan_stop();

std::string engine_state_json();
std::string engine_disks_json();
std::string engine_sig_json();  // текущая подпись набора устройств (без чтения дисков)
