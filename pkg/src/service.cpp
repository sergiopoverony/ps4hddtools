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

// connectFailed (если указан) ставится в true только тогда, когда не удалось именно подключиться
// к порту 9090 (BinLoader выключен/недоступен) - в отличие от ошибок чтения файла или отправки,
// на которые повторная попытка подключиться не поможет.
bool service_send_payload(std::string& err, bool* connectFailed)
{
    if (connectFailed) *connectFailed = false;

    FILE* f = fopen(PAYLOAD_PATH, "rb");
    if (!f) { err = "no payload file in PKG"; logf_("service: cannot open %s", PAYLOAD_PATH); return false; }
    std::string data;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() < 1024) { err = "payload file is empty"; return false; }

    int s = net_connect(SERVICE_HOST, LOADER_PORT, 3000);
    if (s < 0) {
        err = "BinLoader (port 9090) is not available. Is GoldHEN running?";
        logf_("service: BinLoader connect failed");
        if (connectFailed) *connectFailed = true;
        return false;
    }

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
        v.path = d["path"].asStr();
        v.label = d["label"].asStr();
        v.bytes = (uint64_t)d["bytes"].asNum();
        const JsonValue& s = d["smart"];
        if (s.type == JsonValue::OBJ) {
            v.hasSmart = true;
            v.model = s["model"].asStr();
            v.firmware = s["fw"].asStr();
            v.serial = s["serial"].asStr();
            v.bridge = s["bridge"].asStr();
            v.proto = s["proto"].asStr();
            v.verdict = (int)s["verdict"].asInt(-1);
            v.temp = (int)s["temp"].asInt(0);
            v.rpm = (int)s["rpm"].asInt(-1);
            v.poh = (int64_t)s["poh"].asInt(-1);
            v.cycles = (int64_t)s["cycles"].asInt(-1);
            v.hostRead = (int64_t)s["hr"].asInt(-1);
            v.hostWrite = (int64_t)s["hw"].asInt(-1);

            const JsonValue& attrs = s["attrs"];
            for (size_t k = 0; k < attrs.arr.size(); k++) {
                const JsonValue& a = attrs.arr[k];
                SmartAttrView av;
                av.id = (int)a["id"].asInt(0);
                av.name = a["n"].asStr();
                av.cur = (int)a["c"].asInt(0);
                av.worst = (int)a["w"].asInt(0);
                av.thresh = (int)a["t"].asInt(0);
                av.raw = a["rx"].asStr();
                av.sev = (int)a["s"].asInt(0);
                v.attrs.push_back(av);
            }
            const JsonValue& reasons = s["reasons"];
            for (size_t k = 0; k < reasons.arr.size(); k++) {
                const JsonValue& rr = reasons.arr[k];
                SmartReasonView rv;
                rv.sev = (int)rr["s"].asInt(0);
                rv.code = rr["c"].asStr();
                rv.id = (int)rr["id"].asInt(0);
                rv.value = (int64_t)rr["v"].asInt(0);
                v.reasons.push_back(rv);
            }
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

bool service_state(StateView& out)
{
    out = StateView();
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/state", 2000);
    if (!r.connected || r.status != 200) return false;
    JsonValue j;
    if (!json_parse(r.body, j) || j.type != JsonValue::OBJ) return false;
    out.ok = true;
    out.job = j["job"].asStr("idle");
    out.loaded = j["loaded"].asBool();

    const JsonValue& s = j["scan"];
    if (s.type == JsonValue::OBJ) {
        ScanView& sv = out.scan;
        sv.present = true;
        sv.disk = s["disk"].asStr();
        sv.state = s["state"].asStr();
        sv.msg = s["msg"].asStr();
        sv.from = (uint64_t)s["from"].asNum();
        sv.to = (uint64_t)s["to"].asNum();
        sv.done = (uint64_t)s["done"].asNum();
        sv.speed = s["speed"].asNum();
        sv.eta = (int)s["eta"].asInt(-1);
        sv.elapsed = s["elapsed"].asNum();
        sv.bad = (uint64_t)s["bad"].asNum();
        sv.approx = s["approx"].asBool();
        const JsonValue& hist = s["hist"];
        for (size_t i = 0; i < hist.arr.size() && i < 7; i++) sv.hist[i + 1] = (uint64_t)hist.arr[i].asNum();
        const JsonValue& bl = s["badlba"];
        for (size_t i = 0; i < bl.arr.size(); i++) sv.badlba.push_back((uint64_t)bl.arr[i].asNum());
        sv.map = s["map"].asStr();
    }

    const JsonValue& e = j["est"];
    if (e.type == JsonValue::OBJ) {
        EstView& ev = out.est;
        ev.present = true;
        ev.disk = e["disk"].asStr();
        ev.ok = e["ok"].asBool();
        ev.from = (uint64_t)e["from"].asNum();
        ev.to = (uint64_t)e["to"].asNum();
        ev.speed = e["speed"].asNum();
        ev.seconds = (int)e["seconds"].asInt(-1);
    }
    return true;
}

static bool post_scan(const std::string& path, const std::string& disk, uint64_t fromGb, uint64_t toGb)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"disk\":\"%s\",\"from_gb\":%llu,\"to_gb\":%llu}",
              disk.c_str(), (unsigned long long)fromGb, (unsigned long long)toGb);
    HttpResult r = http_post(SERVICE_HOST, SERVICE_PORT, path.c_str(), body, 3000);
    return r.connected && r.status == 200;
}

bool service_scan_estimate(const std::string& disk, uint64_t fromGb, uint64_t toGb) { return post_scan("/api/scan/estimate", disk, fromGb, toGb); }
bool service_scan_start(const std::string& disk, uint64_t fromGb, uint64_t toGb)    { return post_scan("/api/scan/start", disk, fromGb, toGb); }

bool service_scan_pause()  { HttpResult r = http_post(SERVICE_HOST, SERVICE_PORT, "/api/scan/pause", "{}", 2000);  return r.connected && r.status == 200; }
bool service_scan_resume() { HttpResult r = http_post(SERVICE_HOST, SERVICE_PORT, "/api/scan/resume", "{}", 2000); return r.connected && r.status == 200; }
bool service_scan_stop()   { HttpResult r = http_post(SERVICE_HOST, SERVICE_PORT, "/api/scan/stop", "{}", 2000);   return r.connected && r.status == 200; }

std::string service_log(size_t maxBytes)
{
    char path[32];
    snprintf(path, sizeof(path), "/api/log?n=%zu", maxBytes);
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, path, 2000);
    if (!r.connected || r.status != 200) return "";
    return r.body;
}
