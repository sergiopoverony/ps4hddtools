#include "smart.h"
#include "log.h"
#include "platform.h"

#include <cerrno>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef HOST_TEST
#include <sys/types.h>
#include <sys/ioccom.h>
#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/ata/ata_all.h>
#include <cam/scsi/scsi_all.h>
#endif

// ---------------------------------------------------------------- разбор (чистый код)

const char* smart_attr_name(uint8_t id) {
    switch (id) {
        case 0x01: return "Read Error Rate";
        case 0x02: return "Throughput Performance";
        case 0x03: return "Spin-Up Time";
        case 0x04: return "Start/Stop Count";
        case 0x05: return "Reallocated Sectors";
        case 0x07: return "Seek Error Rate";
        case 0x08: return "Seek Time Performance";
        case 0x09: return "Power-On Hours";
        case 0x0A: return "Spin Retry Count";
        case 0x0C: return "Power Cycle Count";
        case 0xB7: return "Runtime Bad Block";
        case 0xB8: return "End-to-End Error";
        case 0xBB: return "Reported Uncorrectable Errors";
        case 0xBC: return "Command Timeout";
        case 0xBD: return "High Fly Writes";
        case 0xBE: return "Airflow Temperature";
        case 0xBF: return "G-Sense Error Rate";
        case 0xC0: return "Power-Off Retract / Unsafe Shutdowns";
        case 0xC1: return "Load/Unload Cycle Count";
        case 0xC2: return "Temperature";
        case 0xC3: return "Hardware ECC Recovered";
        case 0xC4: return "Reallocation Event Count";
        case 0xC5: return "Current Pending Sectors";
        case 0xC6: return "Uncorrectable Sectors";
        case 0xC7: return "UDMA CRC Errors";
        case 0xF0: return "Head Flying Hours";
        case 0xF1: return "Total Host Writes";
        case 0xF2: return "Total Host Reads";
        default: return "Vendor-specific";
    }
}

static bool known_seagate(const std::string& model) { return model.compare(0, 11, "ST2000LM007") == 0; }

void smart_parse(const uint8_t smart[512], const uint8_t* thresh, SmartReport& rep) {
    rep.attrs.clear();
    for (int i = 0; i < 30; ++i) {
        const uint8_t* a = smart + 2 + i * 12;
        if (a[0] == 0x00 || a[0] == 0xFF) continue;
        SmartAttr s;
        s.id = a[0];
        s.flags = a[1];
        s.current = a[3];
        s.worst = a[4];
        for (int b = 0; b < 6; ++b) {
            s.raw[b] = a[5 + b];
            s.raw64 |= (uint64_t)a[5 + b] << (8 * b);
        }
        if (thresh) {
            for (int j = 0; j < 30; ++j) {
                const uint8_t* t = thresh + 2 + j * 12;
                if (t[0] == s.id) { s.thresh = t[1]; break; }
            }
        }
        rep.attrs.push_back(s);
    }
    rep.have_smart = !rep.attrs.empty();
    rep.temp_c = -1; rep.power_on_hours = -1; rep.power_cycles = -1;
    rep.host_read_bytes = -1; rep.host_write_bytes = -1;
    for (const SmartAttr& s : rep.attrs) {
        if (s.id == 0xC2) rep.temp_c = s.raw[0];
        else if (s.id == 0x0C) rep.power_cycles = (int64_t)(s.raw64 & 0xFFFFFFFFULL);
        else if (s.id == 0x09) {
            // Seagate: байты 0..3 это часы. Для прочих: только если старшие байты нули.
            if (known_seagate(rep.model) || (s.raw[3] == 0 && s.raw[4] == 0 && s.raw[5] == 0))
                rep.power_on_hours = (int64_t)(s.raw64 & 0xFFFFFFFFULL);
        } else if ((s.id == 0xF1 || s.id == 0xF2) && known_seagate(rep.model)) {
            int64_t bytes = (int64_t)(s.raw64 * 512ULL);
            (s.id == 0xF1 ? rep.host_write_bytes : rep.host_read_bytes) = bytes;
        }
    }
}

static int64_t low32(const SmartAttr& s) { return (int64_t)(s.raw64 & 0xFFFFFFFFULL); }

void smart_assess(SmartReport& rep) {
    rep.reasons.clear();
    if (!rep.have_smart && rep.status < 0) { rep.verdict = -1; return; }
    int worst = 0;
    auto add = [&](int sev, const char* code, int id, int64_t v) {
        rep.reasons.push_back({sev, code, id, v});
        if (sev > worst) worst = sev;
    };
    if (rep.status == 1) add(2, "status_fail", 0, 0);
    for (SmartAttr& s : rep.attrs) {
        s.sev = 0;
        int64_t r = low32(s);
        if (s.thresh > 0 && s.current > 0 && s.current <= s.thresh) {
            s.sev = 2;
            add(2, "thr", s.id, s.current);
            continue;
        }
        switch (s.id) {
            case 0x05: if (r >= 50) s.sev = 2; else if (r > 0) s.sev = 1; break;
            case 0xC5:
            case 0xC6: if (r >= 20) s.sev = 2; else if (r > 0) s.sev = 1; break;
            case 0xC4:
            case 0x0A:
            case 0xBB: if (r > 0) s.sev = 1; break;
            case 0xC7: if (r >= 20) s.sev = 1; break;
            default: break;
        }
        if (s.sev) add(s.sev, "attr", s.id, r);
    }
    if (rep.temp_c >= 65) add(2, "temp", 0xC2, rep.temp_c);
    else if (rep.temp_c >= 55) add(1, "temp", 0xC2, rep.temp_c);
    rep.verdict = worst;
}

int smart_status_from_sense(const uint8_t* sense, size_t len) {
    if (!sense || len < 22 || (sense[0] & 0x7F) != 0x72) return -1;
    size_t end = std::min<size_t>(len, 8 + (size_t)sense[7]);
    for (size_t off = 8; off + 2 <= end; off += 2 + sense[off + 1]) {
        if (sense[off] == 0x09 && sense[off + 1] >= 0x0C && off + 14 <= len) {
            uint8_t mid = sense[off + 9], high = sense[off + 11];
            if (mid == 0x4F && high == 0xC2) return 0;
            if (mid == 0xF4 && high == 0x2C) return 1;
            return -1;
        }
    }
    return -1;
}

// ---------------------------------------------------------------- доступ к диску

static std::string trim_ident(const uint8_t id[512], unsigned first_word, unsigned words) {
    std::string r;
    for (unsigned i = 0; i < words; ++i) {
        unsigned p = (first_word + i) * 2;
        r += (char)id[p + 1];
        r += (char)id[p];
    }
    for (char& c : r) if ((unsigned char)c < 32 || (unsigned char)c > 126) c = ' ';
    size_t b = r.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    size_t e = r.find_last_not_of(' ');
    return r.substr(b, e - b + 1);
}

static int rpm_from_word(unsigned w) {
    return (w == 1) ? 1 : (w > 0x0400 && w != 0xFFFF) ? (int)w : -1;
}

static void parse_identify(const uint8_t id[512], SmartReport& rep) {
    rep.model = trim_ident(id, 27, 20);
    rep.firmware = trim_ident(id, 23, 4);
    rep.serial = trim_ident(id, 10, 10);
    uint64_t s28 = 0;
    for (int i = 0; i < 4; ++i) s28 |= (uint64_t)(id[200 + i * 2] | (id[201 + i * 2] << 8)) << (16 * i);
    uint32_t s32 = (uint32_t)(id[120] | (id[121] << 8)) | ((uint32_t)(id[122] | (id[123] << 8)) << 16);
    rep.sectors = s28 ? s28 : s32;
    rep.rpm = rpm_from_word(id[434] | (id[435] << 8));
    rep.have_identify = !rep.model.empty();
}

struct AtaCmd {
    uint8_t command = 0, features = 0, count = 1, lba_low = 0, lba_mid = 0, lba_high = 0, device = 0;
    bool data_in = true;   // false = без данных
    bool ck_cond = false;  // вернуть регистры в sense (для RETURN STATUS)
};

#ifdef HOST_TEST
// На ПК «устройство» это файл: рядом лежат образцы ответов <путь>.identify/.smart/.thresh/.status
static bool host_read(const char* dev, const char* ext, uint8_t* buf, size_t n) {
    std::string p = std::string(dev) + "." + ext;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    size_t r = fread(buf, 1, n, f);
    fclose(f);
    return r == n;
}

bool smart_query(const char* pass_path, SmartReport& rep) {
    uint8_t buf[512], th[512];
    if (host_read(pass_path, "weak", buf, 1)) { rep.bridge = "ANKEJE USB3.0"; rep.error = "identify"; return false; }
    if (!host_read(pass_path, "identify", buf, 512)) { rep.error = "identify"; return false; }
    parse_identify(buf, rep);
    rep.proto = "SAT";
    rep.bridge = "HOST_TEST";
    if (host_read(pass_path, "smart", buf, 512)) {
        bool t = host_read(pass_path, "thresh", th, 512);
        smart_parse(buf, t ? th : nullptr, rep);
    }
    uint8_t st[2];
    if (host_read(pass_path, "status", st, 2)) rep.status = (st[0] == 0xF4 && st[1] == 0x2C) ? 1 : 0;
    smart_assess(rep);
    return true;
}
#else

#ifndef CAMIOCOMMAND
#define CAMIOCOMMAND _IOWR(0x17, 2, union ccb)
#endif
static const unsigned long kCamCodes[] = {(unsigned long)CAMIOCOMMAND, (unsigned long)_IOWR(0x16, 2, union ccb)};

static int cam_ioctl(int fd, union ccb* ccb) {
    int last = ENOTTY;
    for (unsigned long code : kCamCodes) {
        if (ioctl(fd, code, ccb) == 0) return 0;
        last = errno;
        if (last != ENOTTY && last != EINVAL && last != ENOSYS) break;
    }
    errno = last;
    return -1;
}

struct CamDev {
    int fd = -1;
    unsigned path = 0, target = 0, lun = 0;
    int protocol = -1;
};

// SAT ATA PASS-THROUGH(16). Поля CDB для чтения данных совпадают с подтверждённым на консоли вариантом.
static bool sat16(const CamDev& d, const AtaCmd& c, uint8_t* data, size_t len, uint8_t sense_out[32], unsigned* cam_status = nullptr) {
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_SCSI_IO;
    ccb.ccb_h.path_id = d.path;
    ccb.ccb_h.target_id = d.target;
    ccb.ccb_h.target_lun = d.lun;
    ccb.ccb_h.timeout = 15000;
    ccb.ccb_h.flags = c.data_in ? CAM_DIR_IN : CAM_DIR_NONE;
    ccb.csio.cdb_len = 16;
    ccb.csio.sense_len = SSD_FULL_SIZE;
    ccb.csio.data_ptr = c.data_in ? data : nullptr;
    ccb.csio.dxfer_len = c.data_in ? (uint32_t)len : 0;
    uint8_t* cdb = ccb.csio.cdb_io.cdb_bytes;
    cdb[0] = 0x85;
    cdb[1] = c.data_in ? 0x08 : 0x06;                                   // PIO data-in / non-data
    cdb[2] = c.data_in ? 0x0E : 0x00;                                   // T_DIR, BYT_BLOK, T_LENGTH
    if (c.ck_cond) cdb[2] |= 0x20;
    cdb[4] = c.features;
    cdb[6] = c.count;
    cdb[8] = c.lba_low;
    cdb[10] = c.lba_mid;
    cdb[12] = c.lba_high;
    cdb[13] = c.device;
    cdb[14] = c.command;
    if (cam_ioctl(d.fd, &ccb) != 0) return false;
    if (sense_out) memcpy(sense_out, &ccb.csio.sense_data, 32);
    unsigned cs = ccb.ccb_h.status & CAM_STATUS_MASK;
    if (cam_status) *cam_status = cs;
    if (c.ck_cond) return cs == CAM_REQ_CMP || cs == CAM_SCSI_STATUS_ERROR;
    return cs == CAM_REQ_CMP && ccb.csio.scsi_status == SCSI_STATUS_OK;
}

// SAT(12): запасной вариант для мостов, не понимающих SAT(16). Только чтение данных.
static bool sat12(const CamDev& d, const AtaCmd& c, uint8_t* data, size_t len) {
    if (!c.data_in) return false;
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_SCSI_IO;
    ccb.ccb_h.path_id = d.path;
    ccb.ccb_h.target_id = d.target;
    ccb.ccb_h.target_lun = d.lun;
    ccb.ccb_h.timeout = 15000;
    ccb.ccb_h.flags = CAM_DIR_IN;
    ccb.csio.cdb_len = 12;
    ccb.csio.sense_len = SSD_FULL_SIZE;
    ccb.csio.data_ptr = data;
    ccb.csio.dxfer_len = (uint32_t)len;
    uint8_t* cdb = ccb.csio.cdb_io.cdb_bytes;
    cdb[0] = 0xA1; cdb[1] = 0x08; cdb[2] = 0x0E;
    cdb[3] = c.features; cdb[4] = c.count; cdb[5] = c.lba_low;
    cdb[6] = c.lba_mid; cdb[7] = c.lba_high; cdb[8] = c.device; cdb[9] = c.command;
    if (cam_ioctl(d.fd, &ccb) != 0) return false;
    return (ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP && ccb.csio.scsi_status == SCSI_STATUS_OK;
}

// Диск, подключённый напрямую по ATA (без моста).
static bool native_ata(const CamDev& d, const AtaCmd& c, uint8_t* data, size_t len) {
    if (!c.data_in) return false;
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_ATA_IO;
    ccb.ccb_h.path_id = d.path;
    ccb.ccb_h.target_id = d.target;
    ccb.ccb_h.target_lun = d.lun;
    ccb.ccb_h.timeout = 10000;
    ccb.ccb_h.flags = CAM_DIR_IN;
    ccb.ataio.cmd.command = c.command;
    ccb.ataio.cmd.features = c.features;
    ccb.ataio.cmd.sector_count = c.count;
    ccb.ataio.cmd.lba_low = c.lba_low;
    ccb.ataio.cmd.lba_mid = c.lba_mid;
    ccb.ataio.cmd.lba_high = c.lba_high;
    ccb.ataio.cmd.device = c.device;
    ccb.ataio.data_ptr = data;
    ccb.ataio.dxfer_len = (uint32_t)len;
    if (cam_ioctl(d.fd, &ccb) != 0) return false;
    return (ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP;
}

// SMART RETURN STATUS для диска на прямом ATA: результат в регистрах lba_mid/lba_high.
static int native_status(const CamDev& d) {
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_ATA_IO;
    ccb.ccb_h.path_id = d.path;
    ccb.ccb_h.target_id = d.target;
    ccb.ccb_h.target_lun = d.lun;
    ccb.ccb_h.timeout = 10000;
    ccb.ccb_h.flags = CAM_DIR_NONE;
    ccb.ataio.cmd.flags = CAM_ATAIO_NEEDRESULT;
    ccb.ataio.cmd.command = 0xB0;
    ccb.ataio.cmd.features = 0xDA;
    ccb.ataio.cmd.lba_mid = 0x4F;
    ccb.ataio.cmd.lba_high = 0xC2;
    if (cam_ioctl(d.fd, &ccb) != 0) return -1;
    if ((ccb.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) return -1;
    uint8_t mid = ccb.ataio.res.lba_mid, high = ccb.ataio.res.lba_high;
    app_log("SMART: RETURN STATUS (ATA) lba_mid=0x%02X lba_high=0x%02X", mid, high);
    if (mid == 0x4F && high == 0xC2) return 0;
    if (mid == 0xF4 && high == 0x2C) return 1;
    return -1;
}

static bool ata_read512(const CamDev& d, const AtaCmd& c, uint8_t out[512]) {
    memset(out, 0, 512);
    if (d.protocol == PROTO_ATA) return native_ata(d, c, out, 512);
    if (sat16(d, c, out, 512, nullptr)) return true;
    memset(out, 0, 512);
    return sat12(d, c, out, 512);
}

static bool checksum_ok(const uint8_t b[512]) {
    unsigned s = 0;
    for (int i = 0; i < 512; ++i) s += b[i];
    return (s & 0xFF) == 0;
}

static std::string clean(const void* p, size_t n) {
    const unsigned char* c = (const unsigned char*)p;
    std::string r;
    for (size_t i = 0; i < n; ++i) r += (c[i] >= 32 && c[i] < 127) ? (char)c[i] : ' ';
    size_t b = r.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    return r.substr(b, r.find_last_not_of(' ') - b + 1);
}

bool smart_query(const char* path, SmartReport& rep) {
    // Обязательно O_RDWR и без O_NONBLOCK (подтверждено на консоли).
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        rep.error = "open";
        app_log("SMART: open(%s) errno=%d (%s)", path, errno, strerror(errno));
        return false;
    }
    CamDev d;
    d.fd = fd;
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_GDEV_TYPE;
    if (cam_ioctl(fd, &ccb) != 0 || (ccb.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
        rep.error = "gdev";
        app_log("SMART: %s: XPT_GDEV_TYPE не удался (errno=%d)", path, errno);
        close(fd);
        return false;
    }
    d.path = ccb.cgd.ccb_h.path_id;
    d.target = ccb.cgd.ccb_h.target_id;
    d.lun = ccb.cgd.ccb_h.target_lun;
    d.protocol = ccb.cgd.protocol;
    std::string vendor = clean(ccb.cgd.inq_data.vendor, sizeof(ccb.cgd.inq_data.vendor));
    std::string product = clean(ccb.cgd.inq_data.product, sizeof(ccb.cgd.inq_data.product));
    app_log("SMART: %s: vendor='%s' product='%s' protocol=%d path=%u target=%u lun=%u", path,
            vendor.c_str(), product.c_str(), d.protocol, d.path, d.target, d.lun);
    if (vendor == "SONY") {  // системная память консоли: никаких команд не посылаем
        rep.error = "sony";
        close(fd);
        return false;
    }
    rep.bridge = vendor.empty() ? product : vendor + " " + product;
    rep.proto = d.protocol == PROTO_ATA ? "ATA" : "SAT";
    if (d.protocol == PROTO_ATA) {
        // Диск напрямую по SATA: система уже знает его паспорт (строки уже в нормальном порядке).
        // Подтверждено на консоли: модель берётся из XPT_GDEV_TYPE, команда диску не нужна.
        const struct ata_params& ap = ccb.cgd.ident_data;
        rep.model = clean(ap.model, sizeof(ap.model));
        rep.firmware = clean(ap.revision, sizeof(ap.revision));
        rep.serial = clean(ap.serial, sizeof(ap.serial));
        uint64_t s48 = (uint64_t)ap.lba_size48_1 | ((uint64_t)ap.lba_size48_2 << 16) |
                       ((uint64_t)ap.lba_size48_3 << 32) | ((uint64_t)ap.lba_size48_4 << 48);
        uint64_t s28 = (uint64_t)ap.lba_size_1 | ((uint64_t)ap.lba_size_2 << 16);
        rep.sectors = s48 ? s48 : s28;
        rep.rpm = rpm_from_word(ap.media_rotation_rate);
        rep.have_identify = !rep.model.empty();
        app_log("SMART: паспорт из системы: model='%s' fw='%s' sectors=%llu", rep.model.c_str(),
                rep.firmware.c_str(), (unsigned long long)rep.sectors);
    }

    uint8_t buf[512], th[512];
    AtaCmd id;
    id.command = 0xEC;
    if (d.protocol == PROTO_ATA) {
        // паспорт уже получен выше
    } else if (ata_read512(d, id, buf)) {
        parse_identify(buf, rep);
        app_log("SMART: IDENTIFY model='%s' fw='%s' sectors=%llu", rep.model.c_str(),
                rep.firmware.c_str(), (unsigned long long)rep.sectors);
    } else {
        app_log("SMART: IDENTIFY недоступен через %s", path);
    }

    AtaCmd sd;
    sd.command = 0xB0; sd.features = 0xD0; sd.lba_mid = 0x4F; sd.lba_high = 0xC2;
    bool got = ata_read512(d, sd, buf);
    if (got && !checksum_ok(buf)) {
        app_log("SMART: контрольная сумма SMART DATA не сошлась");
        got = false;
    }
    if (got) {
        AtaCmd td = sd;
        td.features = 0xD1;
        bool t = ata_read512(d, td, th) && checksum_ok(th);
        smart_parse(buf, t ? th : nullptr, rep);
        app_log("SMART: показателей %u, пороги %s", (unsigned)rep.attrs.size(), t ? "получены" : "недоступны");
        if (d.protocol != PROTO_ATA) {
            AtaCmd st;
            st.command = 0xB0; st.features = 0xDA; st.lba_mid = 0x4F; st.lba_high = 0xC2;
            st.count = 0; st.data_in = false; st.ck_cond = true;
            uint8_t sense[32];
            memset(sense, 0, sizeof(sense));
            unsigned cs = 0;
            bool ok = sat16(d, st, nullptr, 0, sense, &cs);
            rep.status = ok ? smart_status_from_sense(sense, sizeof(sense)) : -1;
            app_log("SMART: RETURN STATUS (SAT) выполнен=%d CAM=0x%x sense=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    ok ? 1 : 0, cs, sense[0], sense[1], sense[2], sense[3], sense[4], sense[5], sense[6], sense[7], sense[8],
                    sense[9], sense[10], sense[11], sense[12], sense[13], sense[14], sense[15], sense[16], sense[17], sense[18],
                    sense[19], sense[20], sense[21]);
        } else {
            rep.status = native_status(d);
        }
        app_log("SMART: RETURN STATUS = %d (0 исправен, 1 скоро откажет, -1 неизвестно)", rep.status);
    } else {
        rep.error = "smart";
        app_log("SMART: SMART DATA не получены");
    }
    close(fd);
    smart_assess(rep);
    return rep.have_identify || rep.have_smart;
}
#endif
