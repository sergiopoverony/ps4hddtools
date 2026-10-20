#include "service.h"
#include "httpc.h"
#include "json.h"
#include "net.h"
#include "log.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>

const char* SERVICE_HOST = "127.0.0.1";
const int   SERVICE_PORT = 8686;
static const int LOADER_PORT = 9090;
static const char* PAYLOAD_PATH = "/app0/assets/misc/ps4hddtools.elf";

bool service_query(std::string& version, bool& busy)
{
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/state", 1500);
    if (!r.connected || r.status != 200) return false;
    JsonValue j;
    if (!json_parse(r.body, j) || j.type != JsonValue::OBJ) return false;
    version = j["v"].asStr();
    busy = j["job"].asStr("idle") != "idle";
    return !version.empty();
}

bool service_send_payload(std::string& err)
{
    FILE* f = fopen(PAYLOAD_PATH, "rb");
    if (!f) { err = "no payload file in PKG"; logf_("service: cannot open %s", PAYLOAD_PATH); return false; }
    std::string data;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() < 1024) { err = "payload file is empty"; return false; }

    int s = net_connect(SERVICE_HOST, LOADER_PORT, 3000);
    if (s < 0) { err = "BinLoader (port 9090) is not available. Is GoldHEN running?"; logf_("service: BinLoader connect failed"); return false; }

    size_t sent = 0;
    while (sent < data.size()) {
        int w = send(s, data.data() + sent, data.size() - sent, 0);
        if (w <= 0) { err = "sending to BinLoader failed"; logf_("service: send failed after %d of %d bytes", (int)sent, (int)data.size()); close(s); return false; }
        sent += (size_t)w;
    }
    close(s);
    logf_("service: payload sent (%d bytes)", (int)sent);
    return true;
}

bool loader_reachable()
{
    int s = net_connect(SERVICE_HOST, LOADER_PORT, 700);
    if (s < 0) return false;
    close(s);
    return true;
}

#ifndef GH_CONFIG_PATH
#define GH_CONFIG_PATH "/data/GoldHEN/config.ini"
#endif
static const char* GH_CONFIG = GH_CONFIG_PATH;

static bool read_whole(const char* path, std::string& out)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char buf[2048];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static std::string trim_ws(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

// Позиция цифры в строке "Enabled = X" внутри раздела [BinLoader].
static bool find_binloader_value(const std::string& s, size_t& pos)
{
    size_t i = 0;
    bool in = false;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        std::string raw = s.substr(i, e - i);
        std::string t = trim_ws(raw);
        if (!t.empty() && t[0] == '[') {
            in = (t == "[BinLoader]");
        } else if (in) {
            size_t eq = raw.find('=');
            if (eq != std::string::npos && trim_ws(raw.substr(0, eq)) == "Enabled") {
                size_t p = i + eq + 1;
                while (p < e && (s[p] == ' ' || s[p] == '\t')) p++;
                if (p < e && (s[p] == '0' || s[p] == '1')) { pos = p; return true; }
            }
        }
        i = e + 1;
    }
    return false;
}

int goldhen_binloader_state()
{
    std::string s;
    if (!read_whole(GH_CONFIG, s)) return -1;
    size_t p;
    if (!find_binloader_value(s, p)) return -1;
    return s[p] == '1' ? 1 : 0;
}

bool goldhen_set_binloader(bool on)
{
    std::string s;
    if (!read_whole(GH_CONFIG, s)) { logf_("goldhen: cannot read %s", GH_CONFIG); return false; }
    size_t p;
    if (!find_binloader_value(s, p)) { logf_("goldhen: no [BinLoader] Enabled in config"); return false; }
    s[p] = on ? '1' : '0';
    FILE* f = fopen(GH_CONFIG, "wb");
    if (!f) { logf_("goldhen: cannot write %s", GH_CONFIG); return false; }
    size_t w = fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    logf_("goldhen: BinLoader Enabled set to %d (%d bytes written)", on ? 1 : 0, (int)w);
    return w == s.size();
}

bool service_quit()
{
    HttpResult r = http_post(SERVICE_HOST, SERVICE_PORT, "/quit", "{}", 2000);
    logf_("service: quit request -> connected=%d status=%d", r.connected ? 1 : 0, r.status);
    return r.connected && r.status == 200;
}

bool service_disks(DisksView& out)
{
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/disks", 3000);
    if (!r.connected || r.status != 200) return false;
    JsonValue j;
    if (!json_parse(r.body, j) || j.type != JsonValue::OBJ) return false;
    out.loaded = j["loaded"].asBool();
    out.sig = j["sig"].asStr();
    out.disks.clear();
    const JsonValue& arr = j["disks"];
    for (size_t i = 0; i < arr.arr.size(); i++) {
        const JsonValue& d = arr.arr[i];
        DiskView v;
        v.id = d["id"].asStr();
        v.label = d["label"].asStr();
        v.bytes = (uint64_t)d["bytes"].asNum();
        const JsonValue& s = d["smart"];
        if (s.type == JsonValue::OBJ) {
            v.hasSmart = true;
            v.model = s["model"].asStr();
            v.bridge = s["bridge"].asStr();
            v.verdict = (int)s["verdict"].asInt(-1);
            v.temp = (int)s["temp"].asInt(0);
        }
        out.disks.push_back(v);
    }
    return true;
}

std::string service_sig()
{
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/sig", 1500);
    if (!r.connected || r.status != 200) return "";
    JsonValue j;
    if (!json_parse(r.body, j) || j.type != JsonValue::OBJ) return "";
    return j["sig"].asStr();
}

bool service_refresh_disks()
{
    HttpResult r = http_post(SERVICE_HOST, SERVICE_PORT, "/api/refresh", "{}", 3000);
    return r.connected && r.status == 200;
}
