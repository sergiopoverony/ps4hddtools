#include "diag.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

// Пробуем открыть устройство (O_RDWR для pass*, как делает служба, и O_RDONLY для da*), для da* ещё читаем
// первый сектор (только чтение). Всё это не меняет данные на диске.
static std::string try_open(const char* tag, const char* name, bool isPass, int& okCount)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", name);
    int fd = open(path, isPass ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        int e = errno;
        logf_("diag[%s]: open %s -> FAILED errno=%d (%s)", tag, path, e, strerror(e));
        char b[40];
        snprintf(b, sizeof(b), "%s=e%d", name, e);
        return b;
    }
    std::string res = "ok";
    if (!isPass) {
        unsigned char buf[512];
        ssize_t n = pread(fd, buf, sizeof(buf), 0);
        if (n == (ssize_t)sizeof(buf)) res = "ok+read";
        else {
            int e = errno;
            logf_("diag[%s]: pread %s -> %d errno=%d (%s)", tag, path, (int)n, e, strerror(e));
            char b[24];
            snprintf(b, sizeof(b), "open,read=e%d", e);
            res = b;
        }
    }
    close(fd);
    okCount++;
    logf_("diag[%s]: open %s -> %s", tag, path, res.c_str());
    return std::string(name) + "=" + res;
}

std::string diag_run(const char* tag)
{
    logf_("diag[%s]: uid=%d euid=%d gid=%d", tag, (int)getuid(), (int)geteuid(), (int)getgid());

    // Что вообще видно в /dev.
    std::vector<std::string> disks, passes;
    DIR* d = opendir("/dev");
    int total = 0;
    if (!d) {
        logf_("diag[%s]: opendir(/dev) failed errno=%d (%s)", tag, errno, strerror(errno));
    } else {
        while (struct dirent* e = readdir(d)) {
            total++;
            const char* n = e->d_name;
            bool allDigits = true;
            if (strncmp(n, "pass", 4) == 0) {
                for (const char* p = n + 4; *p; ++p) if (*p < '0' || *p > '9') allDigits = false;
                if (allDigits && n[4]) passes.push_back(n);
            } else if ((strncmp(n, "da", 2) == 0 && n[2]) || (strncmp(n, "ada", 3) == 0 && n[3])) {
                const char* p = n + (n[1] == 'd' && n[2] == 'a' ? 3 : 2);
                allDigits = *p != 0;
                for (; *p; ++p) if (*p < '0' || *p > '9') allDigits = false;
                if (allDigits) disks.push_back(n);
            }
        }
        closedir(d);
        logf_("diag[%s]: /dev has %d entries, %d pass*, %d da*/ada*", tag, total, (int)passes.size(), (int)disks.size());
    }

    int ok = 0;
    std::string line = std::string(tag) + ": ";
    for (size_t i = 0; i < passes.size() && i < 6; i++) line += try_open(tag, passes[i].c_str(), true, ok) + " ";
    for (size_t i = 0; i < disks.size() && i < 6; i++) line += try_open(tag, disks[i].c_str(), false, ok) + " ";
    if (passes.empty() && disks.empty()) line += "no pass*/da* in /dev";
    logf_("diag[%s]: summary: %s", tag, line.c_str());
    return line;
}
