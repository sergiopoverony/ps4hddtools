// ps4smart: read-only surface scan. No writes, repairs, or remapping commands.
#include "scan.h"
#include <cerrno>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>

// FreeBSD disk ioctls used by the current PS4 environment.
#ifndef PS4S_DIOCGSECTORSIZE
#define PS4S_DIOCGSECTORSIZE 0x40046480UL
#endif
#ifndef PS4S_DIOCGMEDIASIZE
#define PS4S_DIOCGMEDIASIZE 0x40086481UL
#endif

static void scan_log(void (*fn)(const char *), const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (fn) fn(b);
}

int run_surface_scan(const char *device_path, void (*log_line)(const char *)) {
    if (!device_path || !*device_path || !log_line) return -1;
    scan_log(log_line, "=== Проверка поверхности (только чтение) ===");
    scan_log(log_line, "Устройство: %s", device_path);
    scan_log(log_line, "ВНИМАНИЕ: сканирование полного диска может занять несколько часов.");

    int fd = open(device_path, O_RDONLY);
    if (fd < 0) {
        scan_log(log_line, "ОШИБКА: open(%s): errno=%d (%s)", device_path, errno, strerror(errno));
        return -1;
    }

    unsigned sector_size = 0;
    long long media_size = 0;
    if (ioctl(fd, PS4S_DIOCGSECTORSIZE, &sector_size) != 0 ||
        ioctl(fd, PS4S_DIOCGMEDIASIZE, &media_size) != 0 ||
        sector_size < 512 || sector_size > 65536 || media_size <= 0 ||
        media_size % sector_size != 0) {
        scan_log(log_line, "ОШИБКА: не удалось получить корректный размер диска (errno=%d: %s)", errno, strerror(errno));
        close(fd);
        return -1;
    }

    const size_t chunk_size = 1024 * 1024;
    std::vector<unsigned char> buffer(chunk_size);
    const long long total_sectors = media_size / sector_size;
    long long offset = 0, read_errors = 0, bytes_ok = 0;
    long long first_error_lba = -1;
    scan_log(log_line, "Размер: %lld байт; сектор: %u; секторов: %lld", media_size, sector_size, total_sectors);
    scan_log(log_line, "Режим: последовательное чтение блоками до 1 МиБ; диск не изменяется.");

    while (offset < media_size) {
        size_t want = (size_t)((media_size - offset) < (long long)buffer.size() ?
                               (media_size - offset) : (long long)buffer.size());
        want -= want % sector_size;
        if (!want) break;
        ssize_t got = pread(fd, buffer.data(), want, (off_t)offset);
        if (got == (ssize_t)want) {
            bytes_ok += got;
        } else {
            // Ошибочный диапазон перепроверяем по одному сектору, чтобы записать LBA.
            if (got > 0) bytes_ok += got;
            long long bad_start = offset + (got > 0 ? got : 0);
            long long bad_end = offset + (long long)want;
            for (long long pos = bad_start; pos < bad_end; pos += sector_size) {
                unsigned char sector[65536];
                ssize_t one = pread(fd, sector, sector_size, (off_t)pos);
                if (one != (ssize_t)sector_size) {
                    ++read_errors;
                    long long lba = pos / sector_size;
                    if (first_error_lba < 0) first_error_lba = lba;
                    scan_log(log_line, "ОШИБКА ЧТЕНИЯ: LBA=%lld offset=%lld errno=%d (%s)",
                             lba, pos, errno, strerror(errno));
                } else {
                    bytes_ok += sector_size;
                }
            }
        }
        offset += (long long)want;
        if (offset == media_size || (offset % (256LL * 1024 * 1024)) < (long long)want) {
            scan_log(log_line, "Прогресс: %.1f%%; прочитано %.2f / %.2f ГБ; ошибок секторов=%lld",
                     100.0 * (double)offset / (double)media_size,
                     (double)offset / 1e9, (double)media_size / 1e9, read_errors);
        }
    }
    close(fd);
    scan_log(log_line, "ИТОГ: прочитано без учёта повторных попыток %.2f ГБ; ошибок чтения=%lld",
             (double)bytes_ok / 1e9, read_errors);
    if (read_errors == 0) scan_log(log_line, "Результат: ошибок чтения не обнаружено. Это не гарантирует отсутствие всех дефектов.");
    else scan_log(log_line, "Результат: обнаружены ошибки чтения; это может быть диск, кабель или USB-SATA-мост.");
    scan_log(log_line, "Первый проблемный LBA: %lld", first_error_lba);
    scan_log(log_line, "=== Проверка поверхности завершена ===");
    return read_errors == 0 ? 0 : 1;
}
