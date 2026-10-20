#include "http.h"
#include "assets_gen.h"
#include "engine.h"
#include "log.h"
#include "platform.h"
#include "version.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

static const int kMaxClients = 8;
static const size_t kMaxHeader = 16 * 1024;
static const size_t kMaxBody = 4096;
static std::atomic<int> g_clients{0};
static std::atomic<bool> g_quit{false};

static std::string quit_flag_path() { return std::string(DATA_DIR) + "/quit.flag"; }

static bool send_all(int fd, const char* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = send(fd, p + off, n - off, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        off += (size_t)r;
    }
    return true;
}

static void respond(int fd, int code, const char* ctype, const char* body, size_t len, bool cache = false) {
    const char* reason = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden"
                       : code == 404 ? "Not Found" : code == 409 ? "Conflict" : "Error";
    char head[320];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
                     "X-Content-Type-Options: nosniff\r\nCache-Control: %s\r\n\r\n",
                     code, reason, ctype, len, cache ? "max-age=86400" : "no-store");
    if (send_all(fd, head, (size_t)n)) send_all(fd, body, len);
}
static void respond_json(int fd, int code, const std::string& s) { respond(fd, code, "application/json", s.data(), s.size()); }
static void respond_err(int fd, int code, const char* e) { respond_json(fd, code, std::string("{\"error\":\"") + e + "\"}"); }

static std::string json_str(const std::string& body, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = body.find(k);
    if (p == std::string::npos) return "";
    p = body.find(':', p + k.size());
    if (p == std::string::npos) return "";
    size_t q1 = body.find('"', p), q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return body.substr(q1 + 1, q2 - q1 - 1);
}
static double json_num(const std::string& body, const char* key, double def) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = body.find(k);
    if (p == std::string::npos) return def;
    p = body.find(':', p + k.size());
    if (p == std::string::npos) return def;
    ++p;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
    char* end = nullptr;
    double v = strtod(body.c_str() + p, &end);
    return end == body.c_str() + p ? def : v;
}

static void handle(int fd) {
    std::string req;
    char buf[2048];
    size_t head_end = std::string::npos;
    while (head_end == std::string::npos && req.size() < kMaxHeader) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return; }  // молчун или обрыв (таймаут сработал)
        req.append(buf, (size_t)n);
        head_end = req.find("\r\n\r\n");
    }
    if (head_end == std::string::npos) { respond_err(fd, 400, "bad_request"); close(fd); return; }

    std::string head = req.substr(0, head_end);
    std::string lower = head;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) { respond_err(fd, 400, "bad_request"); close(fd); return; }
    std::string method = head.substr(0, sp1), path = head.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qp = path.find('?');
    std::string query = qp == std::string::npos ? "" : path.substr(qp + 1);
    if (qp != std::string::npos) path.resize(qp);

    std::string body;
    if (method == "POST") {
        // Защита от запросов с чужих сайтов: такой заголовок браузер без разрешения не отправит.
        if (lower.find("x-requested-with: ps4hdd") == std::string::npos) {
            respond_err(fd, 403, "forbidden");
            close(fd);
            return;
        }
        size_t cl = 0, cp = lower.find("content-length:");
        if (cp != std::string::npos) cl = (size_t)strtoul(lower.c_str() + cp + 15, nullptr, 10);
        if (cl > kMaxBody) { respond_err(fd, 400, "too_large"); close(fd); return; }
        body = req.substr(head_end + 4);
        while (body.size() < cl) {
            ssize_t n = recv(fd, buf, std::min(sizeof(buf), cl - body.size()), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            body.append(buf, (size_t)n);
        }
    }

    if (method == "GET") {
        if (path == "/" || path == "/index.html")
            respond(fd, 200, "text/html; charset=utf-8", (const char*)ASSET_INDEX_HTML, ASSET_INDEX_HTML_LEN);
        else if (path == "/logo.png")
            respond(fd, 200, "image/png", (const char*)ASSET_TITLE_PNG, ASSET_TITLE_PNG_LEN, true);
        else if (path == "/qr_paypal.png")
            respond(fd, 200, "image/png", (const char*)ASSET_QR_PAYPAL_PNG, ASSET_QR_PAYPAL_PNG_LEN, true);
        else if (path == "/qr.png")
            respond(fd, 200, "image/png", (const char*)ASSET_QR_PNG, ASSET_QR_PNG_LEN, true);
        else if (path == "/favicon.png" || path == "/favicon.ico")
            respond(fd, 200, "image/png", (const char*)ASSET_ICON_PNG, ASSET_ICON_PNG_LEN, true);
        else if (path == "/api/state") respond_json(fd, 200, engine_state_json());
        else if (path == "/api/disks") respond_json(fd, 200, engine_disks_json());
        else if (path == "/api/log") {
            size_t n = 8192;
            size_t p = query.find("n=");
            if (p != std::string::npos) n = std::min<size_t>(32768, (size_t)strtoul(query.c_str() + p + 2, nullptr, 10));
            std::string t = log_tail(n ? n : 8192);
            respond(fd, 200, "text/plain; charset=utf-8", t.data(), t.size());
        } else respond_err(fd, 404, "not_found");
    } else if (method == "POST") {
        std::string err;
        double from = json_num(body, "from_gb", 0), to = json_num(body, "to_gb", 0);
        std::string disk = json_str(body, "disk");
        if (path == "/api/refresh") {
            if (engine_refresh()) respond_json(fd, 200, "{\"ok\":true}"); else respond_err(fd, 409, "busy");
        } else if (path == "/api/scan/estimate") {
            if (engine_estimate(disk, from, to, err)) respond_json(fd, 200, "{\"ok\":true}");
            else respond_err(fd, err == "busy" ? 409 : 400, err.c_str());
        } else if (path == "/api/scan/start") {
            if (engine_scan_start(disk, from, to, err)) respond_json(fd, 200, "{\"ok\":true}");
            else respond_err(fd, err == "busy" ? 409 : 400, err.c_str());
        } else if (path == "/api/scan/pause") { engine_scan_pause(); respond_json(fd, 200, "{\"ok\":true}"); }
        else if (path == "/api/scan/resume") { engine_scan_resume(); respond_json(fd, 200, "{\"ok\":true}"); }
        else if (path == "/api/scan/stop") { engine_scan_stop(); respond_json(fd, 200, "{\"ok\":true}"); }
        else if (path == "/quit") {
            respond_json(fd, 200, "{\"ok\":true}");
            g_quit = true;
        } else respond_err(fd, 404, "not_found");
    } else respond_err(fd, 404, "not_found");
    close(fd);
}

static void client_thread(int fd) {
    handle(fd);
    --g_clients;
}

static int make_listener(int port, bool& in_use) {
    in_use = false;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr*)&a, sizeof(a)) < 0) {
        in_use = (errno == EADDRINUSE);
        close(s);
        return -1;
    }
    if (listen(s, 8) < 0) { close(s); return -1; }
    return s;
}

int http_run(int port) {
    mkdir(DATA_DIR, 0777);
    unlink(quit_flag_path().c_str());  // устаревший флаг от прошлого запуска
    bool in_use = false;
    int srv = make_listener(port, in_use);
    if (srv < 0 && in_use) {
        // Уже работает другая копия: просим её завершиться (файл-флаг) и ждём освобождения порта.
        app_log("порт %d занят: найдена старая копия, жду 3 с и прошу её завершиться", port);
        std::this_thread::sleep_for(std::chrono::seconds(3));  // на случай, если консоль притормаживает
        FILE* f = fopen(quit_flag_path().c_str(), "w");
        if (f) { fputs("quit\n", f); fclose(f); }
        for (int i = 0; i < 180 && srv < 0; ++i) {  // до ~90 с
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            srv = make_listener(port, in_use);
        }
        unlink(quit_flag_path().c_str());
        if (srv >= 0) notify("ps4hddtools перезапущен (старая копия закрыта)");
    }
    if (srv < 0) {
        app_log("не удалось занять порт %d (errno=%d %s)", port, errno, strerror(errno));
        notify("ps4hddtools: не удалось занять порт %d", port);
        return -2;
    }
    app_log("веб-интерфейс слушает порт %d", port);

    for (;;) {
        pollfd p;
        p.fd = srv;
        p.events = POLLIN;
        p.revents = 0;
        int r = poll(&p, 1, 1000);
        if (g_quit) break;
        struct stat st;
        if (stat(quit_flag_path().c_str(), &st) == 0) {
            app_log("получена просьба завершиться от новой копии");
            unlink(quit_flag_path().c_str());
            break;
        }
        if (r <= 0) continue;
        int c = accept(srv, nullptr, nullptr);
        if (c < 0) {
            if (errno == EMFILE || errno == ENFILE || errno == ENOMEM)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        timeval rt = {5, 0}, wt = {10, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &wt, sizeof(wt));
        if (g_clients.load() >= kMaxClients) { close(c); continue; }
        ++g_clients;
        try {
            std::thread(client_thread, c).detach();
        } catch (...) {
            --g_clients;
            close(c);
        }
    }
    close(srv);
    for (int i = 0; i < 30 && g_clients.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}
