// ps4smart: разведка диска PS4 (этап 1)
// Created by SergioPoverony and Mr.Claude. License: GPLv3.
//
// Что делает: пишет в лог версию, uid, список /dev, stat и пробное open(O_RDONLY)
// для устройств, похожих на диск (для da* только целые диски, без разделов).
// Версия 0.0.2 добавляет:
//  * 0.0.4: если код CAMIOCOMMAND (версия 0x17) не узнан, пробуется код версии 0x16 из cam_compat.h;
//  * 0.0.3: открытие pass*/xpt* ПРАВИЛЬНО: O_RDWR без O_NONBLOCK (в 0.0.2 ошибки
//    EPERM/EINVAL/ENODEV давал сам флаг, а не защита Sony). На pass* один запрос
//    XPT_GDEV_TYPE (данные из памяти системы, диску команда не посылается);
//  * для целых da*: ioctl DIOCGSECTORSIZE / DIOCGMEDIASIZE / DIOCGIDENT. Это запросы
//    к системе (размер, номер диска), диску команды не посылаются, чтения/записи нет.

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "smart.h"
#include "scan.h"
#include "check.h"
#include "http_server.h"
#include "app_state.h"
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <cstdint>
#ifndef HOST_TEST
#include <sys/param.h>
#include <sys/types.h>
#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_pass.h>
#endif
#include <sys/types.h>
#include <unistd.h>

#define APP_VERSION "0.0.5"

#ifdef HOST_TEST
#ifndef LOG_DIR
#define LOG_DIR "/tmp/ps4smart_test"
#endif
#ifndef DEV_DIR
#define DEV_DIR "/dev"
#endif
#else
#define LOG_DIR "/data/ps4smart"
#define DEV_DIR "/dev"
#endif

// ---------- уведомления ----------
#ifndef HOST_TEST
struct notify_request_t {
    int type, reqId, priority, msgId, targetId, userId, unk1, unk2, appId, errorNum, unk3;
    unsigned char useIconImageUri;
    char message[1024];
    char iconImageUri[1024];
    char unk[1024];
};
static_assert(sizeof(notify_request_t) == 3120, "notification request size");
extern "C" int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
#endif

static void notify(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#ifdef HOST_TEST
    printf("[notify] %s\n", buf);
#else
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", buf);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
#endif
}

// ---------- лог ----------
static FILE* g_log = nullptr;
static std::string g_mem;  // копия лога в памяти

static void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_mem += buf;
    g_mem += '\n';
    printf("%s\n", buf);
    if (g_log) {
        fprintf(g_log, "%s\n", buf);
        fflush(g_log);
    }
}

// Совместимый логгер для SMART-модуля. Не теряет диагностические сообщения:
// направляет их в тот же stdout, память и файл log.txt, что и остальная программа.
void ps4smart_logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logf("%s", buf);
}

static void module_log(const char* line) {
    logf("%s", line ? line : "");
}

static void open_log() {
    mkdir(LOG_DIR, 0777);
    std::string path = std::string(LOG_DIR) + "/log.txt";
    g_log = fopen(path.c_str(), "a");  // дописываем лог; предыдущие записи не удаляем
    if (g_log) {
        fseek(g_log, 0, SEEK_END);
        fprintf(g_log, "\n\n========== Новый запуск ps4hddtools ==========\n");
        fflush(g_log);
    } else {
        printf("Не удалось открыть лог %s: %s\n", path.c_str(), strerror(errno));
    }
}

// ---------- разведка ----------
// Имена, похожие на диск. Только их пробуем открыть.
bool looks_like_disk(const char* name) {
    static const char* const prefixes[] = {"ada", "da", "pass", "xpt", "sd", "nvd",
                                           "hdd", "ssd", "disk", "sata", "ata"};
    for (const char* p : prefixes) {
        size_t n = strlen(p);
        if (strncmp(name, p, n) == 0) return true;
    }
    return false;
}

static const char* type_name(mode_t m) {
    if (S_ISCHR(m)) return "chr";
    if (S_ISBLK(m)) return "blk";
    if (S_ISDIR(m)) return "dir";
    if (S_ISREG(m)) return "reg";
    if (S_ISLNK(m)) return "lnk";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISSOCK(m)) return "sock";
    return "?";
}

static void probe_device(const std::string& path) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    if (stat(path.c_str(), &st) != 0) {
        logf("  stat %s: ошибка errno=%d (%s)", path.c_str(), errno, strerror(errno));
    } else {
        logf("  stat %s: тип=%s mode=%o rdev=0x%llx", path.c_str(), type_name(st.st_mode),
             (unsigned)(st.st_mode & 07777), (unsigned long long)st.st_rdev);
    }
    // Только чтение, без блокировки; сразу закрываем. Ничего не читаем и не шлём.
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        logf("  open(O_RDONLY) %s: ошибка errno=%d (%s)", path.c_str(), errno, strerror(errno));
    } else {
        logf("  open(O_RDONLY) %s: УДАЛОСЬ (fd=%d)", path.c_str(), fd);
        close(fd);
    }
}


// Целый диск: "da" + только цифры (da0, da1), без разделов вроде da0x1 или da1s1.
bool is_whole_da(const char* n) {
    if (strncmp(n, "da", 2) != 0 || n[2] == 0) return false;
    for (const char* p = n + 2; *p; ++p)
        if (*p < '0' || *p > '9') return false;
    return true;
}

bool is_cam_node(const char* n) { return strncmp(n, "pass", 4) == 0 || strncmp(n, "xpt", 3) == 0; }

// ioctl-коды FreeBSD (sys/disk.h), прописаны вручную: _IOR('d', N, тип)
#define PS4S_DIOCGSECTORSIZE 0x40046480UL  // u_int
#define PS4S_DIOCGMEDIASIZE 0x40086481UL   // off_t (8 байт)
#define PS4S_DIOCGIDENT 0x41006489UL       // char[256]

#ifndef HOST_TEST
// Печатные символы поля фиксированной длины, без хвостовых пробелов/нулей.
static std::string clean(const void* p, size_t n) {
    const unsigned char* c = (const unsigned char*)p;
    std::string r;
    for (size_t i = 0; i < n; i++) r += (c[i] >= 32 && c[i] < 127) ? (char)c[i] : ' ';
    size_t e = r.find_last_not_of(' ');
    return e == std::string::npos ? std::string() : r.substr(0, e + 1);
}
#endif

// pass*/xpt*: нужен O_RDWR и НЕЛЬЗЯ O_NONBLOCK (иначе EINVAL/ENODEV, так устроен CAM).
// На pass* делаем один запрос XPT_GDEV_TYPE: данные берутся из памяти системы,
// команда диску не посылается. На xpt0 только открываем и закрываем.
static void probe_cam(const std::string& path, bool query) {
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) {
        logf("  open(O_RDWR) %s: ошибка errno=%d (%s)", path.c_str(), errno, strerror(errno));
        return;
    }
    logf("  open(O_RDWR) %s: УДАЛОСЬ (fd=%d)", path.c_str(), fd);
#ifdef HOST_TEST
    if (query) logf("  (HOST_TEST: запрос XPT_GDEV_TYPE пропущен)");
#else
    if (query) {
        static union ccb ccb;
        // Два кода из заголовков SDK: версия CAM 0x17 и совместимая 0x16 (cam_compat.h).
        const unsigned long codes[2] = {(unsigned long)CAMIOCOMMAND, (unsigned long)_IOWR(0x16, 2, union ccb)};
        bool ok = false;
        for (int i = 0; i < 2 && !ok; i++) {
            memset(&ccb, 0, sizeof(ccb));
            ccb.ccb_h.func_code = XPT_GDEV_TYPE;
            int r = ioctl(fd, codes[i], &ccb);
            if (r != 0) {
                logf("  %s: ioctl(0x%lx, XPT_GDEV_TYPE) ошибка errno=%d (%s) sizeof(ccb)=%d",
                     path.c_str(), codes[i], errno, strerror(errno), (int)sizeof(ccb));
                continue;
            }
            ok = true;
            struct ccb_getdev& g = ccb.cgd;
            logf("  %s: ioctl 0x%lx УДАЛСЯ. CAM status=0x%x protocol=%d (2=SCSI 3=ATA 4=ATAPI) path=%u target=%u lun=%u",
                 path.c_str(), codes[i], (unsigned)(g.ccb_h.status & CAM_STATUS_MASK), (int)g.protocol,
                 (unsigned)g.ccb_h.path_id, (unsigned)g.ccb_h.target_id, (unsigned)g.ccb_h.target_lun);
            logf("  %s: inquiry: vendor='%s' product='%s' rev='%s'", path.c_str(),
                 clean(g.inq_data.vendor, sizeof(g.inq_data.vendor)).c_str(),
                 clean(g.inq_data.product, sizeof(g.inq_data.product)).c_str(),
                 clean(g.inq_data.revision, sizeof(g.inq_data.revision)).c_str());
            logf("  %s: ata model='%s' fw='%s' serial_len=%d", path.c_str(),
                 clean(g.ident_data.model, sizeof(g.ident_data.model)).c_str(),
                 clean(g.ident_data.revision, sizeof(g.ident_data.revision)).c_str(),
                 (int)g.serial_num_len);
        }
    }
#endif
    close(fd);
}

// Целый диск: три безопасных запроса к системе (не к диску).
static void query_disk(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        logf("  %s: open ошибка errno=%d (%s)", path.c_str(), errno, strerror(errno));
        return;
    }
    unsigned int ssz = 0;
    if (ioctl(fd, PS4S_DIOCGSECTORSIZE, &ssz) == 0)
        logf("  %s: размер сектора=%u", path.c_str(), ssz);
    else
        logf("  %s: DIOCGSECTORSIZE ошибка errno=%d (%s)", path.c_str(), errno, strerror(errno));

    int64_t msz = 0;
    if (ioctl(fd, PS4S_DIOCGMEDIASIZE, &msz) == 0)
        logf("  %s: размер=%lld байт (%.1f ГБ)", path.c_str(), (long long)msz, msz / 1e9);
    else
        logf("  %s: DIOCGMEDIASIZE ошибка errno=%d (%s)", path.c_str(), errno, strerror(errno));

    char ident[256];
    memset(ident, 0, sizeof(ident));
    if (ioctl(fd, PS4S_DIOCGIDENT, ident) == 0) {
        ident[sizeof(ident) - 1] = 0;
        logf("  %s: ident='%s'", path.c_str(), ident);
    } else {
        logf("  %s: DIOCGIDENT ошибка errno=%d (%s)", path.c_str(), errno, strerror(errno));
    }
    close(fd);
}

static int run_recon() {
    logf("version %s", APP_VERSION);
    logf("ps4smart: разведка устройств и попытка чтения SMART (этап 2).");
    logf("uid=%d euid=%d", (int)getuid(), (int)geteuid());

    DIR* d = opendir(DEV_DIR);
    if (!d) {
        logf("opendir(%s) ошибка errno=%d (%s)", DEV_DIR, errno, strerror(errno));
        return 1;
    }
    std::vector<std::string> names;
    while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        names.push_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());

    logf("--- список %s (%d шт.) ---", DEV_DIR, (int)names.size());
    std::string line;
    for (const std::string& n : names) {
        if (line.size() + n.size() + 1 > 100) {
            logf("%s", line.c_str());
            line.clear();
        }
        line += n;
        line += ' ';
    }
    if (!line.empty()) logf("%s", line.c_str());

    logf("--- кандидаты на диск ---");
    int cands = 0;
    for (const std::string& n : names) {
        if (!looks_like_disk(n.c_str())) continue;
        if (strncmp(n.c_str(), "da", 2) == 0 && !is_whole_da(n.c_str())) continue;  // разделы не пробуем
        cands++;
        probe_device(std::string(DEV_DIR) + "/" + n);
    }
    if (cands == 0) logf("  ничего похожего на диск (ada*, da*, pass*, xpt*, sd*, ...) не найдено");

    logf("--- pass*/xpt*: O_RDWR без O_NONBLOCK; на pass* один запрос XPT_GDEV_TYPE ---");
    int cam = 0;
    for (const std::string& n : names) {
        if (!is_cam_node(n.c_str())) continue;
        cam++;
        probe_cam(std::string(DEV_DIR) + "/" + n, strncmp(n.c_str(), "pass", 4) == 0);
    }
    if (cam == 0) logf("  pass*/xpt* не найдены");

    logf("--- целые диски da*: размер и номер (запросы к системе) ---");
    int whole = 0;
    for (const std::string& n : names) {
        if (!is_whole_da(n.c_str())) continue;
        whole++;
        query_disk(std::string(DEV_DIR) + "/" + n);
    }
    if (whole == 0) logf("  целых da* не найдено");

    logf("--- итог: узлов в /dev=%d, кандидатов=%d ---", (int)names.size(), cands);
    logf("конец. Лог также в %s/log.txt", LOG_DIR);
    return 0;
}


static void run_startup_diagnostics() {
    open_log();
    logf("ps4hddtools 0.0.6: запуск диагностики.");
    notify("ps4hddtools: сервис запускается");
    int rc = run_recon();

    logf("");
    logf("=== Чтение SMART ===");
    SmartData smart_buf{};
    struct stat st;
    const char *smart_device = "/dev/pass1";
    if (stat(smart_device, &st) != 0) {
        logf("[-] Устройство %s не найдено: errno=%d (%s)", smart_device, errno, strerror(errno));
    } else if (read_smart_from_pass(smart_device, &smart_buf) == 0) {
        logf("[+] SMART получен, разбираю данные:");
        parse_smart_output(&smart_buf);
    } else {
        logf("[-] Не удалось считать SMART с %s.", smart_device);
    }
    logf("=== Проверка распознавания файловых систем ===");
    int check_rc = run_filesystem_check(DEV_DIR, module_log);
    logf("Результат check.cpp: код=%d", check_rc);
    logf("Автоматическое полное сканирование НЕ запускается. Запустите его вручную через веб-интерфейс.");
    notify("ps4hddtools: диагностика готова. Веб-сервис порт 8686");
    if (g_log) { fclose(g_log); g_log = nullptr; }
    (void)rc;
}

int main() {
    std::printf("ps4hddtools 0.0.6 starting\\n");
    std::printf("Web UI: http://<PS4-IP>:8686/\\n");
    notify("ps4hddtools: запуск сервиса на порту 8686");
    std::thread diagnostics(run_startup_diagnostics);
    diagnostics.detach();
    AppState state;
    int rc = run_http_server(state, "web", 8686);
    notify("ps4hddtools: HTTP-сервис завершился с кодом %d", rc);
    return rc;
}
