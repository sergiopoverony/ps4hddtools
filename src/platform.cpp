#include "platform.h"
#include "log.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
extern "C" int sceNetCtlInit(void);
extern "C" int sceNetCtlGetInfo(int code, void* info);
#endif

std::string local_ip() {
#ifdef HOST_TEST
    return "";
#else
    // Как в других payload для PS4: адрес консоли спрашиваем у системной службы сети.
    sceNetCtlInit();  // если уже инициализирована, вернёт ошибку: это нормально
    char info[512];
    memset(info, 0, sizeof(info));
    if (sceNetCtlGetInfo(14 /* SCE_NET_CTL_INFO_IP_ADDRESS */, info) != 0) return "";
    info[15] = 0;  // ip_address[16]
    std::string ip = info;
    if (ip.empty() || ip == "0.0.0.0" || ip.compare(0, 4, "127.") == 0) return "";
    return ip;
#endif
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
