#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#define QRTR_NODE_BCAST 0xffffffffu
#define QRTR_PORT_CTRL  0xfffffffeu

#define QRTR_TYPE_DATA       1
#define QRTR_TYPE_NEW_SERVER 4
#define QRTR_TYPE_DEL_SERVER 5
#define QRTR_TYPE_NEW_LOOKUP 10

struct sockaddr_qrtr {
    uint16_t sq_family;
    uint32_t sq_node;
    uint32_t sq_port;
};

struct qrtr_ctrl_pkt {
    uint32_t cmd;
    union {
        struct {
            uint32_t service;
            uint32_t instance;
            uint32_t node;
            uint32_t port;
        } server;
        struct {
            uint32_t node;
            uint32_t port;
        } client;
    };
} __attribute__((packed));

#define QMI_REQUEST  0x00
#define QMI_RESPONSE 0x02

#define QMI_RMTFS_SERVICE   14
#define QMI_RMTFS_VERSION   1
#define QMI_RMTFS_OPEN      0x0001
#define QMI_RMTFS_CLOSE     0x0002
#define QMI_RMTFS_RW_IOVEC  0x0003
#define QMI_RMTFS_ALLOC_BUF 0x0004
#define QMI_RMTFS_GET_ERR   0x0005

#define SECTOR_SIZE 512

struct qmi_hdr {
    uint8_t cntl_flag;
    uint16_t txn_id;
    uint16_t msg_id;
    uint16_t msg_len;
} __attribute__((packed));

struct rmtfs_iovec {
    uint32_t sector_addr;
    uint32_t data_phy_offset;
    uint32_t num_sector;
} __attribute__((packed));

struct open_file {
    int fd;
    bool readonly;
    char path[64];
};

static struct open_file g_files[16];
static uint8_t *g_rmtfs_mem = NULL;
static uint64_t g_rmtfs_phys = 0;
static size_t g_rmtfs_size = 0;

static void klog(const char *msg) {
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)write(fd, msg, strlen(msg));
        close(fd);
    }
}

static bool read_sysfs_u64(const char *path, uint64_t *out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    *out = strtoull(buf, NULL, 0);
    return true;
}

static bool init_rmtfs_mem(void) {
    uint64_t phys = 0, sz = 0;
    const char *devs[] = { "/dev/qcom_rmtfs_mem1", "/dev/qcom_rmtfs_mem0" };
    const char *sys_dirs[] = { "/sys/class/rmtfs/qcom_rmtfs_mem1",
                               "/sys/class/rmtfs/qcom_rmtfs_mem0" };

    for (int wait = 0; wait < 30; wait++) {
        for (size_t i = 0; i < 2; i++) {
            char p1[128], p2[128];
            snprintf(p1, sizeof(p1), "%s/phys_addr", sys_dirs[i]);
            snprintf(p2, sizeof(p2), "%s/size", sys_dirs[i]);
            if (read_sysfs_u64(p1, &phys) && read_sysfs_u64(p2, &sz) && sz > 0) {
                int fd = open(devs[i], O_RDWR | O_CLOEXEC);
                if (fd >= 0) {
                    void *m = mmap(NULL, (size_t)sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    close(fd);
                    if (m != MAP_FAILED) {
                        g_rmtfs_mem = (uint8_t *)m;
                        g_rmtfs_phys = phys;
                        g_rmtfs_size = (size_t)sz;
                        char msg[160];
                        snprintf(msg, sizeof(msg),
                                 "<6>sailfish_rmtfs: mapped %s phys=%#llx size=%#zx\n",
                                 devs[i], (unsigned long long)phys, g_rmtfs_size);
                        klog(msg);
                        return true;
                    }
                }
            }
        }
        usleep(100000);
    }
    klog("<3>sailfish_rmtfs: failed to map /dev/qcom_rmtfs_mem*\n");
    return false;
}

static int open_partition(const char *qmi_path, bool *ro_out, const char **used_dev) {
    const char *primary = NULL;
    const char *fallback = NULL;
    *ro_out = false;
    if (strcmp(qmi_path, "/boot/modem_fs1") == 0) {
        primary = "/dev/block/by-name/modemst1";
        fallback = "/dev/sdd9";
    } else if (strcmp(qmi_path, "/boot/modem_fs2") == 0) {
        primary = "/dev/block/by-name/modemst2";
        fallback = "/dev/sdd10";
    } else if (strcmp(qmi_path, "/boot/modem_fsg") == 0) {
        *ro_out = true;
        primary = "/dev/block/by-name/fsg";
        fallback = "/dev/sdf3";
    } else if (strcmp(qmi_path, "/boot/modem_fsc") == 0) {
        primary = "/dev/block/by-name/fsc";
        fallback = "/dev/sdd8";
    } else {
        *used_dev = "unknown";
        return -1;
    }

    const char *cands[2] = { primary, fallback };
    for (int i = 0; i < 2; i++) {
        *used_dev = cands[i];
        int fd = open(cands[i], *ro_out ? (O_RDONLY | O_CLOEXEC) : (O_RDWR | O_CLOEXEC));
        if (fd >= 0)
            return fd;
        if (!*ro_out) {
            fd = open(cands[i], O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                *ro_out = true;
                return fd;
            }
        }
    }
    return -1;
}

static size_t append_tlv(uint8_t *buf, size_t off, uint8_t type,
                         const void *val, uint16_t len) {
    buf[off] = type;
    buf[off + 1] = (uint8_t)(len & 0xff);
    buf[off + 2] = (uint8_t)((len >> 8) & 0xff);
    if (len > 0 && val)
        memcpy(buf + off + 3, val, len);
    return off + 3 + len;
}

static size_t append_result_tlv(uint8_t *buf, size_t off, uint16_t result, uint16_t err) {
    uint16_t v[2] = { result, err };
    return append_tlv(buf, off, 0x02, v, sizeof(v));
}

static void send_qmi_resp(int sock, const struct sockaddr_qrtr *dst,
                          uint16_t txn_id, uint16_t msg_id,
                          const uint8_t *tlv, size_t tlv_len) {
    uint8_t pkt[256];
    struct qmi_hdr hdr = {
        .cntl_flag = QMI_RESPONSE,
        .txn_id = txn_id,
        .msg_id = msg_id,
        .msg_len = (uint16_t)tlv_len,
    };
    memcpy(pkt, &hdr, sizeof(hdr));
    if (tlv_len > 0)
        memcpy(pkt + sizeof(hdr), tlv, tlv_len);
    (void)sendto(sock, pkt, sizeof(hdr) + tlv_len, 0,
                 (const struct sockaddr *)dst, sizeof(*dst));
}

static void handle_qmi_request(int sock, const struct sockaddr_qrtr *src,
                               const uint8_t *buf, size_t len) {
    if (len < sizeof(struct qmi_hdr))
        return;
    struct qmi_hdr hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.cntl_flag != QMI_REQUEST)
        return;

    const uint8_t *p = buf + sizeof(hdr);
    size_t rem = len - sizeof(hdr);
    if (hdr.msg_len < rem)
        rem = hdr.msg_len;

    uint8_t resp[128];
    size_t rlen = 0;

    if (hdr.msg_id == QMI_RMTFS_OPEN) {
        char req_path[64] = {0};
        size_t pos = 0;
        while (pos + 3 <= rem) {
            uint8_t t = p[pos];
            uint16_t l = (uint16_t)(p[pos + 1] | ((uint16_t)p[pos + 2] << 8));
            pos += 3;
            if (pos + l > rem) break;
            if (t == 0x01 && l < sizeof(req_path)) {
                memcpy(req_path, p + pos, l);
                req_path[l] = '\0';
            }
            pos += l;
        }
        bool ro = false;
        const char *dev = NULL;
        int slot = -1;
        int fd = open_partition(req_path, &ro, &dev);
        int open_err = errno;
        if (fd >= 0) {
            for (int i = 1; i < 16; i++) {
                if (g_files[i].fd >= 0 && strcmp(g_files[i].path, req_path) == 0) {
                    close(g_files[i].fd);
                    g_files[i].fd = -1;
                }
            }
            for (int i = 1; i < 16; i++) {
                if (g_files[i].fd < 0) {
                    g_files[i].fd = fd;
                    g_files[i].readonly = ro;
                    snprintf(g_files[i].path, sizeof(g_files[i].path), "%s", req_path);
                    slot = i;
                    break;
                }
            }
            if (slot < 0) close(fd);
        }
        char msg[160];
        snprintf(msg, sizeof(msg), "<6>sailfish_rmtfs: OPEN '%s' -> %s (cid=%d ro=%d err=%d)\n",
                 req_path, dev ? dev : "none", slot, ro ? 1 : 0, fd >= 0 ? 0 : open_err);
        klog(msg);
        if (slot > 0) {
            uint32_t cid = (uint32_t)slot;
            rlen = append_result_tlv(resp, rlen, 0, 0);
            rlen = append_tlv(resp, rlen, 0x10, &cid, sizeof(cid));
        } else {
            rlen = append_result_tlv(resp, rlen, 1, 0x0030);
        }
        send_qmi_resp(sock, src, hdr.txn_id, hdr.msg_id, resp, rlen);
    } else if (hdr.msg_id == QMI_RMTFS_CLOSE) {
        uint32_t cid = 0;
        size_t pos = 0;
        while (pos + 3 <= rem) {
            uint8_t t = p[pos];
            uint16_t l = (uint16_t)(p[pos + 1] | ((uint16_t)p[pos + 2] << 8));
            pos += 3;
            if (pos + l > rem) break;
            if (t == 0x01 && l == 4)
                memcpy(&cid, p + pos, 4);
            pos += l;
        }
        if (cid > 0 && cid < 16 && g_files[cid].fd >= 0) {
            close(g_files[cid].fd);
            g_files[cid].fd = -1;
        }
        rlen = append_result_tlv(resp, rlen, 0, 0);
        send_qmi_resp(sock, src, hdr.txn_id, hdr.msg_id, resp, rlen);
    } else if (hdr.msg_id == QMI_RMTFS_RW_IOVEC) {
        uint32_t cid = 0;
        uint8_t dir = 0;
        uint8_t force_sync = 0;
        const uint8_t *iov_ptr = NULL;
        uint8_t iov_cnt = 0;
        size_t pos = 0;
        while (pos + 3 <= rem) {
            uint8_t t = p[pos];
            uint16_t l = (uint16_t)(p[pos + 1] | ((uint16_t)p[pos + 2] << 8));
            pos += 3;
            if (pos + l > rem) break;
            if (t == 0x01 && l == 4) memcpy(&cid, p + pos, 4);
            else if (t == 0x02 && l == 1) dir = p[pos];
            else if (t == 0x03 && l >= 1) {
                iov_cnt = p[pos];
                if (1U + (size_t)iov_cnt * sizeof(struct rmtfs_iovec) <= l)
                    iov_ptr = p + pos + 1;
            } else if (t == 0x04 && l == 1) force_sync = p[pos];
            pos += l;
        }
        bool ok = (cid > 0 && cid < 16 && g_files[cid].fd >= 0 && g_rmtfs_mem && iov_ptr);
        uint32_t first_sec = 0, first_off = 0, first_num = 0;
        if (ok) {
            for (uint8_t i = 0; i < iov_cnt; i++) {
                struct rmtfs_iovec v;
                memcpy(&v, iov_ptr + i * sizeof(v), sizeof(v));
                if (i == 0) {
                    first_sec = v.sector_addr;
                    first_off = v.data_phy_offset;
                    first_num = v.num_sector;
                }
                size_t bytes = (size_t)v.num_sector * SECTOR_SIZE;
                off_t disk_off = (off_t)v.sector_addr * SECTOR_SIZE;
                uint64_t off64 = v.data_phy_offset;
                if (off64 >= g_rmtfs_phys && off64 < g_rmtfs_phys + g_rmtfs_size)
                    off64 -= g_rmtfs_phys;
                if (off64 + bytes > g_rmtfs_size) {
                    ok = false;
                    break;
                }
                if (dir == 0) {
                    ssize_t r = pread(g_files[cid].fd, g_rmtfs_mem + off64,
                                      bytes, disk_off);
                    if (r < 0) {
                        ok = false;
                        break;
                    }
                    if ((size_t)r < bytes)
                        memset(g_rmtfs_mem + off64 + r, 0, bytes - (size_t)r);
                } else {
                    if (!g_files[cid].readonly) {
                        ssize_t w = pwrite(g_files[cid].fd, g_rmtfs_mem + off64,
                                           bytes, disk_off);
                        if (w != (ssize_t)bytes) {
                            ok = false;
                            break;
                        }
                    }
                }
            }
            if (ok && dir != 0 && force_sync && !g_files[cid].readonly)
                fsync(g_files[cid].fd);
        }
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "<6>sailfish_rmtfs: RW cid=%u dir=%u cnt=%u sec=%u off=%#x n=%u ok=%d\n",
                 cid, dir, iov_cnt, first_sec, first_off, first_num, ok ? 1 : 0);
        klog(msg);
        rlen = append_result_tlv(resp, rlen, ok ? 0 : 1, ok ? 0 : 0x0030);
        send_qmi_resp(sock, src, hdr.txn_id, hdr.msg_id, resp, rlen);
    } else if (hdr.msg_id == QMI_RMTFS_ALLOC_BUF) {
        char msg[128];
        snprintf(msg, sizeof(msg), "<6>sailfish_rmtfs: ALLOC_BUF -> %#llx\n",
                 (unsigned long long)g_rmtfs_phys);
        klog(msg);
        rlen = append_result_tlv(resp, rlen, 0, 0);
        rlen = append_tlv(resp, rlen, 0x10, &g_rmtfs_phys, sizeof(g_rmtfs_phys));
        send_qmi_resp(sock, src, hdr.txn_id, hdr.msg_id, resp, rlen);
    } else if (hdr.msg_id == QMI_RMTFS_GET_ERR) {
        uint32_t st = 0;
        rlen = append_result_tlv(resp, rlen, 0, 0);
        rlen = append_tlv(resp, rlen, 0x10, &st, sizeof(st));
        send_qmi_resp(sock, src, hdr.txn_id, hdr.msg_id, resp, rlen);
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "<6>sailfish_rmtfs: UNK msg_id=%#x len=%zu\n",
                 hdr.msg_id, rem);
        klog(msg);
        rlen = append_result_tlv(resp, rlen, 1, 0x0001);
        send_qmi_resp(sock, src, hdr.txn_id, hdr.msg_id, resp, rlen);
    }
}

static void start_modem_rproc(void) {
    int pfd = open("/sys/module/firmware_class/parameters/path", O_WRONLY | O_CLOEXEC);
    if (pfd >= 0) {
        const char *mpath = "/mnt/vendor/modem_fw";
        (void)write(pfd, mpath, strlen(mpath));
        close(pfd);
    }
    int rfd = open("/sys/class/remoteproc/remoteproc0/state", O_WRONLY | O_CLOEXEC);
    if (rfd >= 0) {
        ssize_t w = write(rfd, "start\n", 6);
        close(rfd);
        char msg[128];
        snprintf(msg, sizeof(msg), "<6>sailfish_rmtfs: triggered remoteproc0 start (rc=%zd)\n", w);
        klog(msg);
    }
    pfd = open("/sys/module/firmware_class/parameters/path", O_WRONLY | O_CLOEXEC);
    if (pfd >= 0) {
        const char *defpath = "/vendor/firmware,/tools/firmware";
        (void)write(pfd, defpath, strlen(defpath));
        close(pfd);
    }
}

int main(void) {
    for (int i = 0; i < 16; i++)
        g_files[i].fd = -1;

    if (!init_rmtfs_mem())
        return 1;

    int sock = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        klog("<3>sailfish_rmtfs: socket(AF_QIPCRTR) failed\n");
        return 1;
    }

    struct sockaddr_qrtr local;
    memset(&local, 0, sizeof(local));
    socklen_t slen = sizeof(local);
    if (getsockname(sock, (struct sockaddr *)&local, &slen) < 0) {
        klog("<3>sailfish_rmtfs: getsockname(AF_QIPCRTR) failed\n");
        return 1;
    }

    struct sockaddr_qrtr ctrl = {
        .sq_family = AF_QIPCRTR,
        .sq_node = local.sq_node,
        .sq_port = QRTR_PORT_CTRL,
    };

    struct qrtr_ctrl_pkt reg = {
        .cmd = QRTR_TYPE_NEW_SERVER,
        .server = {
            .service = QMI_RMTFS_SERVICE,
            .instance = QMI_RMTFS_VERSION,
            .node = local.sq_node,
            .port = local.sq_port,
        },
    };
    if (sendto(sock, &reg, sizeof(reg), 0, (struct sockaddr *)&ctrl, sizeof(ctrl)) < 0) {
        klog("<3>sailfish_rmtfs: NEW_SERVER failed\n");
        return 1;
    }

    struct qrtr_ctrl_pkt lkp = {
        .cmd = QRTR_TYPE_NEW_LOOKUP,
        .server = { .service = 0, .instance = 0, .node = 0, .port = 0 },
    };
    (void)sendto(sock, &lkp, sizeof(lkp), 0, (struct sockaddr *)&ctrl, sizeof(ctrl));

    char msg[128];
    snprintf(msg, sizeof(msg),
             "<6>sailfish_rmtfs: registered QMI RMTFS(14) at node=%u port=%u\n",
             local.sq_node, local.sq_port);
    klog(msg);

    start_modem_rproc();

    uint8_t buf[4096];
    for (;;) {
        struct sockaddr_qrtr src;
        socklen_t srclen = sizeof(src);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &srclen);
        if (n <= 0)
            continue;
        if (src.sq_port == QRTR_PORT_CTRL && (size_t)n >= sizeof(struct qrtr_ctrl_pkt)) {
            struct qrtr_ctrl_pkt cp;
            memcpy(&cp, buf, sizeof(cp));
            if (cp.cmd == QRTR_TYPE_NEW_SERVER && cp.server.service != 0) {
                snprintf(msg, sizeof(msg),
                         "<6>sailfish_rmtfs: QMI svc=%u ver=%u inst=%u node=%u port=%u\n",
                         cp.server.service, cp.server.instance & 0xff,
                         cp.server.instance >> 8, cp.server.node, cp.server.port);
                klog(msg);
            }
            continue;
        }
        handle_qmi_request(sock, &src, buf, (size_t)n);
    }
    return 0;
}
