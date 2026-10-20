#ifndef SMART_H
#define SMART_H

#include <cstdint>

// Общий лог приложения; сообщения SMART также пишутся в log.txt и stdout.
void ps4smart_logf(const char *fmt, ...);

struct SmartData {
    uint8_t data[512];
};

// Читает SMART READ DATA через CAM pass-устройство (например, /dev/pass1).
// Возвращает 0 при успешном чтении, -1 при ошибке.
int read_smart_from_pass(const char *device_path, SmartData *out_data);
void parse_smart_output(const SmartData *data);

#endif
