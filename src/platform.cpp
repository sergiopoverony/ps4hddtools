#include "platform.h"
#include "log.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
extern "C" int sceSystemServicePowerTick(void);
#endif

std::string local_ip() {
    // Адрес, с которого консоль вышла бы в сеть: UDP-сокет «подключается» к внешнему адресу
    // (пакеты не отправляются) и спрашивает свой адрес. Только сокеты, которые программа и так использует.
    std::string found;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return found;
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808);  // 8.8.8.8
    if (connect(s, (struct sockaddr*)&to, sizeof(to)) == 0) {
        struct sockaddr_in me;
        socklen_t l = sizeof(me);
        memset(&me, 0, sizeof(me));
        if (getsockname(s, (struct sockaddr*)&me, &l) == 0) {
            uint32_t a = ntohl(me.sin_addr.s_addr);
            if ((a >> 24) != 127 && a != 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255);
                found = buf;
            }
        }
    }
    close(s);
    return found;
}

void notify(const char* fmt, ...) {
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

static int g_ticks = 0;
#ifndef HOST_TEST
static bool g_tick_err = false;
#endif
int host_test_ticks() { return g_ticks; }

void keep_awake_tick() {
    ++g_ticks;
#ifndef HOST_TEST
    int r = sceSystemServicePowerTick();
    if (g_ticks == 1) app_log("защита от сна: sceSystemServicePowerTick вернул %d (0 = принято)", r);
    else if (r != 0 && !g_tick_err) { g_tick_err = true; app_log("защита от сна: sceSystemServicePowerTick вернул %d", r); }
#endif
}
