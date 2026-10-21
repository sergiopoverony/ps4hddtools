#include <stdio.h>
#include <signal.h>
#include <sys/stat.h>
#include <orbis/libkernel.h>

#include "log.h"
#include "net.h"
#include "ui.h"
#include "i18n.h"
#include "service.h"

#ifndef APP_VERSION
#define APP_VERSION "0.0.0.0"
#endif

int main()
{
    signal(SIGPIPE, SIG_IGN);

    mkdir("/data/ps4hddtools_app", 0777);
    log_reset();
    logf_("---- ps4hddtools app %s start ----", APP_VERSION);

    // Устанавливаем payload в автозапуск GoldHEN (AutoRun в payloads.ini) - тогда служба
    // поднимется сама через несколько секунд после активации GoldHEN, даже без открытия
    // этого приложения. Делаем это при каждом запуске (недорого), ошибка - не фатальна,
    // служба всё равно поднимется как раньше, через BinLoader при проверке ниже.
    std::string instErr;
    if (!service_install_autorun(instErr)) logf_("autorun install failed: %s", instErr.c_str());

    net_init();
    i18n_init();
    if (!ui_init()) logf_("ui_init failed: no screen");

    for (;;) {
        ui_pump();
        sceKernelUsleep(30000);
    }
    return 0;
}
