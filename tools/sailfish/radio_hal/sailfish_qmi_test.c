/*
 * sailfish_qmi_test - Live QMI probe over AF_QIPCRTR for Pixel 1 (MSM8996).
 *
 * Queries DMS (IMEI, baseband, operating mode), UIM (SIM card status, ICCID, IMSI),
 * and NAS (serving system, PLMN, signal strength). Pass --online to switch DMS to ONLINE.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "qmi_client.h"

static uint16_t g_txn = 1;
static uint16_t next_txn(void) {
    uint16_t t = g_txn++;
    if (g_txn == 0) g_txn = 1;
    return t;
}

static const char *dms_mode_str(uint8_t m) {
    switch (m) {
    case 0: return "ONLINE";
    case 1: return "LOW_POWER";
    case 2: return "FTM";
    case 3: return "OFFLINE";
    case 4: return "RESETTING";
    case 5: return "SHUTTING_DOWN";
    case 6: return "PERSISTENT_LOW_POWER";
    default: return "UNKNOWN";
    }
}

static const char *uim_app_type_str(uint8_t t) {
    switch (t) {
    case 1: return "SIM";
    case 2: return "USIM";
    case 3: return "RUIM";
    case 4: return "CSIM";
    case 5: return "ISIM";
    default: return "UNKNOWN";
    }
}

static const char *uim_app_state_str(uint8_t s) {
    switch (s) {
    case 1: return "DETECTED";
    case 2: return "PIN1_REQ";
    case 3: return "PUK1_REQ";
    case 4: return "PERSO";
    case 5: return "PIN1_BLOCKED";
    case 6: return "ILLEGAL";
    case 7: return "READY";
    default: return "UNKNOWN";
    }
}

static const char *nas_reg_str(uint8_t r) {
    switch (r) {
    case 0: return "NOT_REGISTERED";
    case 1: return "REGISTERED";
    case 2: return "SEARCHING";
    case 3: return "DENIED";
    case 4: return "UNKNOWN";
    default: return "OTHER";
    }
}

static const char *nas_rat_str(uint8_t r) {
    switch (r) {
    case 1: return "CDMA_1X";
    case 2: return "EVDO";
    case 4: return "GSM";
    case 5: return "UMTS";
    case 8: return "LTE";
    case 9: return "TD_SCDMA";
    default: return "NONE/OTHER";
    }
}

static void decode_bcd_iccid(const uint8_t *b, size_t len, char *out, size_t cap) {
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < cap; i++) {
        uint8_t lo = b[i] & 0x0f;
        uint8_t hi = (b[i] >> 4) & 0x0f;
        if (lo <= 9) out[pos++] = (char)('0' + lo);
        if (hi <= 9) out[pos++] = (char)('0' + hi);
    }
    out[pos] = '\0';
}

static void decode_imsi(const uint8_t *b, size_t len, char *out, size_t cap) {
    size_t pos = 0;
    if (len < 2) { out[0] = '\0'; return; }
    uint8_t nbytes = b[0];
    if (1 + nbytes > len) nbytes = (uint8_t)(len - 1);
    for (size_t i = 1; i <= nbytes && pos + 2 < cap; i++) {
        uint8_t lo = b[i] & 0x0f;
        uint8_t hi = (b[i] >> 4) & 0x0f;
        if (i == 1) {
            /* First byte low nibble is parity/length, high nibble is first IMSI digit */
            if (hi <= 9) out[pos++] = (char)('0' + hi);
        } else {
            if (lo <= 9) out[pos++] = (char)('0' + lo);
            if (hi <= 9) out[pos++] = (char)('0' + hi);
        }
    }
    out[pos] = '\0';
}

static void probe_dms(int fd, uint32_t local_node, bool set_online) {
    struct qmi_service_addr svc;
    if (qmi_lookup_service(fd, local_node, QMI_SVC_DMS, &svc, 2000) < 0) {
        printf("[DMS] service not found\n");
        return;
    }
    printf("[DMS] node=%u port=%u ver=%u\n", svc.node, svc.port, svc.version);

    uint8_t rsp[1024];
    size_t rlen = 0;
    uint16_t qerr = 0;

    /* 0x0023 Get Device Rev ID */
    if (qmi_transact(fd, &svc, next_txn(), 0x0023, NULL, 0, rsp, sizeof(rsp), &rlen, 2000) == 0 &&
        qmi_result_code(rsp, rlen, &qerr) == 0) {
        uint16_t l = 0;
        const uint8_t *v = qmi_tlv_find(rsp, rlen, 0x01, &l);
        if (v) printf("  Baseband: %.*s\n", (int)l, (const char *)v);
    }

    /* 0x0025 Get Device Serial Numbers */
    if (qmi_transact(fd, &svc, next_txn(), 0x0025, NULL, 0, rsp, sizeof(rsp), &rlen, 2000) == 0 &&
        qmi_result_code(rsp, rlen, &qerr) == 0) {
        uint16_t l = 0;
        const uint8_t *imei = qmi_tlv_find(rsp, rlen, 0x11, &l);
        if (imei) printf("  IMEI:     %.*s\n", (int)l, (const char *)imei);
        const uint8_t *meid = qmi_tlv_find(rsp, rlen, 0x12, &l);
        if (meid) printf("  MEID:     %.*s\n", (int)l, (const char *)meid);
    }

    /* 0x002D Get Operating Mode */
    uint8_t cur_mode = 0xff;
    if (qmi_transact(fd, &svc, next_txn(), 0x002D, NULL, 0, rsp, sizeof(rsp), &rlen, 2000) == 0 &&
        qmi_result_code(rsp, rlen, &qerr) == 0) {
        uint16_t l = 0;
        const uint8_t *m = qmi_tlv_find(rsp, rlen, 0x01, &l);
        if (m && l >= 1) {
            cur_mode = m[0];
            printf("  OpMode:   %u (%s)\n", cur_mode, dms_mode_str(cur_mode));
        }
    }

    if (set_online && cur_mode != 0) {
        uint8_t req[16];
        uint8_t mode = 0; /* ONLINE */
        size_t req_len = qmi_tlv_append(req, 0, sizeof(req), 0x01, &mode, 1);
        int rc = qmi_transact(fd, &svc, next_txn(), 0x002E, req, (uint16_t)req_len,
                              rsp, sizeof(rsp), &rlen, 3000);
        int qrc = (rc == 0) ? qmi_result_code(rsp, rlen, &qerr) : rc;
        printf("  SetOpMode(ONLINE): rc=%d qerr=%u\n", qrc, qerr);
        sleep(2);
    }
}

static void uim_read_ef(int fd, const struct qmi_service_addr *svc,
                        uint8_t session_type, uint16_t file_id,
                        const uint8_t *path, uint8_t path_len,
                        uint8_t *out, size_t out_cap, size_t *out_len) {
    uint8_t req[64];
    size_t off = 0;

    uint8_t sess[2] = { session_type, 0 };
    off = qmi_tlv_append(req, off, sizeof(req), 0x01, sess, sizeof(sess));

    uint8_t fid[16];
    qmi_wr16(fid, file_id);
    fid[2] = path_len;
    if (path_len) memcpy(fid + 3, path, path_len);
    off = qmi_tlv_append(req, off, sizeof(req), 0x02, fid, (uint16_t)(3 + path_len));

    uint8_t rd[4] = { 0, 0, 0, 0 }; /* offset=0, length=0 (whole file) */
    off = qmi_tlv_append(req, off, sizeof(req), 0x03, rd, sizeof(rd));

    uint8_t rsp[512];
    size_t rlen = 0;
    uint16_t qerr = 0;
    *out_len = 0;
    if (qmi_transact(fd, svc, next_txn(), 0x0020, req, (uint16_t)off,
                     rsp, sizeof(rsp), &rlen, 2000) == 0 &&
        qmi_result_code(rsp, rlen, &qerr) == 0) {
        uint16_t l = 0;
        const uint8_t *v = qmi_tlv_find(rsp, rlen, 0x11, &l);
        if (v && l >= 2) {
            uint16_t clen = qmi_rd16(v);
            if (clen > l - 2) clen = l - 2;
            if (clen > out_cap) clen = (uint16_t)out_cap;
            memcpy(out, v + 2, clen);
            *out_len = clen;
        }
    }
}

static void probe_uim(int fd, uint32_t local_node) {
    struct qmi_service_addr svc;
    if (qmi_lookup_service(fd, local_node, QMI_SVC_UIM, &svc, 2000) < 0) {
        printf("[UIM] service not found\n");
        return;
    }
    printf("[UIM] node=%u port=%u ver=%u\n", svc.node, svc.port, svc.version);

    uint8_t rsp[1024];
    size_t rlen = 0;
    uint16_t qerr = 0;
    if (qmi_transact(fd, &svc, next_txn(), 0x002F, NULL, 0, rsp, sizeof(rsp), &rlen, 2000) == 0 &&
        qmi_result_code(rsp, rlen, &qerr) == 0) {
        uint16_t l = 0;
        const uint8_t *v = qmi_tlv_find(rsp, rlen, 0x10, &l);
        if (v && l >= 9) {
            uint16_t gw_pri = qmi_rd16(v);
            uint8_t num_slots = v[8];
            printf("  gw_pri=0x%04x num_slots=%u\n", gw_pri, num_slots);
            size_t pos = 9;
            for (uint8_t s = 0; s < num_slots && pos + 6 <= l; s++) {
                uint8_t cstate = v[pos];
                uint8_t cerr   = v[pos + 4];
                uint8_t napps  = v[pos + 5];
                pos += 6;
                printf("  Slot %u: card_state=%u (%s) err=%u num_apps=%u\n",
                       s, cstate, cstate == 1 ? "PRESENT" : (cstate == 0 ? "ABSENT" : "ERROR"),
                       cerr, napps);
                for (uint8_t a = 0; a < napps && pos + 7 <= l; a++) {
                    uint8_t atype  = v[pos];
                    uint8_t astate = v[pos + 1];
                    uint8_t aidlen = v[pos + 6];
                    pos += 7;
                    if (pos + aidlen + 7 > l) break;
                    printf("    App %u: type=%s(%u) state=%s(%u) aid=",
                           a, uim_app_type_str(atype), atype,
                           uim_app_state_str(astate), astate);
                    for (uint8_t k = 0; k < aidlen; k++) printf("%02X", v[pos + k]);
                    printf("\n");
                    pos += aidlen + 7;
                }
            }
        }
    } else {
        printf("  GET_CARD_STATUS failed qerr=%u\n", qerr);
    }

    uint8_t buf[64];
    size_t blen = 0;
    const uint8_t path_mf[2] = { 0x00, 0x3F };
    uim_read_ef(fd, &svc, 6 /* CARD_SLOT_1 */, 0x2FE2, path_mf, 2, buf, sizeof(buf), &blen);
    if (blen > 0) {
        char iccid[40];
        decode_bcd_iccid(buf, blen, iccid, sizeof(iccid));
        printf("  ICCID:    %s\n", iccid);
    }

    const uint8_t path_usim[4] = { 0x00, 0x3F, 0xFF, 0x7F };
    uim_read_ef(fd, &svc, 0 /* PRIMARY_GW */, 0x6F07, path_usim, 4, buf, sizeof(buf), &blen);
    if (blen > 0) {
        char imsi[40];
        decode_imsi(buf, blen, imsi, sizeof(imsi));
        printf("  IMSI:     %s\n", imsi);
    }
}

static void probe_nas(int fd, uint32_t local_node) {
    struct qmi_service_addr svc;
    if (qmi_lookup_service(fd, local_node, QMI_SVC_NAS, &svc, 2000) < 0) {
        printf("[NAS] service not found\n");
        return;
    }
    printf("[NAS] node=%u port=%u ver=%u\n", svc.node, svc.port, svc.version);

    uint8_t rsp[1024];
    size_t rlen = 0;
    uint16_t qerr = 0;
    if (qmi_transact(fd, &svc, next_txn(), 0x0024, NULL, 0, rsp, sizeof(rsp), &rlen, 2000) == 0 &&
        qmi_result_code(rsp, rlen, &qerr) == 0) {
        uint16_t l = 0;
        const uint8_t *ss = qmi_tlv_find(rsp, rlen, 0x01, &l);
        if (ss && l >= 5) {
            uint8_t reg = ss[0], cs = ss[1], ps = ss[2], nrat = ss[4];
            printf("  Serving:  reg=%s(%u) cs=%u ps=%u rats=",
                   nas_reg_str(reg), reg, cs, ps);
            for (uint8_t i = 0; i < nrat && 5 + i < l; i++) {
                printf("%s%s", i ? "," : "", nas_rat_str(ss[5 + i]));
            }
            printf("\n");
        }
        const uint8_t *plmn = qmi_tlv_find(rsp, rlen, 0x12, &l);
        if (plmn && l >= 5) {
            uint16_t mcc = qmi_rd16(plmn);
            uint16_t mnc = qmi_rd16(plmn + 2);
            uint8_t dlen = plmn[4];
            if (5 + dlen > l) dlen = (uint8_t)(l - 5);
            printf("  PLMN:     mcc=%u mnc=%u desc='%.*s'\n",
                   mcc, mnc, (int)dlen, (const char *)(plmn + 5));
        }
        const uint8_t *cid = qmi_tlv_find(rsp, rlen, 0x1E, &l);
        if (cid && l >= 4)
            printf("  CellID:   0x%08x (%u)\n", qmi_rd32(cid), qmi_rd32(cid));
    }

    if (qmi_transact(fd, &svc, next_txn(), 0x004F, NULL, 0, rsp, sizeof(rsp), &rlen, 2000) == 0 &&
        qmi_result_code(rsp, rlen, &qerr) == 0) {
        uint16_t l = 0;
        const uint8_t *lte = qmi_tlv_find(rsp, rlen, 0x14, &l);
        if (lte && l >= 6) {
            int8_t rssi = (int8_t)lte[0];
            int8_t rsrq = (int8_t)lte[1];
            int16_t rsrp = (int16_t)qmi_rd16(lte + 2);
            int16_t snr  = (int16_t)qmi_rd16(lte + 4);
            printf("  LTE Sig:  rssi=%d rsrq=%d rsrp=%d snr=%d\n", rssi, rsrq, rsrp, snr);
        }
        const uint8_t *wcdma = qmi_tlv_find(rsp, rlen, 0x13, &l);
        if (wcdma && l >= 3) {
            printf("  WCDMA:    rssi=%d ecio=%d\n", (int8_t)wcdma[0], (int16_t)qmi_rd16(wcdma + 1));
        }
        const uint8_t *gsm = qmi_tlv_find(rsp, rlen, 0x11, &l);
        if (gsm && l >= 1) {
            printf("  GSM Sig:  rssi=%d\n", (int8_t)gsm[0]);
        }
    }
}

int main(int argc, char **argv) {
    bool set_online = (argc > 1 && strcmp(argv[1], "--online") == 0);
    uint32_t local_node = 0;
    int fd = qmi_open_socket(&local_node);
    if (fd < 0) {
        perror("qmi_open_socket");
        return 1;
    }
    printf("QRTR local_node=%u\n", local_node);
    probe_dms(fd, local_node, set_online);
    probe_uim(fd, local_node);
    probe_nas(fd, local_node);
    close(fd);
    return 0;
}
