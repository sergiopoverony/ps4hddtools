#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

enum class ScanState { Idle, Benchmark, Running, Paused, Finished, Error, Stopping };

struct DiskInfo {
    std::string path;
    std::string name;
    uint64_t bytes = 0;
    uint32_t sector_size = 512;
};

struct AppState {
    std::mutex mutex;
    std::vector<DiskInfo> disks;
    ScanState scan_state = ScanState::Idle;
    std::string device;
    uint64_t total_bytes = 0;
    uint64_t processed_bytes = 0;
    uint64_t read_errors = 0;
    uint64_t current_lba = 0;
    uint32_t sector_size = 512;
    double speed_bps = 0.0;
    int64_t eta_seconds = -1;
    std::string message = "Ожидание";
    std::vector<uint8_t> block_map; // 0 unknown, 1 good, 2 error
    std::atomic<bool> pause_requested{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> worker_active{false};
};
