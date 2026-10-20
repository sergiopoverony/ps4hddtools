#include "smart.h"

#include <cerrno>
#include <cstdio>
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

// Populated by ATA IDENTIFY before SMART parsing.
static char g_identified_model[41] = {0};

#ifndef HOST_TEST
#ifndef CAMIOCOMMAND
#define CAMIOCOMMAND _IOWR(0x17, 2, union ccb)
#endif

static const unsigned long kCamIoctlCodes[] = {
    (unsigned long)CAMIOCOMMAND,
    (unsigned long)_IOWR(0x16, 2, union ccb)
};

static int cam_ioctl_compat(int fd, union ccb *ccb) {
    int last_errno = ENOTTY;
    for (unsigned long code : kCamIoctlCodes) {
        if (ioctl(fd, code, ccb) == 0) return 0;
        last_errno = errno;
        if (last_errno != ENOTTY && last_errno != EINVAL && last_errno != ENOSYS)
            break;
    }
    errno = last_errno;
    return -1;
}

static int get_cam_device(int fd, union ccb *ccb,
                          unsigned *path_id, unsigned *target_id,
                          unsigned *target_lun, int *protocol) {
    memset(ccb, 0, sizeof(*ccb));
    ccb->ccb_h.func_code = XPT_GDEV_TYPE;
    if (cam_ioctl_compat(fd, ccb) != 0) return -1;
    if ((ccb->ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
        errno = EIO;
        return -1;
    }
    *path_id = ccb->cgd.ccb_h.path_id;
    *target_id = ccb->cgd.ccb_h.target_id;
    *target_lun = ccb->cgd.ccb_h.target_lun;
    *protocol = ccb->cgd.protocol;
    return 0;
}

// Direct ATA CCB path. Used only when CAM reports native ATA protocol.
static int read_smart_native_ata(int fd, unsigned path_id, unsigned target_id,
                                 unsigned target_lun, SmartData *out_data) {
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_ATA_IO;
    ccb.ccb_h.path_id = path_id;
    ccb.ccb_h.target_id = target_id;
    ccb.ccb_h.target_lun = target_lun;
    ccb.ccb_h.timeout = 10000;
    ccb.ccb_h.flags = CAM_DIR_IN;

    ccb.ataio.cmd.command = 0xB0;       // SMART
    ccb.ataio.cmd.features = 0xD0;      // READ DATA
    ccb.ataio.cmd.sector_count = 1;
    ccb.ataio.cmd.lba_low = 0;
    ccb.ataio.cmd.lba_mid = 0x4F;
    ccb.ataio.cmd.lba_high = 0xC2;
    ccb.ataio.cmd.device = 0;
    ccb.ataio.cmd.control = 0;
    ccb.ataio.data_ptr = out_data->data;
    ccb.ataio.dxfer_len = sizeof(out_data->data);

    if (cam_ioctl_compat(fd, &ccb) != 0) {
        ps4smart_logf("[-] ATA SMART ioctl failed: errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    unsigned cs = ccb.ccb_h.status & CAM_STATUS_MASK;
    if (cs != CAM_REQ_CMP) {
        ps4smart_logf("[-] ATA SMART CAM status=0x%x ATA status=0x%x error=0x%x\n",
               cs, (unsigned)ccb.ataio.res.status, (unsigned)ccb.ataio.res.error);
        errno = EIO;
        return -1;
    }
    return 0;
}

// USB-SATA bridges often expose a SCSI protocol. Try SAT ATA PASS-THROUGH(16).
// This is a SCSI command to the bridge; it succeeds only if the bridge supports SAT.
static int read_smart_sat16(int fd, unsigned path_id, unsigned target_id,
                            unsigned target_lun, SmartData *out_data) {
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_SCSI_IO;
    ccb.ccb_h.path_id = path_id;
    ccb.ccb_h.target_id = target_id;
    ccb.ccb_h.target_lun = target_lun;
    ccb.ccb_h.timeout = 15000;
    ccb.ccb_h.flags = CAM_DIR_IN;

    ccb.csio.cdb_len = 16;
    ccb.csio.sense_len = SSD_FULL_SIZE;
    ccb.csio.data_ptr = out_data->data;
    ccb.csio.dxfer_len = sizeof(out_data->data);
    ccb.csio.cdb_io.cdb_bytes[0] = 0x85; // ATA PASS-THROUGH(16)
    ccb.csio.cdb_io.cdb_bytes[1] = 0x08; // PIO Data-In protocol
    ccb.csio.cdb_io.cdb_bytes[2] = 0x0E; // T_DIR=IN, BYT_BLOK=1, T_LENGTH=sector count; CK_COND=0
    ccb.csio.cdb_io.cdb_bytes[3] = 0x00; // features expanded
    ccb.csio.cdb_io.cdb_bytes[4] = 0xD0; // SMART READ DATA feature
    ccb.csio.cdb_io.cdb_bytes[5] = 0x00; // sector count expanded
    ccb.csio.cdb_io.cdb_bytes[6] = 0x01; // one sector
    ccb.csio.cdb_io.cdb_bytes[7] = 0x00; // LBA low expanded
    ccb.csio.cdb_io.cdb_bytes[8] = 0x00; // LBA low
    ccb.csio.cdb_io.cdb_bytes[9] = 0x00; // LBA mid expanded
    ccb.csio.cdb_io.cdb_bytes[10] = 0x4F; // SMART signature mid
    ccb.csio.cdb_io.cdb_bytes[11] = 0x00; // LBA high expanded
    ccb.csio.cdb_io.cdb_bytes[12] = 0xC2; // SMART signature high
    ccb.csio.cdb_io.cdb_bytes[13] = 0x00; // device
    ccb.csio.cdb_io.cdb_bytes[14] = 0xB0; // SMART command
    ccb.csio.cdb_io.cdb_bytes[15] = 0x00; // control

    if (cam_ioctl_compat(fd, &ccb) != 0) {
        ps4smart_logf("[-] SAT ATA PASS-THROUGH(16) ioctl failed: errno=%d (%s)\n",
               errno, strerror(errno));
        return -1;
    }

    unsigned cs = ccb.ccb_h.status & CAM_STATUS_MASK;
    if (cs != CAM_REQ_CMP || ccb.csio.scsi_status != SCSI_STATUS_OK) {
        ps4smart_logf("[-] SAT command failed: CAM status=0x%x SCSI status=0x%x",
               cs, (unsigned)ccb.csio.scsi_status);
        if (ccb.csio.sense_len > 0) {
            const unsigned char *sense = (const unsigned char *)&ccb.csio.sense_data;
            ps4smart_logf(" sense=");
            for (unsigned i = 0; i < 18 && i < sizeof(ccb.csio.sense_data); ++i)
                ps4smart_logf("%02X", sense[i]);
        }
        ps4smart_logf("\n");
        errno = EIO;
        return -1;
    }
    return 0;
}

// Some USB-SATA bridges support the 12-byte SAT command but reject SAT16.
static int read_smart_sat12(int fd, unsigned path_id, unsigned target_id,
                            unsigned target_lun, SmartData *out_data) {
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_SCSI_IO;
    ccb.ccb_h.path_id = path_id;
    ccb.ccb_h.target_id = target_id;
    ccb.ccb_h.target_lun = target_lun;
    ccb.ccb_h.timeout = 15000;
    ccb.ccb_h.flags = CAM_DIR_IN;

    ccb.csio.cdb_len = 12;
    ccb.csio.sense_len = SSD_FULL_SIZE;
    ccb.csio.data_ptr = out_data->data;
    ccb.csio.dxfer_len = sizeof(out_data->data);
    ccb.csio.cdb_io.cdb_bytes[0] = 0xA1; // ATA PASS-THROUGH(12)
    ccb.csio.cdb_io.cdb_bytes[1] = 0x08; // PIO Data-In
    ccb.csio.cdb_io.cdb_bytes[2] = 0x0E; // Data-In, block, sector-count transfer
    ccb.csio.cdb_io.cdb_bytes[3] = 0xD0; // SMART READ DATA feature
    ccb.csio.cdb_io.cdb_bytes[4] = 0x01; // one sector
    ccb.csio.cdb_io.cdb_bytes[5] = 0x00; // LBA low
    ccb.csio.cdb_io.cdb_bytes[6] = 0x4F; // SMART signature mid
    ccb.csio.cdb_io.cdb_bytes[7] = 0xC2; // SMART signature high
    ccb.csio.cdb_io.cdb_bytes[8] = 0x00; // device
    ccb.csio.cdb_io.cdb_bytes[9] = 0xB0; // SMART command
    ccb.csio.cdb_io.cdb_bytes[10] = 0x00; // control

    if (cam_ioctl_compat(fd, &ccb) != 0) {
        ps4smart_logf("[-] SAT ATA PASS-THROUGH(12) ioctl failed: errno=%d (%s)\n",
               errno, strerror(errno));
        return -1;
    }
    unsigned cs = ccb.ccb_h.status & CAM_STATUS_MASK;
    if (cs != CAM_REQ_CMP || ccb.csio.scsi_status != SCSI_STATUS_OK) {
        ps4smart_logf("[-] SAT12 command failed: CAM status=0x%x SCSI status=0x%x\n",
               cs, (unsigned)ccb.csio.scsi_status);
        errno = EIO;
        return -1;
    }
    return 0;
}
#endif

#ifndef HOST_TEST
// Query ATA IDENTIFY through SAT to learn the actual disk model behind USB-SATA.
static int read_identify_sat(int fd, unsigned path_id, unsigned target_id,
                             unsigned target_lun, uint8_t identify[512], int use16) {
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_SCSI_IO;
    ccb.ccb_h.path_id = path_id;
    ccb.ccb_h.target_id = target_id;
    ccb.ccb_h.target_lun = target_lun;
    ccb.ccb_h.timeout = 15000;
    ccb.ccb_h.flags = CAM_DIR_IN;
    ccb.csio.cdb_len = use16 ? 16 : 12;
    ccb.csio.sense_len = SSD_FULL_SIZE;
    ccb.csio.data_ptr = identify;
    ccb.csio.dxfer_len = 512;
    uint8_t *cdb = ccb.csio.cdb_io.cdb_bytes;
    if (use16) {
        cdb[0] = 0x85; cdb[1] = 0x08; cdb[2] = 0x0E;
        cdb[6] = 0x01; cdb[14] = 0xEC;
    } else {
        cdb[0] = 0xA1; cdb[1] = 0x08; cdb[2] = 0x0E;
        cdb[4] = 0x01; cdb[9] = 0xEC;
    }
    if (cam_ioctl_compat(fd, &ccb) != 0) return -1;
    if ((ccb.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP ||
        ccb.csio.scsi_status != SCSI_STATUS_OK) { errno = EIO; return -1; }
    return 0;
}

static void identify_string(const uint8_t identify[512], unsigned first_word,
                            unsigned words, char *out, size_t out_size) {
    size_t n = 0;
    for (unsigned i = 0; i < words && n + 1 < out_size; ++i) {
        unsigned p = (first_word + i) * 2;
        out[n++] = (char)identify[p + 1];
        if (n + 1 < out_size) out[n++] = (char)identify[p];
    }
    out[n] = '\0';
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\0')) out[--n] = '\0';
    size_t start = 0;
    while (out[start] == ' ') ++start;
    if (start) memmove(out, out + start, strlen(out + start) + 1);
}

static void log_identify_model(int fd, unsigned path_id, unsigned target_id,
                               unsigned target_lun) {
    uint8_t identify[512];
    memset(identify, 0, sizeof(identify));
    if (read_identify_sat(fd, path_id, target_id, target_lun, identify, 1) != 0) {
        memset(identify, 0, sizeof(identify));
        if (read_identify_sat(fd, path_id, target_id, target_lun, identify, 0) != 0) {
            ps4smart_logf("[*] ATA IDENTIFY is not forwarded by this USB-SATA bridge\n");
            return;
        }
    }
    char model[41], firmware[9];
    identify_string(identify, 27, 20, model, sizeof(model));
    identify_string(identify, 23, 4, firmware, sizeof(firmware));
    strncpy(g_identified_model, model, sizeof(g_identified_model) - 1);
    g_identified_model[sizeof(g_identified_model) - 1] = '\0';
    if (model[0] == '\0') {
        ps4smart_logf("[*] ATA IDENTIFY returned no readable model string\n");
        return;
    }
    ps4smart_logf("[+] ATA IDENTIFY model='%s' firmware='%s'\n", model, firmware);
}
#endif

int read_smart_from_pass(const char *device_path, SmartData *out_data) {
    if (!device_path || !out_data) {
        errno = EINVAL;
        ps4smart_logf("[-] SMART: invalid argument\n");
        return -1;
    }
#ifdef HOST_TEST
    (void)device_path;
    ps4smart_logf("[-] SMART CAM read is unavailable in HOST_TEST mode\n");
    return -1;
#else
    memset(out_data->data, 0, sizeof(out_data->data));
    int fd = open(device_path, O_RDWR);
    if (fd < 0) {
        ps4smart_logf("[-] open(%s) failed: errno=%d (%s)\n", device_path, errno, strerror(errno));
        return -1;
    }

    union ccb info;
    unsigned path_id = 0, target_id = 0, target_lun = 0;
    int protocol = -1;
    if (get_cam_device(fd, &info, &path_id, &target_id, &target_lun, &protocol) != 0) {
        ps4smart_logf("[-] %s: XPT_GDEV_TYPE failed: errno=%d (%s), CAM status=0x%x\n",
               device_path, errno, strerror(errno), (unsigned)(info.ccb_h.status & CAM_STATUS_MASK));
        close(fd);
        return -1;
    }

    ps4smart_logf("[*] %s: CAM protocol=%d path=%u target=%u lun=%u\n",
           device_path, protocol, path_id, target_id, target_lun);
    g_identified_model[0] = '\0';
    if (protocol != PROTO_ATA) {
        log_identify_model(fd, path_id, target_id, target_lun);
    }
    int result;
    if (protocol == PROTO_ATA) {
        ps4smart_logf("[*] Trying native ATA SMART READ DATA...\n");
        result = read_smart_native_ata(fd, path_id, target_id, target_lun, out_data);
    } else {
        ps4smart_logf("[*] Device reports SCSI; trying SAT ATA PASS-THROUGH(16)...\n");
        result = read_smart_sat16(fd, path_id, target_id, target_lun, out_data);
        if (result != 0) {
            memset(out_data->data, 0, sizeof(out_data->data));
            ps4smart_logf("[*] SAT16 failed; trying SAT ATA PASS-THROUGH(12)...\n");
            result = read_smart_sat12(fd, path_id, target_id, target_lun, out_data);
        }
    }
    close(fd);
    if (result != 0) return -1;

    unsigned sum = 0;
    for (unsigned i = 0; i < sizeof(out_data->data); ++i) sum = (sum + out_data->data[i]) & 0xFF;
    if (sum != 0) {
        ps4smart_logf("[-] 512-byte response checksum invalid (sum=0x%02X); data may not be SMART data\n", sum);
        return -1;
    }
    ps4smart_logf("[+] SMART READ DATA response received from %s (checksum OK)\n", device_path);
    return 0;
#endif
}

static const char *smart_attribute_name(uint8_t id) {
    switch (id) {
        case 0x01: return "Read Error Rate";
        case 0x03: return "Spin-Up Time";
        case 0x04: return "Start/Stop Count";
        case 0x05: return "Reallocated Sectors";
        case 0x07: return "Seek Error Rate";
        case 0x09: return "Power-On Hours";
        case 0x0C: return "Power Cycle Count";
        case 0xB8: return "End-to-End Error";
        case 0xBB: return "Reported Uncorrectable Errors";
        case 0xBC: return "Command Timeout";
        case 0xC0: return "Unsafe Shutdowns / Power-Off Retract";
        case 0xC1: return "Load/Unload Cycle Count";
        case 0xC2: return "Temperature";
        case 0xC4: return "Reallocation Event Count";
        case 0xC5: return "Current Pending Sectors";
        case 0xC6: return "Uncorrectable Sectors";
        case 0xC7: return "UDMA CRC Errors";
        case 0xF1: return "Total Host Writes (vendor-specific)";
        case 0xF2: return "Total Host Reads (vendor-specific)";
        default: return "Unknown/vendor-specific";
    }
}

void parse_smart_output(const SmartData *data) {
    if (!data) {
        ps4smart_logf("[-] SMART data pointer is null");
        return;
    }

    ps4smart_logf("    --- SMART Data Analysis ---");
    ps4smart_logf("    ID  Attribute                              Current Worst Raw (hex)");
    ps4smart_logf("    --  ------------------------------------- ------- ----- ----------------");

    unsigned found = 0;
    for (int i = 0; i < 30; ++i) {
        const int offset = 2 + i * 12;
        const uint8_t id = data->data[offset];
        if (id == 0x00 || id == 0xFF) continue;

        const uint8_t current = data->data[offset + 3];
        const uint8_t worst = data->data[offset + 4];
        uint8_t raw_bytes[6];
        uint64_t raw = 0;
        for (int b = 0; b < 6; ++b) {
            raw_bytes[b] = data->data[offset + 5 + b];
            raw |= (uint64_t)raw_bytes[b] << (b * 8);
        }

        ps4smart_logf("    %02X  %-37s %3u     %3u   0x%012llX",
                      (unsigned)id, smart_attribute_name(id),
                      (unsigned)current, (unsigned)worst,
                      (unsigned long long)raw);

        // SMART raw fields are vendor-specific. Only decode fields with a
        // widely used interpretation, and always retain the original bytes.
        if (id == 0xC2) {
            ps4smart_logf("        -> Temperature: %u C (raw[0]; remaining bytes are drive-specific)",
                          (unsigned)raw_bytes[0]);
            ps4smart_logf("           Raw bytes: %02X %02X %02X %02X %02X %02X",
                          raw_bytes[0], raw_bytes[1], raw_bytes[2],
                          raw_bytes[3], raw_bytes[4], raw_bytes[5]);
        } else if (id == 0x09) {
            // Seagate ST2000LM007 family: bytes 0..3 are power-on hours;
            // bytes 4..5 are the low part of the manufacturer's sub-hour field.
            if (strncmp(g_identified_model, "ST2000LM007", 11) == 0) {
                uint32_t hours = (uint32_t)raw_bytes[0] |
                                 ((uint32_t)raw_bytes[1] << 8) |
                                 ((uint32_t)raw_bytes[2] << 16) |
                                 ((uint32_t)raw_bytes[3] << 24);
                uint32_t subhour_ms = (uint32_t)raw_bytes[4] |
                                      ((uint32_t)raw_bytes[5] << 8);
                ps4smart_logf("        -> Power-on time: %u hours (Seagate raw bytes 0..3)", hours);
                ps4smart_logf("           Sub-hour field: %u ms (manufacturer-specific; not added to hours)", subhour_ms);
            } else if (raw_bytes[3] == 0 && raw_bytes[4] == 0 && raw_bytes[5] == 0) {
                ps4smart_logf("        -> Power-on time candidate: %llu hours (simple raw counter)",
                              (unsigned long long)raw);
            } else {
                ps4smart_logf("        -> Power-on time not decoded: unknown manufacturer/model format");
                ps4smart_logf("           Raw bytes: %02X %02X %02X %02X %02X %02X",
                              raw_bytes[0], raw_bytes[1], raw_bytes[2],
                              raw_bytes[3], raw_bytes[4], raw_bytes[5]);
            }
        } else if ((id == 0xF1 || id == 0xF2) &&
                   strncmp(g_identified_model, "ST2000LM007", 11) == 0) {
            // For this Seagate family, F1/F2 are host-written/read LBA counters.
            // ATA LBA units are 512 bytes; keep the raw sector count visible.
            uint64_t bytes = raw * 512ULL;
            uint64_t tb_whole = bytes / 1000000000000ULL;
            uint64_t tb_frac = (bytes % 1000000000000ULL) / 1000000000ULL;
            ps4smart_logf("        -> %s: %llu sectors, about %llu.%03llu TB (512-byte sectors)",
                          id == 0xF1 ? "Host writes" : "Host reads",
                          (unsigned long long)raw,
                          (unsigned long long)tb_whole,
                          (unsigned long long)tb_frac);
        } else if (id == 0xF1 || id == 0xF2) {
            ps4smart_logf("        -> Vendor-specific counter; not converted for unidentified drive model");
        }
        ++found;
    }

    if (found == 0) {
        ps4smart_logf("    [-] No populated SMART attributes found.");
        ps4smart_logf("    First 64 bytes of response:");
        for (int i = 0; i < 64; ++i) {
            if (i % 16 == 0) ps4smart_logf("    ");
            ps4smart_logf("%02X%s", data->data[i], (i % 16 == 15) ? "\n" : " ");
        }
    }
    ps4smart_logf("    SMART attributes parsed: %u", found);
}
