#pragma once
// Всё, что отличается между консолью и тестом на ПК (HOST_TEST).
#ifdef HOST_TEST
#ifndef DATA_DIR
#define DATA_DIR "/tmp/ps4hddtools_test"
#endif
#ifndef DEV_DIR
#define DEV_DIR "/dev"
#endif
#else
#define DATA_DIR "/data/ps4hddtools"
#define DEV_DIR "/dev"
#endif

// IPv4-адрес консоли в домашней сети (не 127.0.0.1). Пустая строка, если определить не удалось.
#include <string>
std::string local_ip();

// Уведомление на экране консоли.
void notify(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Сбрасывает таймер автоотключения/сна. Вызывать не реже раза в ~10 с во время проверки.
void keep_awake_tick();
