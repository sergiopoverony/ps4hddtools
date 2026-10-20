#ifndef PS4SMART_SCAN_H
#define PS4SMART_SCAN_H

// Полное чтение поверхности диска без записи.
// log_line получает одну строку лога. Устройство указывается явно.
int run_surface_scan(const char *device_path, void (*log_line)(const char *));

#endif
