#ifndef SAILFISH_QMI_CLIENT_H
#define SAILFISH_QMI_CLIENT_H

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#define QRTR_NODE_BCAST  0xffffffffu
#define QRTR_PORT_CTRL   0xfffffffeu

enum {
    QRTR_TYPE_DATA       = 1,
    QRTR_TYPE_HELLO      = 2,
    QRTR_TYPE_BYE        = 3,
    QRTR_TYPE_NEW_SERVER = 4,
    QRTR_TYPE_DEL_SERVER = 5,
    QRTR_TYPE_DEL_CLIENT = 6,
    QRTR_TYPE_RESUME_TX  = 7,
    QRTR_TYPE_EXIT       = 8,
    QRTR_TYPE_PING       = 9,
    QRTR_TYPE_NEW_LOOKUP = 10,
    QRTR_TYPE_DEL_LOOKUP = 11,
};

struct sockaddr_qrtr {
    sa_family_t sq_family;
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
};

/* Standard Qualcomm QMI Service IDs */
enum {
    QMI_SVC_WDS   = 1,
    QMI_SVC_DMS   = 2,
    QMI_SVC_NAS   = 3,
    QMI_SVC_WMS   = 5,
    QMI_SVC_VOICE = 9,
    QMI_SVC_UIM   = 11,
    QMI_SVC_LOC   = 16,
    QMI_SVC_WDA   = 26,
    QMI_SVC_DPM   = 47,
};

#define QMI_TYPE_REQ  0x00
#define QMI_TYPE_RESP 0x02
#define QMI_TYPE_IND  0x04

struct qmi_hdr {
    uint8_t  type;
    uint16_t txn_id;
    uint16_t msg_id;
    uint16_t msg_len;
} __attribute__((packed));

struct qmi_tlv_hdr {
    uint8_t  type;
    uint16_t len;
} __attribute__((packed));

struct qmi_service_addr {
    uint32_t service;
    uint32_t version;
    uint32_t instance;
    uint32_t node;
    uint32_t port;
    bool     found;
};

static inline uint16_t qmi_rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t qmi_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void qmi_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static inline void qmi_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static inline size_t qmi_tlv_append(uint8_t *buf, size_t off, size_t cap,
                                    uint8_t type, const void *val, uint16_t len) {
    if (off + 3 + len > cap)
        return off;
    buf[off] = type;
    qmi_wr16(buf + off + 1, len);
    if (len && val)
        memcpy(buf + off + 3, val, len);
    return off + 3 + len;
}

static inline const uint8_t *qmi_tlv_find(const uint8_t *tlvs, size_t tlvs_len,
                                          uint8_t type, uint16_t *out_len) {
    size_t off = 0;
    while (off + 3 <= tlvs_len) {
        uint8_t t = tlvs[off];
        uint16_t l = qmi_rd16(tlvs + off + 1);
        if (off + 3 + l > tlvs_len)
            break;
        if (t == type) {
            if (out_len) *out_len = l;
            return tlvs + off + 3;
        }
        off += 3 + l;
    }
    return NULL;
}

static inline int qmi_result_code(const uint8_t *tlvs, size_t tlvs_len, uint16_t *out_err) {
    uint16_t len = 0;
    const uint8_t *v = qmi_tlv_find(tlvs, tlvs_len, 0x02, &len);
    if (!v || len < 4) {
        if (out_err) *out_err = 0xffff;
        return -1;
    }
    uint16_t res = qmi_rd16(v);
    uint16_t err = qmi_rd16(v + 2);
    if (out_err) *out_err = err;
    return (res == 0) ? 0 : (int)err;
}

static inline int qmi_open_socket(uint32_t *out_local_node) {
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_qrtr me;
    socklen_t sl = sizeof(me);
    memset(&me, 0, sizeof(me));
    if (getsockname(fd, (struct sockaddr *)&me, &sl) < 0) {
        close(fd);
        return -1;
    }
    if (out_local_node)
        *out_local_node = me.sq_node;
    return fd;
}

/* Lookup a single QMI service via QRTR nameserver (node=local_node, port=QRTR_PORT_CTRL). */
static inline int qmi_lookup_service(int fd, uint32_t local_node, uint32_t service,
                                     struct qmi_service_addr *out, int timeout_ms) {
    struct qrtr_ctrl_pkt pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
    pkt.server.service = service;
    pkt.server.instance = 0;

    struct sockaddr_qrtr ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.sq_family = AF_QIPCRTR;
    ctrl.sq_node = local_node;
    ctrl.sq_port = QRTR_PORT_CTRL;

    if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&ctrl, sizeof(ctrl)) < 0)
        return -1;

    memset(out, 0, sizeof(*out));
    int waited = 0;
    while (waited < timeout_ms) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int rc = poll(&pfd, 1, 100);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rc == 0) {
            waited += 100;
            continue;
        }
        uint8_t buf[256];
        struct sockaddr_qrtr src;
        socklen_t sl = sizeof(src);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &sl);
        if (n >= (ssize_t)sizeof(struct qrtr_ctrl_pkt) && src.sq_port == QRTR_PORT_CTRL) {
            struct qrtr_ctrl_pkt *rp = (struct qrtr_ctrl_pkt *)buf;
            if (rp->cmd == QRTR_TYPE_NEW_SERVER) {
                if (rp->server.service == 0 && rp->server.instance == 0 &&
                    rp->server.node == 0 && rp->server.port == 0) {
                    /* End of current server list */
                    break;
                }
                if (rp->server.service == service) {
                    out->service  = rp->server.service;
                    out->version  = rp->server.instance & 0xff;
                    out->instance = rp->server.instance >> 8;
                    out->node     = rp->server.node;
                    out->port     = rp->server.port;
                    out->found    = true;
                    break;
                }
            }
        }
    }

    /* Clean up lookup subscription */
    pkt.cmd = QRTR_TYPE_DEL_LOOKUP;
    sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&ctrl, sizeof(ctrl));
    return out->found ? 0 : -ENOENT;
}

/* Perform a synchronous QMI REQ -> RESP transaction on a dedicated or shared socket. */
static inline int qmi_transact(int fd, const struct qmi_service_addr *svc,
                               uint16_t txn_id, uint16_t msg_id,
                               const uint8_t *req_tlvs, uint16_t req_len,
                               uint8_t *rsp_tlvs, size_t rsp_cap, size_t *out_rsp_len,
                               int timeout_ms) {
    uint8_t tx[2048];
    if (sizeof(struct qmi_hdr) + req_len > sizeof(tx))
        return -EMSGSIZE;

    struct qmi_hdr *h = (struct qmi_hdr *)tx;
    h->type    = QMI_TYPE_REQ;
    h->txn_id  = txn_id;
    h->msg_id  = msg_id;
    h->msg_len = req_len;
    if (req_len && req_tlvs)
        memcpy(tx + sizeof(*h), req_tlvs, req_len);

    struct sockaddr_qrtr dst;
    memset(&dst, 0, sizeof(dst));
    dst.sq_family = AF_QIPCRTR;
    dst.sq_node   = svc->node;
    dst.sq_port   = svc->port;

    if (sendto(fd, tx, sizeof(*h) + req_len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0)
        return -errno;

    int waited = 0;
    while (waited < timeout_ms) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int rc = poll(&pfd, 1, 100);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (rc == 0) {
            waited += 100;
            continue;
        }
        uint8_t rx[4096];
        struct sockaddr_qrtr src;
        socklen_t sl = sizeof(src);
        ssize_t n = recvfrom(fd, rx, sizeof(rx), 0, (struct sockaddr *)&src, &sl);
        if (n < (ssize_t)sizeof(struct qmi_hdr))
            continue;
        if (src.sq_port == QRTR_PORT_CTRL)
            continue;
        struct qmi_hdr *rh = (struct qmi_hdr *)rx;
        if (rh->type == QMI_TYPE_RESP && rh->txn_id == txn_id && rh->msg_id == msg_id) {
            size_t plen = rh->msg_len;
            if (sizeof(*rh) + plen > (size_t)n)
                plen = (size_t)n - sizeof(*rh);
            if (plen > rsp_cap)
                plen = rsp_cap;
            if (rsp_tlvs && plen)
                memcpy(rsp_tlvs, rx + sizeof(*rh), plen);
            if (out_rsp_len)
                *out_rsp_len = plen;
            return 0;
        }
    }
    return -ETIMEDOUT;
}

#endif /* SAILFISH_QMI_CLIENT_H */
