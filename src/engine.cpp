#include "engine.h"
#include "disks.h"
#include "log.h"
#include "platform.h"
#include "version.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <fcntl.h>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
static double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

enum class Job { Idle, Refresh, Bench, Scan };

static const size_t kChunk = 1024 * 1024;          // 1 МиБ за одно чтение
static const int kMapCells = 500;                  // ячеек в карте
static const size_t kMaxBadLba = 100;
static const double kBlockBudgetSec = 60.0;        // на разбор одного плохого блока
static const double kWindowSec = 30.0;             // окно для скорости и оставшегося времени

struct ScanInfo {
    bool present = false;
    std::string disk, state, msg;
    uint64_t from = 0, to = 0, done = 0;
    double speed = 0, elapsed = 0;
    int64_t eta = -1;
    uint64_t hist[8] = {0};  // [1..6] время чтения блока, [7] ошибка
    uint64_t bad_sectors = 0;
    bool bad_approx = false;
    std::vector<uint64_t> bad_lba;
    std::string map;
};

struct EstInfo {
    bool present = false, ok = false;
    std::string disk;
    uint64_t from = 0, to = 0;
    double speed = 0, s0 = 0, s1 = 0, s2 = 0;
    int64_t seconds = 0;
};

static std::mutex g_m;
static std::vector<DiskInfo> g_disks;
static bool g_loaded = false;
static Job g_job = Job::Idle;
static std::thread g_worker;
static std::atomic<bool> g_pause{false}, g_stop{false};
static ScanInfo g_sc;
static EstInfo g_est;

void engine_init() {}

// ---------------------------------------------------------------- чтение устройства

#ifdef HOST_TEST
// Для тестов: HDD_FAULTS="от-до,от-до" (байты) даёт EIO; HDD_DELAY_US задерживает каждое чтение.
static std::vector<std::pair<uint64_t, uint64_t>> host_faults() {
    static std::vector<std::pair<uint64_t, uint64_t>> v;
    static bool init = false;
    if (!init) {
        init = true;
        const char* e = getenv("HDD_FAULTS");
        while (e && *e) {
            unsigned long long a = 0, b = 0;
            int n = 0;
            if (sscanf(e, "%llu-%llu%n", &a, &b, &n) < 2) break;
            v.push_back({a, b});
            e += n;
            if (*e == ',') ++e;
        }
    }
    return v;
}
#endif

static ssize_t dev_pread(int fd, void* buf, size_t n, uint64_t off) {
#ifdef HOST_TEST
    static const char* d = getenv("HDD_DELAY_US");
    if (d) usleep((useconds_t)atoi(d));
    for (auto& f : host_faults())
        if (off < f.second && off + n > f.first) { errno = EIO; return -1; }
#endif
    return pread(fd, buf, n, (off_t)off);
}

// Читает до want байт; возвращает сколько прочитано (может быть меньше), -1 если ничего.
static ssize_t read_full(int fd, uint8_t* buf, size_t want, uint64_t off) {
    size_t got = 0;
    while (got < want) {
        ssize_t n = dev_pread(fd, buf + got, want - got, off + got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return got ? (ssize_t)got : -1;
        }
        if (n == 0) break;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

static int open_disk(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    return fd;
}

static uint8_t* alloc_buf() {
    void* p = mmap(nullptr, kChunk, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    return p == MAP_FAILED ? nullptr : (uint8_t*)p;
}

// ---------------------------------------------------------------- вспомогательное

static bool find_disk(const std::string& id, DiskInfo& out) {
    for (const DiskInfo& d : g_disks) if (d.id == id) { out = d; return true; }
    return false;
}

static void norm_range(const DiskInfo& d, double from_gb, double to_gb, uint64_t& from, uint64_t& to) {
    double f = from_gb > 0 ? from_gb * 1e9 : 0;
    double t = to_gb > 0 ? to_gb * 1e9 : (double)d.bytes;
    if (t > (double)d.bytes) t = (double)d.bytes;
    from = ((uint64_t)f / kChunk) * kChunk;
    to = (uint64_t)t;
    if (to < d.bytes) to = ((to + kChunk - 1) / kChunk) * kChunk;
    if (to > d.bytes) to = d.bytes;
}

static void spawn(Job j, std::function<void()> fn) {  // вызывать под g_m
    if (g_worker.joinable()) g_worker.join();  // прежнее задание уже завершилось
    g_job = j;
    g_pause = false;
    g_stop = false;
    g_worker = std::thread([fn]() {
        fn();
        std::lock_guard<std::mutex> l(g_m);
        g_job = Job::Idle;
    });
}

// ---------------------------------------------------------------- чтение дисков и SMART

bool engine_refresh() {
    std::lock_guard<std::mutex> l(g_m);
    if (g_job != Job::Idle) return false;
    spawn(Job::Refresh, []() {
        app_log("обновление: поиск дисков и чтение SMART");
        std::vector<DiskInfo> d = disks_discover();
        std::lock_guard<std::mutex> l2(g_m);
        g_disks = d;
        g_loaded = true;
    });
    return true;
}

// ---------------------------------------------------------------- замер скорости

static void bench_job(DiskInfo d, uint64_t from, uint64_t to) {
    EstInfo e;
    e.present = true;
    e.disk = d.id;
    e.from = from;
    e.to = to;
    int fd = open_disk(d.path);
    uint8_t* buf = alloc_buf();
    if (fd < 0 || !buf) {
        app_log("замер: не удалось открыть %s или выделить память", d.path.c_str());
        if (fd >= 0) close(fd);
        if (buf) munmap(buf, kChunk);
        std::lock_guard<std::mutex> l(g_m);
        g_est = e;
        return;
    }
    uint64_t span = to - from;
    uint64_t sample = std::min<uint64_t>(span, 16ULL * kChunk);
    uint64_t offs[3] = {from, from + (span > sample ? (span - sample) / 2 : 0), from + (span > sample ? span - sample : 0)};
    offs[1] = (offs[1] / kChunk) * kChunk;
    double sp[3] = {0, 0, 0};
    int ok = 0;
    for (int i = 0; i < 3 && !g_stop; ++i) {
        uint64_t done = 0;
        auto t0 = Clock::now();
        bool fail = false;
        while (done < sample && !g_stop) {
            size_t want = (size_t)std::min<uint64_t>(kChunk, sample - done);
            ssize_t n = read_full(fd, buf, want, offs[i] + done);
            if (n <= 0) { fail = true; break; }
            done += (uint64_t)n;
            keep_awake_tick();
        }
        double s = secs(t0, Clock::now());
        if (!fail && done > 0 && s > 0) { sp[i] = (double)done / s; ++ok; }
    }
    close(fd);
    munmap(buf, kChunk);
    if (ok > 0 && !g_stop) {
        double sum = sp[0] + sp[1] + sp[2];
        e.speed = sum / ok;
        e.s0 = sp[0]; e.s1 = sp[1]; e.s2 = sp[2];
        e.seconds = (int64_t)((double)span / e.speed);
        e.ok = true;
        app_log("замер %s: %.1f / %.1f / %.1f МБ/с, оценка %lld с", d.id.c_str(), sp[0] / 1e6, sp[1] / 1e6,
                sp[2] / 1e6, (long long)e.seconds);
    }
    std::lock_guard<std::mutex> l(g_m);
    g_est = e;
}

bool engine_estimate(const std::string& disk, double from_gb, double to_gb, std::string& err) {
    std::lock_guard<std::mutex> l(g_m);
    DiskInfo d;
    if (!find_disk(disk, d)) { err = "unknown_disk"; return false; }
    if (g_job != Job::Idle) { err = "busy"; return false; }
    uint64_t from, to;
    norm_range(d, from_gb, to_gb, from, to);
    if (to <= from) { err = "bad_range"; return false; }
    g_est = EstInfo();
    spawn(Job::Bench, [d, from, to]() { bench_job(d, from, to); });
    return true;
}

// ---------------------------------------------------------------- проверка поверхности

static int bucket_for_ms(double ms) {
    if (ms <= 40) return 1;
    if (ms <= 160) return 2;
    if (ms <= 400) return 3;
    if (ms <= 1600) return 4;
    if (ms <= 4800) return 5;
    return 6;
}

#ifdef HOST_TEST
int host_test_ticks();
#endif
static void scan_finish(const char* state, const char* msg, double elapsed, uint64_t done) {
#ifdef HOST_TEST
    app_log("ticks=%d", host_test_ticks());
#endif
    std::lock_guard<std::mutex> l(g_m);
    g_sc.state = state;
    g_sc.msg = msg;
    g_sc.elapsed = elapsed;
    g_sc.done = done;
    g_sc.eta = -1;
}

static void scan_job(DiskInfo d, uint64_t from, uint64_t to) {
    int fd = open_disk(d.path);
    uint8_t* buf = alloc_buf();
    if (fd < 0 || !buf) {
        app_log("проверка: не удалось открыть %s (errno=%d) или выделить память", d.path.c_str(), errno);
        if (fd >= 0) close(fd);
        if (buf) munmap(buf, kChunk);
        scan_finish("error", "open_failed", 0, from);
        return;
    }
    const uint64_t total = to - from;
    const uint32_t ss = d.sector ? d.sector : 512;
    app_log("проверка %s: %llu..%llu байт, только чтение", d.id.c_str(), (unsigned long long)from,
            (unsigned long long)to);
    notify("ps4hddtools: проверка диска %s началась", d.id.c_str());

    auto t_start = Clock::now();
    double paused_total = 0;
    Clock::time_point pause_begin = t_start;
    bool paused = false;
    auto last_tick = t_start;
    std::deque<std::pair<double, uint64_t>> win;  // (активное время, сколько прочитано)
    win.push_back({0, 0});
    double last_sample = 0;
    uint64_t pos = from;
    uint64_t bad = 0;
    bool first_bad_notified = false;
    int dead_blocks = 0;
    std::string abort_msg;

    auto active = [&]() {
        auto now = Clock::now();
        double a = secs(t_start, now) - paused_total;
        if (paused) a -= secs(pause_begin, now);
        return a;
    };
    auto tick = [&]() {
        if (secs(last_tick, Clock::now()) >= 5.0) { keep_awake_tick(); last_tick = Clock::now(); }
    };

    while (pos < to && !g_stop) {
        if (g_pause) {
            if (!paused) {
                paused = true;
                pause_begin = Clock::now();
                std::lock_guard<std::mutex> l(g_m);
                g_sc.state = "paused";
                g_sc.speed = 0;
                g_sc.eta = -1;
            }
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (paused) {
            paused_total += secs(pause_begin, Clock::now());
            paused = false;
            std::lock_guard<std::mutex> l(g_m);
            g_sc.state = "running";
        }
        size_t want = (size_t)std::min<uint64_t>(kChunk, to - pos);
        auto tb = Clock::now();
        ssize_t got = read_full(fd, buf, want, pos);
        double ms = secs(tb, Clock::now()) * 1000.0 * ((double)kChunk / (double)want);
        int bucket;
        uint64_t new_bad = 0;
        std::vector<uint64_t> new_lba;
        bool approx = false;
        if (got == (ssize_t)want) {
            bucket = bucket_for_ms(ms);
            dead_blocks = 0;
        } else {
            bucket = 7;
            uint64_t good = got > 0 ? ((uint64_t)got / ss) * ss : 0;
            int err_first = errno;
            auto bstart = Clock::now();
            uint64_t okSectors = 0;
            for (uint64_t o = good; o < want && !g_stop; o += ss) {
                if (secs(bstart, Clock::now()) > kBlockBudgetSec) {
                    uint64_t rest = (want - o + ss - 1) / ss;
                    new_bad += rest;
                    approx = true;
                    break;
                }
                size_t sl = (size_t)std::min<uint64_t>(ss, want - o);
                ssize_t r = read_full(fd, buf, sl, pos + o);
                if (r == (ssize_t)sl) { ++okSectors; continue; }
                ++new_bad;
                if (new_lba.size() < kMaxBadLba) new_lba.push_back((pos + o) / ss);
                tick();
            }
            if (okSectors == 0 && (err_first == ENXIO || err_first == ENODEV)) {
                if (++dead_blocks >= 3) abort_msg = "device_lost";
            } else {
                dead_blocks = 0;
            }
        }
        bad += new_bad;
        pos += want;
        double a = active();
        if (a - last_sample >= 0.5 || pos >= to) {
            win.push_back({a, pos - from});
            last_sample = a;
            while (win.size() > 2 && a - win[1].first >= kWindowSec) win.pop_front();
        }
        {
            std::lock_guard<std::mutex> l(g_m);
            g_sc.done = pos - from;
            g_sc.elapsed = a;
            g_sc.hist[bucket]++;
            size_t cell = (size_t)(((pos - want / 2 - from) * (uint64_t)kMapCells) / total);
            if (cell >= (size_t)kMapCells) cell = kMapCells - 1;
            char c = (char)('0' + bucket);
            if (c > g_sc.map[cell]) g_sc.map[cell] = c;
            g_sc.bad_sectors = bad;
            if (approx) g_sc.bad_approx = true;
            for (uint64_t lba : new_lba)
                if (g_sc.bad_lba.size() < kMaxBadLba) g_sc.bad_lba.push_back(lba);
            double span = a - win.front().first;
            g_sc.speed = span > 0.3 ? (double)((pos - from) - win.front().second) / span : g_sc.speed;
            g_sc.eta = g_sc.speed > 1 ? (int64_t)((double)(to - pos) / g_sc.speed) : -1;
            if (g_stop) g_sc.state = "stopping";
        }
        if (bad > 0 && !first_bad_notified) {
            first_bad_notified = true;
            notify("ps4hddtools: на диске %s найдены ошибки чтения", d.id.c_str());
        }
        if (!abort_msg.empty()) break;
        tick();
    }
    close(fd);
    munmap(buf, kChunk);
    double a = active();
    if (!abort_msg.empty()) {
        app_log("проверка %s прервана: устройство перестало отвечать", d.id.c_str());
        notify("ps4hddtools: проверка диска %s прервана: диск не отвечает", d.id.c_str());
        scan_finish("error", abort_msg.c_str(), a, pos - from);
    } else if (g_stop) {
        app_log("проверка %s остановлена на %llu байт", d.id.c_str(), (unsigned long long)(pos - from));
        scan_finish("stopped", "stopped", a, pos - from);
    } else {
        app_log("проверка %s завершена: плохих секторов %llu, время %.0f с", d.id.c_str(), (unsigned long long)bad, a);
        notify(bad ? "ps4hddtools: проверка диска %s завершена, найдены ошибки: %llu секторов"
                   : "ps4hddtools: проверка диска %s завершена, ошибок нет",
               d.id.c_str(), (unsigned long long)bad);
        scan_finish("done", bad ? "done_errors" : "done_ok", a, total);
    }
}

bool engine_scan_start(const std::string& disk, double from_gb, double to_gb, std::string& err) {
    std::lock_guard<std::mutex> l(g_m);
    DiskInfo d;
    if (!find_disk(disk, d)) { err = "unknown_disk"; return false; }
    if (g_job != Job::Idle) { err = "busy"; return false; }
    uint64_t from, to;
    norm_range(d, from_gb, to_gb, from, to);
    if (to <= from) { err = "bad_range"; return false; }
    g_sc = ScanInfo();
    g_sc.present = true;
    g_sc.disk = d.id;
    g_sc.state = "running";
    g_sc.from = from;
    g_sc.to = to;
    g_sc.map.assign(kMapCells, '0');
    spawn(Job::Scan, [d, from, to]() { scan_job(d, from, to); });
    return true;
}

void engine_scan_pause() {
    std::lock_guard<std::mutex> l(g_m);
    if (g_job == Job::Scan) g_pause = true;
}
void engine_scan_resume() {
    std::lock_guard<std::mutex> l(g_m);
    if (g_job == Job::Scan) g_pause = false;
}
void engine_scan_stop() {
    std::lock_guard<std::mutex> l(g_m);
    if (g_job == Job::Scan || g_job == Job::Bench) { g_stop = true; g_pause = false; }
}

void engine_shutdown() {
    g_stop = true;
    g_pause = false;
    std::thread t;
    {
        std::lock_guard<std::mutex> l(g_m);
        t = std::move(g_worker);
    }
    if (t.joinable()) t.join();
}

// ---------------------------------------------------------------- JSON

static void jstr(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    o += '"';
}
static void jnum(std::string& o, double v) {
    char b[48];
    snprintf(b, sizeof(b), "%.3f", v);
    o += b;
}
static void ju(std::string& o, uint64_t v) { o += std::to_string((unsigned long long)v); }
static void ji(std::string& o, int64_t v) { o += std::to_string((long long)v); }

static const char* job_name(Job j) {
    switch (j) {
        case Job::Refresh: return "refresh";
        case Job::Bench: return "bench";
        case Job::Scan: return "scan";
        default: return "idle";
    }
}

std::string engine_state_json() {
    std::lock_guard<std::mutex> l(g_m);
    std::string o = "{\"v\":\"" APP_VERSION "\",\"job\":\"";
    o += job_name(g_job);
    o += "\",\"loaded\":";
    o += g_loaded ? "true" : "false";
    o += ",\"paused\":";
    o += g_pause ? "true" : "false";
    o += ",\"est\":";
    if (!g_est.present) o += "null";
    else {
        o += "{\"disk\":"; jstr(o, g_est.disk);
        o += ",\"ok\":"; o += g_est.ok ? "true" : "false";
        o += ",\"from\":"; ju(o, g_est.from);
        o += ",\"to\":"; ju(o, g_est.to);
        o += ",\"speed\":"; jnum(o, g_est.speed);
        o += ",\"s\":["; jnum(o, g_est.s0); o += ','; jnum(o, g_est.s1); o += ','; jnum(o, g_est.s2);
        o += "],\"seconds\":"; ji(o, g_est.seconds);
        o += '}';
    }
    o += ",\"scan\":";
    if (!g_sc.present) o += "null";
    else {
        const ScanInfo& s = g_sc;
        o += "{\"disk\":"; jstr(o, s.disk);
        o += ",\"state\":"; jstr(o, s.state);
        o += ",\"msg\":"; jstr(o, s.msg);
        o += ",\"from\":"; ju(o, s.from);
        o += ",\"to\":"; ju(o, s.to);
        o += ",\"done\":"; ju(o, s.done);
        o += ",\"speed\":"; jnum(o, s.speed);
        o += ",\"eta\":"; ji(o, s.eta);
        o += ",\"elapsed\":"; jnum(o, s.elapsed);
        o += ",\"bad\":"; ju(o, s.bad_sectors);
        o += ",\"approx\":"; o += s.bad_approx ? "true" : "false";
        o += ",\"hist\":[";
        for (int i = 1; i <= 7; ++i) { if (i > 1) o += ','; ju(o, s.hist[i]); }
        o += "],\"badlba\":[";
        for (size_t i = 0; i < s.bad_lba.size(); ++i) { if (i) o += ','; ju(o, s.bad_lba[i]); }
        o += "],\"map\":"; jstr(o, s.map);
        o += '}';
    }
    o += '}';
    return o;
}

static void smart_json(std::string& o, const SmartReport& r) {
    o += "{\"model\":"; jstr(o, r.model);
    o += ",\"fw\":"; jstr(o, r.firmware);
    o += ",\"serial\":"; jstr(o, r.serial);
    o += ",\"bridge\":"; jstr(o, r.bridge);
    o += ",\"proto\":"; jstr(o, r.proto);
    o += ",\"sectors\":"; ju(o, r.sectors);
    o += ",\"rpm\":"; ji(o, r.rpm);
    o += ",\"status\":"; ji(o, r.status);
    o += ",\"verdict\":"; ji(o, r.verdict);
    o += ",\"temp\":"; ji(o, r.temp_c);
    o += ",\"poh\":"; ji(o, r.power_on_hours);
    o += ",\"cycles\":"; ji(o, r.power_cycles);
    o += ",\"hr\":"; ji(o, r.host_read_bytes);
    o += ",\"hw\":"; ji(o, r.host_write_bytes);
    o += ",\"err\":"; jstr(o, r.error);
    o += ",\"reasons\":[";
    for (size_t i = 0; i < r.reasons.size(); ++i) {
        if (i) o += ',';
        o += "{\"s\":"; ji(o, r.reasons[i].sev);
        o += ",\"c\":"; jstr(o, r.reasons[i].code);
        o += ",\"id\":"; ji(o, r.reasons[i].id);
        o += ",\"v\":"; ji(o, r.reasons[i].value);
        o += '}';
    }
    o += "],\"attrs\":[";
    for (size_t i = 0; i < r.attrs.size(); ++i) {
        const SmartAttr& a = r.attrs[i];
        if (i) o += ',';
        char raw[16];
        snprintf(raw, sizeof(raw), "%02X%02X%02X%02X%02X%02X", a.raw[5], a.raw[4], a.raw[3], a.raw[2], a.raw[1], a.raw[0]);
        o += "{\"id\":"; ji(o, a.id);
        o += ",\"n\":"; jstr(o, smart_attr_name(a.id));
        o += ",\"c\":"; ji(o, a.current);
        o += ",\"w\":"; ji(o, a.worst);
        o += ",\"t\":"; ji(o, a.thresh);
        o += ",\"rx\":\""; o += raw;
        o += "\",\"rv\":"; ju(o, a.raw64);
        o += ",\"s\":"; ji(o, a.sev);
        o += '}';
    }
    o += "]}";
}

std::string engine_disks_json() {
    std::lock_guard<std::mutex> l(g_m);
    std::string o = "{\"loaded\":";
    o += g_loaded ? "true" : "false";
    o += ",\"disks\":[";
    for (size_t i = 0; i < g_disks.size(); ++i) {
        const DiskInfo& d = g_disks[i];
        if (i) o += ',';
        o += "{\"id\":"; jstr(o, d.id);
        o += ",\"path\":"; jstr(o, d.path);
        o += ",\"label\":"; jstr(o, d.label);
        o += ",\"bytes\":"; ju(o, d.bytes);
        o += ",\"sector\":"; ju(o, d.sector);
        o += ",\"smart\":";
        if (d.has_smart) smart_json(o, d.smart); else o += "null";
        o += '}';
    }
    o += "]}";
    return o;
}
