// ps4smart: read-only filesystem/partition reconnaissance.
#include "check.h"
#include <cerrno>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static void check_log(void (*fn)(const char *), const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (fn) fn(b);
}

static bool starts_with(const std::string &s, const char *p) {
    return s.compare(0, strlen(p), p) == 0;
}

static bool is_partition_candidate(const std::string &n) {
    // Учитываем типичные FreeBSD-узлы PS4: da0x1, da0x1.crypt и похожие.
    if (!starts_with(n, "da") || n.size() < 4) return false;
    if (n.find(".crypt") != std::string::npos) return false;
    return n.find('x') != std::string::npos || n.find('s') != std::string::npos ||
           n.find('p') != std::string::npos;
}

static void describe_signature(int fd, const std::string &name,
                               void (*log_line)(const char *)) {
    unsigned char b[4096];
    memset(b, 0, sizeof(b));
    ssize_t n = pread(fd, b, sizeof(b), 0);
    if (n < 0) {
        check_log(log_line, "  %s: ошибка чтения начала раздела: errno=%d (%s)", name.c_str(), errno, strerror(errno));
        return;
    }
    if (n < 512) {
        check_log(log_line, "  %s: прочитано только %lld байт; сигнатура не определена", name.c_str(), (long long)n);
        return;
    }
    const char *type = "неизвестная/зашифрованная/неподдерживаемая ФС";
    if (b[510] == 0x55 && b[511] == 0xAA) type = "MBR/загрузочная сигнатура 55AA (не доказывает тип ФС)";
    if (n >= 1082 && b[1080] == 0x53 && b[1081] == 0xEF) type = "похоже на ext2/3/4 (магия EF53)";
    if (n >= 4 && memcmp(b, "XFSB", 4) == 0) type = "похоже на XFS";
    if (n >= 4 && memcmp(b, "NTFS", 4) == 0) type = "похоже на NTFS";
    if (n >= 8 && memcmp(b + 3, "EXFAT   ", 8) == 0) type = "похоже на exFAT";
    if (n >= 4 && memcmp(b, "APFS", 4) == 0) type = "возможная сигнатура APFS (нужна дополнительная проверка)";
    check_log(log_line, "  %s: %s; прочитано %lld байт", name.c_str(), type, (long long)n);
}

int run_filesystem_check(const char *dev_dir, void (*log_line)(const char *)) {
    if (!dev_dir || !*dev_dir || !log_line) return -1;
    check_log(log_line, "=== Проверка файловой системы (только чтение) ===");
    check_log(log_line, "Каталог устройств: %s", dev_dir);
    check_log(log_line, "Режим: только чтение первых 4096 байт; исправления не выполняются.");
    DIR *d = opendir(dev_dir);
    if (!d) {
        check_log(log_line, "ОШИБКА: opendir(%s): errno=%d (%s)", dev_dir, errno, strerror(errno));
        return -1;
    }
    std::vector<std::string> names;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string n(ent->d_name);
        if (is_partition_candidate(n)) names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    if (names.empty()) {
        check_log(log_line, "Разделы-кандидаты da* не найдены.");
        check_log(log_line, "=== Проверка файловой системы завершена ===");
        return 0;
    }
    for (const std::string &name : names) {
        std::string path = std::string(dev_dir) + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISCHR(st.st_mode)) continue;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            check_log(log_line, "%s: открыть только для чтения не удалось: errno=%d (%s)", path.c_str(), errno, strerror(errno));
            continue;
        }
        check_log(log_line, "Раздел: %s", path.c_str());
        describe_signature(fd, path, log_line);
        close(fd);
    }
    check_log(log_line, "Примечание: распознавание сигнатуры — предварительное; целостность ФС не проверялась.");
    check_log(log_line, "Для полноценного fsck нужно определить точную файловую систему Sony/PS4 и её формат.");
    check_log(log_line, "=== Проверка файловой системы завершена ===");
    return 0;
}
