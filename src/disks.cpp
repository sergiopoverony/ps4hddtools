#include "disks.h"
#include "log.h"
#include "platform.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef HOST_TEST
#include <sys/param.h>
#include <sys/mount.h>
#endif

#define PS4H_DIOCGSECTORSIZE 0x40046480UL  // u_int
#define PS4H_DIOCGMEDIASIZE 0x40086481UL   // off_t

static bool all_digits(const char* p) {
    if (!*p) return false;
    for (; *p; ++p) if (*p < '0' || *p > '9') return false;
    return true;
}
static bool is_whole_disk(const char* n) {
    if (strncmp(n, "ada", 3) == 0) return all_digits(n + 3);
    if (strncmp(n, "da", 2) == 0) return all_digits(n + 2);
    return false;
}
static bool is_pass(const char* n) { return strncmp(n, "pass", 4) == 0 && all_digits(n + 4); }

static std::vector<std::string> list_dev(bool (*pred)(const char*)) {
    std::vector<std::string> v;
    DIR* d = opendir(DEV_DIR);
    if (!d) {
        app_log("opendir(%s) errno=%d (%s)", DEV_DIR, errno, strerror(errno));
        return v;
    }
    while (struct dirent* e = readdir(d))
        if (pred(e->d_name)) v.push_back(e->d_name);
    closedir(d);
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    });
    return v;
}

// Подпись «USB0» / «EXT0» по точкам монтирования консоли (если диск смонтирован).
static std::string mount_label(const std::string& disk_id) {
#ifdef HOST_TEST
    (void)disk_id;
    return "";
#else
    static const char* const mounts[] = {"/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
                                         "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
                                         "/mnt/ext0", "/mnt/ext1"};
    for (const char* m : mounts) {
        struct statfs sf;
        if (statfs(m, &sf) != 0) continue;
        const char* from = sf.f_mntfromname;
        if (strncmp(from, "/dev/", 5) == 0) from += 5;
        size_t n = disk_id.size();
        if (strncmp(from, disk_id.c_str(), n) == 0 && (from[n] == 0 || from[n] < '0' || from[n] > '9')) {
            std::string l = m + 5;
            for (char& c : l) c = (char)toupper((unsigned char)c);
            return l;
        }
    }
    return "";
#endif
}

std::vector<DiskInfo> disks_discover() {
    std::vector<DiskInfo> disks;
    for (const std::string& n : list_dev(is_whole_disk)) {
        DiskInfo d;
        d.id = n;
        d.path = std::string(DEV_DIR) + "/" + n;
        int fd = open(d.path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            app_log("диск %s: open errno=%d (%s)", d.path.c_str(), errno, strerror(errno));
            continue;
        }
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) d.bytes = (uint64_t)st.st_size;
        int64_t media = 0;
        if (ioctl(fd, PS4H_DIOCGMEDIASIZE, &media) == 0 && media > 0) d.bytes = (uint64_t)media;
        unsigned ss = 0;
        if (ioctl(fd, PS4H_DIOCGSECTORSIZE, &ss) == 0 && ss > 0) d.sector = ss;
        close(fd);
        if (d.bytes == 0) {
            app_log("диск %s: размер не определён, пропускаю", d.path.c_str());
            continue;
        }
        d.label = mount_label(d.id);
        app_log("диск %s: %llu байт, сектор %u%s%s", d.id.c_str(), (unsigned long long)d.bytes, d.sector,
                d.label.empty() ? "" : ", точка монтирования ", d.label.c_str());
        disks.push_back(d);
    }

    struct Rep { std::string pass; SmartReport r; bool used = false; };
    std::vector<Rep> reps;
    for (const std::string& n : list_dev(is_pass)) {
        Rep r;
        r.pass = std::string(DEV_DIR) + "/" + n;
        if (smart_query(r.pass.c_str(), r.r)) reps.push_back(r);
    }
    // Сопоставление по ёмкости: IDENTIFY (сектора по 512 Б) против размера da.
    for (DiskInfo& d : disks)
        for (Rep& r : reps)
            if (!r.used && r.r.sectors && r.r.sectors * 512ULL == d.bytes) {
                d.pass = r.pass; d.smart = r.r; d.has_smart = true; r.used = true;
                break;
            }
    // Запасной вариант: один диск без пары и один отчёт без пары.
    int free_disks = 0, free_reps = 0;
    for (DiskInfo& d : disks) if (!d.has_smart) ++free_disks;
    for (Rep& r : reps) if (!r.used) ++free_reps;
    if (free_disks == 1 && free_reps == 1) {
        for (DiskInfo& d : disks)
            if (!d.has_smart)
                for (Rep& r : reps)
                    if (!r.used) { d.pass = r.pass; d.smart = r.r; d.has_smart = true; r.used = true; }
        app_log("диск и pass сопоставлены по единственности (ёмкость не совпала)");
    }
    for (DiskInfo& d : disks)
        app_log("итог: %s <-> %s %s", d.id.c_str(), d.pass.empty() ? "(SMART недоступен)" : d.pass.c_str(),
                d.has_smart ? d.smart.model.c_str() : "");
    return disks;
}
