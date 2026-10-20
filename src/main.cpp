// ps4hddtools: проверка диска PS4 (SMART, проверка поверхности, веб-интерфейс).
// Created by SergioPoverony and Mr.Claude. License: GPLv3.
//
// В покое ничего не делает: ждёт обращения к веб-интерфейсу. Диски и SMART читаются
// только по запросу страницы, проверка поверхности только после подтверждения пользователя.
#include "engine.h"
#include "http.h"
#include "log.h"
#include "platform.h"
#include "version.h"
#include <cstdlib>

int main() {
    log_open(DATA_DIR);
    app_log("version %s", APP_VERSION);
    engine_init();
    int port = APP_PORT;
#ifdef HOST_TEST
    if (const char* e = getenv("HDD_PORT")) port = atoi(e);
#endif
    int rc = http_run(port);
    engine_shutdown();
    app_log("завершение, код %d", rc);
    if (rc == 0) notify("ps4hddtools: выключен");
    return rc;
}
