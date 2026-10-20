#pragma once
#include "smart.h"
#include <cstdint>
#include <string>
#include <vector>

struct DiskInfo {
    std::string id;     // "da0"
    std::string path;   // "/dev/da0"
    std::string pass;   // "/dev/pass1" (для SMART), может быть пусто
    std::string label;  // "USB0", "EXT0" или пусто
    uint64_t bytes = 0;
    uint32_t sector = 512;
    SmartReport smart;
    bool has_smart = false;
};

// Находит целые диски (da*, ada*), сопоставляет их со служебными pass-устройствами
// и читает SMART. Выполняется только по запросу. Только чтение.
std::vector<DiskInfo> disks_discover();
