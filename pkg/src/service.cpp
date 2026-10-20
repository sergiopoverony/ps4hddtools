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
