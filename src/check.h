#ifndef PS4SMART_CHECK_H
#define PS4SMART_CHECK_H

// Read-only диагностика доступных разделов и распознавание сигнатур ФС.
// Это пока НЕ ремонт и НЕ полноценная проверка целостности ФС.
int run_filesystem_check(const char *dev_dir, void (*log_line)(const char *));

#endif
