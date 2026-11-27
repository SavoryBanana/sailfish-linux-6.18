/*
 * sailfish_radio_hal - Android 17 AIDL Radio HAL (V1) over AF_QIPCRTR (QMI)
 * for Google Pixel 1 (sailfish / MSM8996).
 *
 * Registers VINTF-stability AIDL V1 services:
 *   - android.hardware.radio.config.IRadioConfig/default
 *   - android.hardware.radio.modem.IRadioModem/slot1
 *   - android.hardware.radio.sim.IRadioSim/slot1
 *   - android.hardware.radio.network.IRadioNetwork/slot1
 *   - android.hardware.radio.data.IRadioData/slot1
 *   - android.hardware.radio.voice.IRadioVoice/slot1
 *   - android.hardware.radio.messaging.IRadioMessaging/slot1
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "qmi_client.h"

/* ---- libbinder_ndk opaque types & function table ---- */
typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;

typedef void *(*AIBinder_Class_onCreate)(void *args);
typedef void (*AIBinder_Class_onDestroy)(void *userData);
typedef int32_t (*AIBinder_Class_onTransact)(AIBinder *binder, uint32_t code,
                                             const AParcel *in, AParcel *out);
typedef bool (*AParcel_stringAllocator)(void *stringData, int32_t length, char **buffer);

#define FLAG_ONEWAY_VAL       1u
#define STATUS_OK_VAL         0
#define STATUS_UNKNOWN_TX_VAL (-EBADMSG)

#define TX_GET_INTERFACE_HASH    16777214u
#define TX_GET_INTERFACE_VERSION 16777215u

#define RADIO_ERROR_NONE                  0
#define RADIO_ERROR_RADIO_NOT_AVAILABLE   1
#define RADIO_ERROR_GENERIC_FAILURE       2
#define RADIO_ERROR_REQUEST_NOT_SUPPORTED 6
#define RADIO_ERROR_NO_SUCH_ELEMENT       17

#define RADIO_RESP_SOLICITED  0
#define RADIO_IND_UNSOLICITED 0

#define RADIO_STATE_OFF         0
#define RADIO_STATE_UNAVAILABLE 1
#define RADIO_STATE_ON          10

#define INT32_MAX_VAL 0x7fffffff

static struct {
    AIBinder_Class *(*Class_define)(const char *, AIBinder_Class_onCreate,
                                    AIBinder_Class_onDestroy,
                                    AIBinder_Class_onTransact);
    AIBinder *(*AIBinder_new)(const AIBinder_Class *, void *);
    void (*markVintfStability)(AIBinder *);
    bool (*associateClass)(AIBinder *, const AIBinder_Class *);
    void (*incStrong)(AIBinder *);
    void (*decStrong)(AIBinder *);
    int32_t (*prepareTransaction)(AIBinder *, AParcel **);
    int32_t (*transact)(AIBinder *, uint32_t, AParcel **, AParcel **, uint32_t);
    int32_t (*addService)(AIBinder *, const char *);
    bool (*setThreadPoolMaxThreadCount)(uint32_t);
    void (*startThreadPool)(void);
    void (*joinThreadPool)(void);
    AStatus *(*AStatus_newOk)(void);
    void (*AStatus_delete)(AStatus *);
    void (*AParcel_delete)(AParcel *);
    int32_t (*getDataPosition)(const AParcel *);
    int32_t (*setDataPosition)(const AParcel *, int32_t);
    int32_t (*writeStatusHeader)(AParcel *, const AStatus *);
    int32_t (*writeInt32)(AParcel *, int32_t);
    int32_t (*writeInt64)(AParcel *, int64_t);
    int32_t (*writeBool)(AParcel *, bool);
    int32_t (*writeByte)(AParcel *, int8_t);
    int32_t (*writeString)(AParcel *, const char *, int32_t);
    int32_t (*writeByteArray)(AParcel *, const int8_t *, int32_t);
    int32_t (*writeInt32Array)(AParcel *, const int32_t *, int32_t);
    int32_t (*readInt32)(const AParcel *, int32_t *);
    int32_t (*readBool)(const AParcel *, bool *);
    int32_t (*readByte)(const AParcel *, int8_t *);
    int32_t (*readString)(const AParcel *, void *, AParcel_stringAllocator);
    int32_t (*readStrongBinder)(const AParcel *, AIBinder **);
} g_ndk;

static int g_kmsg_fd = -1;
static int (*g_alog_buf_write)(int bufID, int prio, const char *tag, const char *msg) = NULL;
static int (*g_alog_write)(int prio, const char *tag, const char *msg) = NULL;

static void load_liblog(void) {
    void *h = dlopen("liblog.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) return;
    *(void **)(&g_alog_buf_write) = dlsym(h, "__android_log_buf_write");
    *(void **)(&g_alog_write) = dlsym(h, "__android_log_write");
}

static void klog(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    buf[n] = '\0';
    if (g_alog_buf_write)
        (void)g_alog_buf_write(1 /* LOG_ID_RADIO */, 4 /* ANDROID_LOG_INFO */, "sailfish_radio_hal", buf);
    else if (g_alog_write)
        (void)g_alog_write(4 /* ANDROID_LOG_INFO */, "sailfish_radio_hal", buf);
    if (g_kmsg_fd >= 0) {
        char line[560];
        int ln = snprintf(line, sizeof(line), "sailfish_radio_hal: %.*s\n", n, buf);
        if (ln > 0) (void)write(g_kmsg_fd, line, (size_t)ln);
    }
}

static bool load_binder_ndk(void) {
    void *h = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        klog("dlopen(libbinder_ndk.so) failed: %s", dlerror());
        return false;
    }
#define LOAD(field, sym) do { \
    *(void **)(&g_ndk.field) = dlsym(h, sym); \
    if (!g_ndk.field) { klog("missing symbol %s", sym); return false; } \
} while (0)
    LOAD(Class_define,               "AIBinder_Class_define");
    LOAD(AIBinder_new,               "AIBinder_new");
    LOAD(markVintfStability,         "AIBinder_markVintfStability");
    LOAD(associateClass,             "AIBinder_associateClass");
    LOAD(incStrong,                  "AIBinder_incStrong");
    LOAD(decStrong,                  "AIBinder_decStrong");
    LOAD(prepareTransaction,         "AIBinder_prepareTransaction");
    LOAD(transact,                   "AIBinder_transact");
    LOAD(addService,                 "AServiceManager_addService");
    LOAD(setThreadPoolMaxThreadCount,"ABinderProcess_setThreadPoolMaxThreadCount");
    LOAD(startThreadPool,            "ABinderProcess_startThreadPool");
    LOAD(joinThreadPool,             "ABinderProcess_joinThreadPool");
    LOAD(AStatus_newOk,              "AStatus_newOk");
    LOAD(AStatus_delete,             "AStatus_delete");
    LOAD(AParcel_delete,             "AParcel_delete");
    LOAD(getDataPosition,            "AParcel_getDataPosition");
    LOAD(setDataPosition,            "AParcel_setDataPosition");
    LOAD(writeStatusHeader,          "AParcel_writeStatusHeader");
    LOAD(writeInt32,                 "AParcel_writeInt32");
    LOAD(writeInt64,                 "AParcel_writeInt64");
    LOAD(writeBool,                  "AParcel_writeBool");
    LOAD(writeByte,                  "AParcel_writeByte");
    LOAD(writeString,                "AParcel_writeString");
    LOAD(writeByteArray,             "AParcel_writeByteArray");
    LOAD(writeInt32Array,            "AParcel_writeInt32Array");
    LOAD(readInt32,                  "AParcel_readInt32");
    LOAD(readBool,                   "AParcel_readBool");
    LOAD(readByte,                   "AParcel_readByte");
    LOAD(readString,                 "AParcel_readString");
    LOAD(readStrongBinder,           "AParcel_readStrongBinder");
#undef LOAD
    return true;
}

/* ---- Parcel helpers for @VintfStability structured parcelables ---- */
static void wr_str(AParcel *p, const char *s) {
    if (!s) s = "";
    g_ndk.writeString(p, s, (int32_t)strlen(s));
}

static int32_t parcelable_begin(AParcel *p) {
    g_ndk.writeInt32(p, 1); /* non-null */
    int32_t pos = g_ndk.getDataPosition(p);
    g_ndk.writeInt32(p, 0); /* size placeholder */
    return pos;
}

static void parcelable_end(AParcel *p, int32_t start_pos) {
    int32_t end_pos = g_ndk.getDataPosition(p);
    g_ndk.setDataPosition(p, start_pos);
    g_ndk.writeInt32(p, end_pos - start_pos);
    g_ndk.setDataPosition(p, end_pos);
}

static void wr_resp_info(AParcel *p, int32_t serial, int32_t error) {
    int32_t s = parcelable_begin(p);
    g_ndk.writeInt32(p, RADIO_RESP_SOLICITED);
    g_ndk.writeInt32(p, serial);
    g_ndk.writeInt32(p, error);
    parcelable_end(p, s);
}

struct str_buf {
    char *data;
    int32_t len;
};

static bool str_alloc_cb(void *stringData, int32_t length, char **buffer) {
    struct str_buf *sb = (struct str_buf *)stringData;
    if (length < 0) {
        sb->data = NULL;
        sb->len = -1;
        *buffer = NULL;
        return true;
    }
    sb->data = (char *)calloc(1, (size_t)length + 1);
    if (!sb->data) return false;
    sb->len = length;
    *buffer = sb->data;
    return true;
}

static char *rd_str_dup(const AParcel *p) {
    struct str_buf sb = { NULL, 0 };
    if (g_ndk.readString(p, &sb, str_alloc_cb) != STATUS_OK_VAL || !sb.data)
        return strdup("");
    return sb.data;
}

static bool prep_tx(AIBinder *cb, AParcel **p, const char *tag) {
    int32_t rc = g_ndk.prepareTransaction(cb, p);
    if (rc != STATUS_OK_VAL || !*p) {
        klog("prepareTransaction(%s) failed rc=%d", tag, rc);
        return false;
    }
    return true;
}

static void send_oneway(AIBinder *cb, uint32_t code, AParcel *in) {
    if (!cb || !in) {
        if (in) g_ndk.AParcel_delete(in);
        return;
    }
    AParcel *out = NULL;
    int32_t rc = g_ndk.transact(cb, code, &in, &out, FLAG_ONEWAY_VAL);
    if (out) g_ndk.AParcel_delete(out);
    if (rc != STATUS_OK_VAL)
        klog("send_oneway(code=%u) failed rc=%d", code, rc);
}

static void send_simple_resp(AIBinder *cb, uint32_t code, int32_t serial,
                             int32_t err, const char *tag) {
    if (!cb) return;
    AParcel *in = NULL;
    if (!prep_tx(cb, &in, tag)) return;
    wr_resp_info(in, serial, err);
    send_oneway(cb, code, in);
}

static void send_i32_resp(AIBinder *cb, uint32_t code, int32_t serial,
                          int32_t err, int32_t val, const char *tag) {
    if (!cb) return;
    AParcel *in = NULL;
    if (!prep_tx(cb, &in, tag)) return;
    wr_resp_info(in, serial, err);
    g_ndk.writeInt32(in, val);
    send_oneway(cb, code, in);
}

static void send_bool_resp(AIBinder *cb, uint32_t code, int32_t serial,
                           int32_t err, bool val, const char *tag) {
    if (!cb) return;
    AParcel *in = NULL;
    if (!prep_tx(cb, &in, tag)) return;
    wr_resp_info(in, serial, err);
    g_ndk.writeBool(in, val);
    send_oneway(cb, code, in);
}

static void send_empty_array_resp(AIBinder *cb, uint32_t code, int32_t serial,
                                  int32_t err, const char *tag) {
    if (!cb) return;
    AParcel *in = NULL;
    if (!prep_tx(cb, &in, tag)) return;
    wr_resp_info(in, serial, err);
    g_ndk.writeInt32(in, 0);
    send_oneway(cb, code, in);
}

static void wr_icc_io_result(AParcel *p, int32_t sw1, int32_t sw2, const char *hex) {
    int32_t s = parcelable_begin(p);
    g_ndk.writeInt32(p, sw1);
    g_ndk.writeInt32(p, sw2);
    wr_str(p, hex);
    parcelable_end(p, s);
}

static int32_t reply_version_or_hash(const char *tag, uint32_t code, AParcel *out, const char *hash) {
    (void)tag;
    AStatus *st = g_ndk.AStatus_newOk();
    int32_t rc = g_ndk.writeStatusHeader(out, st);
    g_ndk.AStatus_delete(st);
    if (rc != STATUS_OK_VAL) return rc;
    if (code == TX_GET_INTERFACE_VERSION)
        return g_ndk.writeInt32(out, 1);
    return g_ndk.writeString(out, hash, (int32_t)strlen(hash));
}

/* ---- Shared QMI state (mutex-protected) ---- */
static pthread_mutex_t g_qmi_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_qmi_fd = -1;
static uint32_t g_local_node = 0;
static uint16_t g_txn = 1;
static struct qmi_service_addr g_svc_dms;
static struct qmi_service_addr g_svc_uim;
static struct qmi_service_addr g_svc_nas;
static struct qmi_service_addr g_svc_wda;
static struct qmi_service_addr g_svc_dpm;
static struct qmi_service_addr g_svc_voice;
static struct qmi_service_addr g_svc_wms;

/* ---- Voice & SMS state ---- */
#define MAX_VOICE_CALLS 8

struct voice_call_info {
    int32_t  index;      /* QMI call_id (1..7) */
    int32_t  state;      /* Android Call.STATE_* (0..5) */
    bool     is_mt;
    bool     is_mpty;
    int8_t   als;
    int32_t  num_pres;   /* Android PRESENTATION_* */
    char     number[64];
};

static pthread_mutex_t g_voice_mu = PTHREAD_MUTEX_INITIALIZER;
static struct voice_call_info g_calls[MAX_VOICE_CALLS];
static int32_t g_num_calls = 0;
static int32_t g_last_call_fail_cause = 16; /* NORMAL */
static bool g_voice_mute = false;

static pthread_mutex_t g_sms_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_smsc_addr[64] = "";
static uint32_t g_last_sms_txn_id = 0;
static bool g_last_sms_need_ack = false;

/* Cached modem/SIM identity */
static char g_baseband[64] = "8996-130361-1905270421";
static char g_imei[32]     = "";
static char g_meid[32]     = "";
static char g_iccid[32]    = "";
static char g_imsi[32]     = "";
static char g_aid_hex[64]  = "A0000000871002FF63F00189000001FF";
static bool g_sim_present  = false;
static bool g_sim_ready    = false;
static volatile bool g_qmi_ready    = false;
static volatile bool g_sim_notified = false;
static int32_t g_radio_state = RADIO_STATE_ON;
static volatile bool g_nas_force_notify = true;

/* Active packet data call & WDA configuration state */
struct data_call_state {
    bool     active;
    int32_t  cid;
    int32_t  pdp_type;
    char     apn[128];
    char     ifname[32];
    char     addrs[4][64];
    int32_t  n_addrs;
    char     dns[4][64];
    int32_t  n_dns;
    char     gws[4][64];
    int32_t  n_gws;
    int32_t  mtu_v4;
    int32_t  mtu_v6;
    int      wds_fd_v4;
    uint32_t handle_v4;
    bool     has_v4;
    int      wds_fd_v6;
    uint32_t handle_v6;
    bool     has_v6;
};

static pthread_mutex_t g_data_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_wda_configured = false;
static bool g_dpm_opened = false;
static struct data_call_state g_data_call = {
    .active    = false,
    .cid       = 1,
    .pdp_type  = 0,
    .wds_fd_v4 = -1,
    .wds_fd_v6 = -1,
};

static uint16_t qmi_next_txn(void) {
    uint16_t t = g_txn++;
    if (g_txn == 0) g_txn = 1;
    return t;
}

static bool qmi_ensure_svc_locked(uint32_t svc_id, struct qmi_service_addr *addr) {
    if (g_qmi_fd < 0) {
        g_qmi_fd = qmi_open_socket(&g_local_node);
        if (g_qmi_fd < 0) return false;
    }
    if (addr->found) return true;
    return qmi_lookup_service(g_qmi_fd, g_local_node, svc_id, addr, 1500) == 0;
}

static int qmi_call_locked(uint32_t svc_id, struct qmi_service_addr *addr,
                           uint16_t msg_id, const uint8_t *req, uint16_t req_len,
                           uint8_t *rsp, size_t rsp_cap, size_t *out_len) {
    if (!qmi_ensure_svc_locked(svc_id, addr))
        return -ENOENT;
    int rc = qmi_transact(g_qmi_fd, addr, qmi_next_txn(), msg_id,
                          req, req_len, rsp, rsp_cap, out_len, 2500);
    if (rc < 0) {
        addr->found = false;
        return rc;
    }
    uint16_t qerr = 0;
    return qmi_result_code(rsp, *out_len, &qerr);
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
            if (hi <= 9) out[pos++] = (char)('0' + hi);
        } else {
            if (lo <= 9) out[pos++] = (char)('0' + lo);
            if (hi <= 9) out[pos++] = (char)('0' + hi);
        }
    }
    out[pos] = '\0';
}

static bool qmi_uim_read_ef_locked(uint16_t msg_id, uint8_t session_type, uint16_t file_id,
                                   const uint8_t *path, uint8_t path_len,
                                   uint16_t offset_or_rec, uint16_t length,
                                   uint8_t *out, size_t out_cap, size_t *out_len,
                                   uint8_t *sw1, uint8_t *sw2, bool *got_card_sw) {
    uint8_t req[64];
    size_t off = 0;
    uint8_t sess[2] = { session_type, 0 };
    off = qmi_tlv_append(req, off, sizeof(req), 0x01, sess, sizeof(sess));

    uint8_t fid[16];
    qmi_wr16(fid, file_id);
    fid[2] = path_len;
    if (path_len) memcpy(fid + 3, path, path_len);
    off = qmi_tlv_append(req, off, sizeof(req), 0x02, fid, (uint16_t)(3 + path_len));

    uint8_t rd[4];
    qmi_wr16(rd, offset_or_rec);
    qmi_wr16(rd + 2, length);
    off = qmi_tlv_append(req, off, sizeof(req), 0x03, rd, sizeof(rd));

    uint8_t rsp[512];
    size_t rlen = 0;
    *out_len = 0;
    if (sw1) *sw1 = 0x6F;
    if (sw2) *sw2 = 0x00;
    if (got_card_sw) *got_card_sw = false;
    if (qmi_call_locked(QMI_SVC_UIM, &g_svc_uim, msg_id, req, (uint16_t)off,
                        rsp, sizeof(rsp), &rlen) != 0) {
        uint16_t l = 0;
        const uint8_t *cr = qmi_tlv_find(rsp, rlen, 0x10, &l);
        if (cr && l >= 2) {
            if (sw1) *sw1 = cr[0];
            if (sw2) *sw2 = cr[1];
            if (got_card_sw) *got_card_sw = true;
        }
        return false;
    }
    uint16_t l = 0;
    const uint8_t *cr = qmi_tlv_find(rsp, rlen, 0x10, &l);
    if (cr && l >= 2) {
        if (sw1) *sw1 = cr[0];
        if (sw2) *sw2 = cr[1];
        if (got_card_sw) *got_card_sw = true;
    } else {
        if (sw1) *sw1 = 0x90;
        if (sw2) *sw2 = 0x00;
    }
    const uint8_t *v = qmi_tlv_find(rsp, rlen, 0x11, &l);
    if (v && l >= 2) {
        uint16_t clen = qmi_rd16(v);
        if (clen > l - 2) clen = (uint16_t)(l - 2);
        if (clen > out_cap) clen = (uint16_t)out_cap;
        memcpy(out, v + 2, clen);
        *out_len = clen;
    }
    return true;
}

static bool qmi_uim_read_transparent_locked(uint8_t session_type, uint16_t file_id,
                                            const uint8_t *path, uint8_t path_len,
                                            uint16_t offset, uint16_t length,
                                            uint8_t *out, size_t out_cap, size_t *out_len,
                                            uint8_t *sw1, uint8_t *sw2, bool *got_card_sw) {
    return qmi_uim_read_ef_locked(0x0020, session_type, file_id, path, path_len,
                                  offset, length, out, out_cap, out_len, sw1, sw2, got_card_sw);
}

static bool qmi_uim_get_file_attrs_locked(uint8_t session_type, uint16_t file_id,
                                          const uint8_t *path, uint8_t path_len,
                                          uint8_t *out, size_t out_cap, size_t *out_len,
                                          uint8_t *sw1, uint8_t *sw2, bool *got_card_sw) {
    uint8_t req[64];
    size_t off = 0;
    uint8_t sess[2] = { session_type, 0 };
    off = qmi_tlv_append(req, off, sizeof(req), 0x01, sess, sizeof(sess));

    uint8_t fid[16];
    qmi_wr16(fid, file_id);
    fid[2] = path_len;
    if (path_len) memcpy(fid + 3, path, path_len);
    off = qmi_tlv_append(req, off, sizeof(req), 0x02, fid, (uint16_t)(3 + path_len));

    uint8_t rsp[512];
    size_t rlen = 0;
    *out_len = 0;
    if (sw1) *sw1 = 0x6F;
    if (sw2) *sw2 = 0x00;
    if (got_card_sw) *got_card_sw = false;
    if (qmi_call_locked(QMI_SVC_UIM, &g_svc_uim, 0x0024, req, (uint16_t)off,
                        rsp, sizeof(rsp), &rlen) != 0) {
        uint16_t l = 0;
        const uint8_t *cr = qmi_tlv_find(rsp, rlen, 0x10, &l);
        if (cr && l >= 2) {
            if (sw1) *sw1 = cr[0];
            if (sw2) *sw2 = cr[1];
            if (got_card_sw) *got_card_sw = true;
        }
        return false;
    }

    uint16_t l = 0;
    const uint8_t *v = qmi_tlv_find(rsp, rlen, 0x11, &l);
    if (!v || l < 9 || out_cap < 15)
        return false;

    uint16_t file_size = qmi_rd16(v);
    uint16_t fid_val   = qmi_rd16(v + 2);
    uint8_t  file_type = v[4];
    uint16_t rec_size  = qmi_rd16(v + 5);
    /* uint16_t rec_count = qmi_rd16(v + 7); */

    uint8_t ef_struct = 0;
    uint8_t rec_len = 0;
    if (file_type == 1) {
        ef_struct = 3; /* cyclic */
        rec_len = (uint8_t)(rec_size & 0xff);
    } else if (file_type == 2) {
        ef_struct = 1; /* linear fixed */
        rec_len = (uint8_t)(rec_size & 0xff);
    }

    memset(out, 0, 15);
    out[2]  = (uint8_t)((file_size >> 8) & 0xff);
    out[3]  = (uint8_t)(file_size & 0xff);
    out[4]  = (uint8_t)((fid_val >> 8) & 0xff);
    out[5]  = (uint8_t)(fid_val & 0xff);
    out[6]  = 0x04;
    out[12] = 0x01;
    out[13] = ef_struct;
    out[14] = rec_len;
    *out_len = 15;
    if (sw1) *sw1 = 0x90;
    if (sw2) *sw2 = 0x00;
    return true;
}

static void qmi_refresh_identity_locked(void) {
    uint8_t rsp[1024];
    size_t rlen = 0;

    if (qmi_call_locked(QMI_SVC_DMS, &g_svc_dms, 0x0023, NULL, 0, rsp, sizeof(rsp), &rlen) == 0) {
        uint16_t l = 0;
        const uint8_t *v = qmi_tlv_find(rsp, rlen, 0x01, &l);
        if (v && l > 0) {
            size_t c = l < sizeof(g_baseband) - 1 ? l : sizeof(g_baseband) - 1;
            memcpy(g_baseband, v, c);
            g_baseband[c] = '\0';
        }
    }
    if (qmi_call_locked(QMI_SVC_DMS, &g_svc_dms, 0x0025, NULL, 0, rsp, sizeof(rsp), &rlen) == 0) {
        uint16_t l = 0;
        const uint8_t *v = qmi_tlv_find(rsp, rlen, 0x11, &l);
        if (v && l > 0) {
            size_t c = l < sizeof(g_imei) - 1 ? l : sizeof(g_imei) - 1;
            memcpy(g_imei, v, c);
            g_imei[c] = '\0';
        }
        v = qmi_tlv_find(rsp, rlen, 0x12, &l);
        if (v && l > 0) {
            size_t c = l < sizeof(g_meid) - 1 ? l : sizeof(g_meid) - 1;
            memcpy(g_meid, v, c);
            g_meid[c] = '\0';
        }
    }
    if (qmi_call_locked(QMI_SVC_DMS, &g_svc_dms, 0x002D, NULL, 0, rsp, sizeof(rsp), &rlen) == 0) {
        uint16_t l = 0;
        const uint8_t *m = qmi_tlv_find(rsp, rlen, 0x01, &l);
        if (m && l >= 1)
            g_radio_state = (m[0] == 0) ? RADIO_STATE_ON : RADIO_STATE_OFF;
    }

    if (qmi_call_locked(QMI_SVC_UIM, &g_svc_uim, 0x002F, NULL, 0, rsp, sizeof(rsp), &rlen) == 0) {
        uint16_t l = 0;
        const uint8_t *v = qmi_tlv_find(rsp, rlen, 0x10, &l);
        if (v && l >= 15 && v[8] >= 1) {
            g_sim_present = (v[9] == 1);
            uint8_t napps = v[14];
            if (napps >= 1 && l >= 22) {
                uint8_t astate = v[16];
                uint8_t aidlen = v[21];
                g_sim_ready = (astate == 7);
                if (aidlen > 0 && 22u + aidlen <= l && aidlen * 2 < sizeof(g_aid_hex)) {
                    for (uint8_t i = 0; i < aidlen; i++)
                        snprintf(g_aid_hex + i * 2, 3, "%02X", v[22 + i]);
                }
            }
        }
    }

    if (g_sim_present) {
        uint8_t buf[64];
        size_t blen = 0;
        const uint8_t path_mf[2] = { 0x00, 0x3F };
        if (qmi_uim_read_transparent_locked(6, 0x2FE2, path_mf, 2, 0, 0,
                                            buf, sizeof(buf), &blen, NULL, NULL, NULL) && blen > 0) {
            decode_bcd_iccid(buf, blen, g_iccid, sizeof(g_iccid));
        }
        const uint8_t path_usim[4] = { 0x00, 0x3F, 0xFF, 0x7F };
        if (qmi_uim_read_transparent_locked(0, 0x6F07, path_usim, 4, 0, 0,
                                            buf, sizeof(buf), &blen, NULL, NULL, NULL) && blen > 0) {
            decode_imsi(buf, blen, g_imsi, sizeof(g_imsi));
        }
    }
}

/* ---- Callback holders & AIBinder classes ---- */
static pthread_mutex_t g_cb_mu = PTHREAD_MUTEX_INITIALIZER;
static AIBinder *g_cfg_rsp = NULL, *g_cfg_ind = NULL;
static AIBinder *g_mdm_rsp = NULL, *g_mdm_ind = NULL;
static AIBinder *g_sim_rsp = NULL, *g_sim_ind = NULL;
static AIBinder *g_net_rsp = NULL, *g_net_ind = NULL;
static AIBinder *g_dat_rsp = NULL, *g_dat_ind = NULL;
static AIBinder *g_voc_rsp = NULL, *g_voc_ind = NULL;
static AIBinder *g_msg_rsp = NULL, *g_msg_ind = NULL;

static AIBinder_Class *g_cfg_rsp_cls = NULL, *g_cfg_ind_cls = NULL;
static AIBinder_Class *g_mdm_rsp_cls = NULL, *g_mdm_ind_cls = NULL;
static AIBinder_Class *g_sim_rsp_cls = NULL, *g_sim_ind_cls = NULL;
static AIBinder_Class *g_net_rsp_cls = NULL, *g_net_ind_cls = NULL;
static AIBinder_Class *g_dat_rsp_cls = NULL, *g_dat_ind_cls = NULL;
static AIBinder_Class *g_voc_rsp_cls = NULL, *g_voc_ind_cls = NULL;
static AIBinder_Class *g_msg_rsp_cls = NULL, *g_msg_ind_cls = NULL;

static void replace_cb_binder(AIBinder **slot, AIBinder *next, const AIBinder_Class *cls) {
    if (next && cls && !g_ndk.associateClass(next, cls)) {
        klog("associateClass failed for callback binder");
    }
    pthread_mutex_lock(&g_cb_mu);
    if (*slot) g_ndk.decStrong(*slot);
    *slot = next;
    pthread_mutex_unlock(&g_cb_mu);
}

static AIBinder *get_binder(AIBinder **slot) {
    pthread_mutex_lock(&g_cb_mu);
    AIBinder *b = *slot;
    if (b) g_ndk.incStrong(b);
    pthread_mutex_unlock(&g_cb_mu);
    return b;
}

static void *noop_create(void *args) { return args; }
static void noop_destroy(void *ud) { (void)ud; }
static int32_t noop_transact(AIBinder *b, uint32_t code, const AParcel *in, AParcel *out) {
    (void)b; (void)code; (void)in; (void)out;
    return STATUS_UNKNOWN_TX_VAL;
}

static void send_unsol_empty_ind(AIBinder **slot, uint32_t code, const char *tag) {
    AIBinder *ind = get_binder(slot);
    if (!ind) return;
    AParcel *p = NULL;
    if (prep_tx(ind, &p, tag)) {
        g_ndk.writeInt32(p, RADIO_IND_UNSOLICITED);
        send_oneway(ind, code, p);
    }
    g_ndk.decStrong(ind);
}

/* =========================================================================
 * 1. android.hardware.radio.config.IRadioConfig (V1)
 * ========================================================================= */
static const char ICFG_DESC[] = "android.hardware.radio.config.IRadioConfig";
static const char ICFG_INST[] = "android.hardware.radio.config.IRadioConfig/default";
static const char ICFG_HASH[] = "3395232b44456054090524c5875842900442d1a0";

enum {
    CFG_TX_GET_HAL_CAPS = 1,
    CFG_TX_GET_NUM_LIVE_MODEMS = 2,
    CFG_TX_GET_PHONE_CAPABILITY = 3,
    CFG_TX_GET_SIM_SLOTS_STATUS = 4,
    CFG_TX_SET_NUM_LIVE_MODEMS = 5,
    CFG_TX_SET_PREF_DATA_MODEM = 6,
    CFG_TX_SET_RESPONSE_FUNCTIONS = 7,
    CFG_TX_SET_SIM_SLOTS_MAPPING = 8,
};

enum {
    CFG_RSP_GET_HAL_CAPS = 1,
    CFG_RSP_GET_NUM_LIVE_MODEMS = 2,
    CFG_RSP_GET_PHONE_CAPABILITY = 3,
    CFG_RSP_GET_SIM_SLOTS_STATUS = 4,
    CFG_RSP_SET_NUM_LIVE_MODEMS = 5,
    CFG_RSP_SET_PREF_DATA_MODEM = 6,
    CFG_RSP_SET_SIM_SLOTS_MAPPING = 7,
};

static int32_t cfg_on_transact(AIBinder *b, uint32_t code, const AParcel *in, AParcel *out) {
    (void)b;
    if (code == TX_GET_INTERFACE_VERSION || code == TX_GET_INTERFACE_HASH)
        return reply_version_or_hash("IRadioConfig", code, out, ICFG_HASH);

    if (code == CFG_TX_SET_RESPONSE_FUNCTIONS) {
        AIBinder *rsp = NULL, *ind = NULL;
        g_ndk.readStrongBinder(in, &rsp);
        g_ndk.readStrongBinder(in, &ind);
        replace_cb_binder(&g_cfg_rsp, rsp, g_cfg_rsp_cls);
        replace_cb_binder(&g_cfg_ind, ind, g_cfg_ind_cls);
        klog("IRadioConfig.setResponseFunctions");
        return STATUS_OK_VAL;
    }

    int32_t serial = 0;
    g_ndk.readInt32(in, &serial);
    AIBinder *cb = get_binder(&g_cfg_rsp);
    if (!cb) return STATUS_OK_VAL;

    switch (code) {
    case CFG_TX_GET_HAL_CAPS: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getHalDeviceCapabilitiesResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeBool(p, false); /* modemReducedFeatureSet1 */
            send_oneway(cb, CFG_RSP_GET_HAL_CAPS, p);
        }
        break;
    }
    case CFG_TX_GET_NUM_LIVE_MODEMS: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getNumOfLiveModemsResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeByte(p, 1);
            send_oneway(cb, CFG_RSP_GET_NUM_LIVE_MODEMS, p);
        }
        break;
    }
    case CFG_TX_GET_PHONE_CAPABILITY: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getPhoneCapabilityResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            int32_t s = parcelable_begin(p);
            g_ndk.writeByte(p, 1);     /* maxActiveData */
            g_ndk.writeByte(p, 1);     /* maxActiveInternetData */
            g_ndk.writeBool(p, false); /* isInternetLingeringSupported */
            int8_t mids[1] = { 0 };
            g_ndk.writeByteArray(p, mids, 1);
            parcelable_end(p, s);
            send_oneway(cb, CFG_RSP_GET_PHONE_CAPABILITY, p);
        }
        break;
    }
    case CFG_TX_GET_SIM_SLOTS_STATUS: {
        pthread_mutex_lock(&g_qmi_mu);
        if (!g_sim_present) qmi_refresh_identity_locked();
        bool present = g_sim_present;
        char iccid[32];
        snprintf(iccid, sizeof(iccid), "%s", g_iccid);
        pthread_mutex_unlock(&g_qmi_mu);

        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getSimSlotsStatusResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeInt32(p, 1); /* SimSlotStatus[1] */
            int32_t s = parcelable_begin(p);
            g_ndk.writeInt32(p, present ? 1 : 0); /* cardState */
            wr_str(p, "");                        /* atr */
            wr_str(p, "");                        /* eid */
            g_ndk.writeInt32(p, 1);               /* SimPortInfo[1] */
            int32_t sp = parcelable_begin(p);
            wr_str(p, iccid);
            g_ndk.writeInt32(p, 0);               /* logicalSlotId */
            g_ndk.writeBool(p, true);             /* portActive */
            parcelable_end(p, sp);
            parcelable_end(p, s);
            send_oneway(cb, CFG_RSP_GET_SIM_SLOTS_STATUS, p);
        }
        break;
    }
    case CFG_TX_SET_NUM_LIVE_MODEMS:
        send_simple_resp(cb, CFG_RSP_SET_NUM_LIVE_MODEMS, serial, RADIO_ERROR_NONE, "setNumOfLiveModemsResp");
        break;
    case CFG_TX_SET_PREF_DATA_MODEM:
        send_simple_resp(cb, CFG_RSP_SET_PREF_DATA_MODEM, serial, RADIO_ERROR_NONE, "setPreferredDataModemResp");
        break;
    case CFG_TX_SET_SIM_SLOTS_MAPPING:
        send_simple_resp(cb, CFG_RSP_SET_SIM_SLOTS_MAPPING, serial, RADIO_ERROR_NONE, "setSimSlotsMappingResp");
        break;
    default:
        klog("IRadioConfig unknown tx=%u", code);
        break;
    }
    g_ndk.decStrong(cb);
    return STATUS_OK_VAL;
}

/* =========================================================================
 * 2. android.hardware.radio.modem.IRadioModem (V1)
 * ========================================================================= */
static const char IMDM_DESC[] = "android.hardware.radio.modem.IRadioModem";
static const char IMDM_INST[] = "android.hardware.radio.modem.IRadioModem/slot1";
static const char IMDM_HASH[] = "9dee2319b599d654955c05268c1eed6ca4373b58";

enum {
    MDM_TX_ENABLE_MODEM = 1,
    MDM_TX_GET_BASEBAND = 2,
    MDM_TX_GET_DEVICE_ID = 3,
    MDM_TX_GET_HW_CONFIG = 4,
    MDM_TX_GET_ACTIVITY = 5,
    MDM_TX_GET_STACK_STATUS = 6,
    MDM_TX_GET_RADIO_CAP = 7,
    MDM_TX_NV_READ = 8,
    MDM_TX_NV_RESET = 9,
    MDM_TX_NV_WRITE_PRL = 10,
    MDM_TX_NV_WRITE_ITEM = 11,
    MDM_TX_REQUEST_SHUTDOWN = 12,
    MDM_TX_RESP_ACK = 13,
    MDM_TX_SEND_DEV_STATE = 14,
    MDM_TX_SET_RADIO_CAP = 15,
    MDM_TX_SET_RADIO_POWER = 16,
    MDM_TX_SET_RESPONSE_FUNCTIONS = 17,
};

enum {
    MDM_RSP_ACK = 1,
    MDM_RSP_ENABLE_MODEM = 2,
    MDM_RSP_GET_BASEBAND = 3,
    MDM_RSP_GET_DEVICE_ID = 4,
    MDM_RSP_GET_HW_CONFIG = 5,
    MDM_RSP_GET_ACTIVITY = 6,
    MDM_RSP_GET_STACK_STATUS = 7,
    MDM_RSP_GET_RADIO_CAP = 8,
    MDM_RSP_NV_READ = 9,
    MDM_RSP_NV_RESET = 10,
    MDM_RSP_NV_WRITE_PRL = 11,
    MDM_RSP_NV_WRITE_ITEM = 12,
    MDM_RSP_REQUEST_SHUTDOWN = 13,
    MDM_RSP_SEND_DEV_STATE = 14,
    MDM_RSP_SET_RADIO_CAP = 15,
    MDM_RSP_SET_RADIO_POWER = 16,
};

enum {
    MDM_IND_HW_CONFIG = 1,
    MDM_IND_MODEM_RESET = 2,
    MDM_IND_RADIO_CAP = 3,
    MDM_IND_RADIO_STATE = 4,
    MDM_IND_RIL_CONNECTED = 5,
};

static void wr_radio_capability(AParcel *p) {
    int32_t s = parcelable_begin(p);
    g_ndk.writeInt32(p, 0);                  /* session */
    g_ndk.writeInt32(p, 4);                  /* PHASE_FINISH */
    g_ndk.writeInt32(p, 0x0001401f);         /* GSM|GPRS|EDGE|UMTS|LTE */
    wr_str(p, "0");                          /* logicalModemUuid */
    g_ndk.writeInt32(p, 1);                  /* STATUS_SUCCESS */
    parcelable_end(p, s);
}

static void send_radio_state_ind(int32_t state) {
    AIBinder *ind = get_binder(&g_mdm_ind);
    if (!ind) return;
    AParcel *p = NULL;
    if (prep_tx(ind, &p, "radioStateChanged")) {
        g_ndk.writeInt32(p, RADIO_IND_UNSOLICITED);
        g_ndk.writeInt32(p, state);
        send_oneway(ind, MDM_IND_RADIO_STATE, p);
    }
    g_ndk.decStrong(ind);
}

static int32_t mdm_on_transact(AIBinder *b, uint32_t code, const AParcel *in, AParcel *out) {
    (void)b;
    if (code == TX_GET_INTERFACE_VERSION || code == TX_GET_INTERFACE_HASH)
        return reply_version_or_hash("IRadioModem", code, out, IMDM_HASH);

    if (code == MDM_TX_RESP_ACK)
        return STATUS_OK_VAL;

    if (code == MDM_TX_SET_RESPONSE_FUNCTIONS) {
        AIBinder *rsp = NULL, *ind = NULL;
        g_ndk.readStrongBinder(in, &rsp);
        g_ndk.readStrongBinder(in, &ind);
        replace_cb_binder(&g_mdm_rsp, rsp, g_mdm_rsp_cls);
        replace_cb_binder(&g_mdm_ind, ind, g_mdm_ind_cls);
        klog("IRadioModem.setResponseFunctions");

        send_unsol_empty_ind(&g_mdm_ind, MDM_IND_RIL_CONNECTED, "rilConnected");
        send_radio_state_ind(g_radio_state);
        return STATUS_OK_VAL;
    }

    int32_t serial = 0;
    g_ndk.readInt32(in, &serial);
    AIBinder *cb = get_binder(&g_mdm_rsp);
    if (!cb) return STATUS_OK_VAL;

    switch (code) {
    case MDM_TX_ENABLE_MODEM:
        send_simple_resp(cb, MDM_RSP_ENABLE_MODEM, serial, RADIO_ERROR_NONE, "enableModemResp");
        break;
    case MDM_TX_GET_BASEBAND: {
        pthread_mutex_lock(&g_qmi_mu);
        qmi_refresh_identity_locked();
        char bb[64];
        snprintf(bb, sizeof(bb), "%s", g_baseband);
        pthread_mutex_unlock(&g_qmi_mu);

        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getBasebandVersionResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            wr_str(p, bb);
            send_oneway(cb, MDM_RSP_GET_BASEBAND, p);
        }
        break;
    }
    case MDM_TX_GET_DEVICE_ID: {
        pthread_mutex_lock(&g_qmi_mu);
        if (!g_imei[0]) qmi_refresh_identity_locked();
        char imei[32], meid[32];
        snprintf(imei, sizeof(imei), "%s", g_imei);
        snprintf(meid, sizeof(meid), "%s", g_meid);
        pthread_mutex_unlock(&g_qmi_mu);

        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getDeviceIdentityResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            wr_str(p, imei);
            wr_str(p, "01");
            wr_str(p, "");
            wr_str(p, meid);
            send_oneway(cb, MDM_RSP_GET_DEVICE_ID, p);
        }
        break;
    }
    case MDM_TX_GET_HW_CONFIG: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getHardwareConfigResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            g_ndk.writeInt32(p, 0);
            send_oneway(cb, MDM_RSP_GET_HW_CONFIG, p);
        }
        break;
    }
    case MDM_TX_GET_ACTIVITY: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getModemActivityInfoResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            int32_t s = parcelable_begin(p);
            g_ndk.writeInt32(p, 0);
            g_ndk.writeInt32(p, 0);
            g_ndk.writeInt32(p, 0); /* ActivityStatsTechSpecificInfo[0] */
            parcelable_end(p, s);
            send_oneway(cb, MDM_RSP_GET_ACTIVITY, p);
        }
        break;
    }
    case MDM_TX_GET_STACK_STATUS: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getModemStackStatusResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeBool(p, true);
            send_oneway(cb, MDM_RSP_GET_STACK_STATUS, p);
        }
        break;
    }
    case MDM_TX_GET_RADIO_CAP: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getRadioCapabilityResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            wr_radio_capability(p);
            send_oneway(cb, MDM_RSP_GET_RADIO_CAP, p);
        }
        break;
    }
    case MDM_TX_SET_RADIO_CAP: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "setRadioCapabilityResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            wr_radio_capability(p);
            send_oneway(cb, MDM_RSP_SET_RADIO_CAP, p);
        }
        break;
    }
    case MDM_TX_SET_RADIO_POWER: {
        bool power_on = true;
        g_ndk.readBool(in, &power_on);
        uint8_t mode = power_on ? 0 : 1; /* 0=ONLINE, 1=LOW_POWER */
        uint8_t req[8], rsp[64];
        size_t req_len = qmi_tlv_append(req, 0, sizeof(req), 0x01, &mode, 1);
        size_t rlen = 0;
        pthread_mutex_lock(&g_qmi_mu);
        qmi_call_locked(QMI_SVC_DMS, &g_svc_dms, 0x002E, req, (uint16_t)req_len,
                        rsp, sizeof(rsp), &rlen);
        g_radio_state = power_on ? RADIO_STATE_ON : RADIO_STATE_OFF;
        if (power_on) g_nas_force_notify = true;
        pthread_mutex_unlock(&g_qmi_mu);

        klog("IRadioModem.setRadioPower(on=%d)", (int)power_on);
        send_simple_resp(cb, MDM_RSP_SET_RADIO_POWER, serial, RADIO_ERROR_NONE, "setRadioPowerResp");
        send_radio_state_ind(g_radio_state);
        break;
    }
    case MDM_TX_SEND_DEV_STATE:
        send_simple_resp(cb, MDM_RSP_SEND_DEV_STATE, serial, RADIO_ERROR_NONE, "sendDeviceStateResp");
        break;
    case MDM_TX_REQUEST_SHUTDOWN:
        send_simple_resp(cb, MDM_RSP_REQUEST_SHUTDOWN, serial, RADIO_ERROR_NONE, "requestShutdownResp");
        break;
    case MDM_TX_NV_READ: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "nvReadItemResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            wr_str(p, "");
            send_oneway(cb, MDM_RSP_NV_READ, p);
        }
        break;
    }
    case MDM_TX_NV_RESET:
        send_simple_resp(cb, MDM_RSP_NV_RESET, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "nvResetConfigResp");
        break;
    case MDM_TX_NV_WRITE_PRL:
        send_simple_resp(cb, MDM_RSP_NV_WRITE_PRL, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "nvWriteCdmaPrlResp");
        break;
    case MDM_TX_NV_WRITE_ITEM:
        send_simple_resp(cb, MDM_RSP_NV_WRITE_ITEM, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "nvWriteItemResp");
        break;
    default:
        klog("IRadioModem unknown tx=%u serial=%d", code, serial);
        break;
    }
    g_ndk.decStrong(cb);
    return STATUS_OK_VAL;
}

/* =========================================================================
 * 3. android.hardware.radio.sim.IRadioSim (V1)
 * ========================================================================= */
static const char ISIM_DESC[] = "android.hardware.radio.sim.IRadioSim";
static const char ISIM_INST[] = "android.hardware.radio.sim.IRadioSim/slot1";
static const char ISIM_HASH[] = "01cea196fdf8f5e41fd8dc415308a4c4b5a77607";

enum {
    SIM_TX_ARE_UICC_APPS_ENABLED = 1,
    SIM_TX_CHANGE_ICC_PIN2 = 2,
    SIM_TX_CHANGE_ICC_PIN = 3,
    SIM_TX_ENABLE_UICC_APPS = 4,
    SIM_TX_GET_ALLOWED_CARRIERS = 5,
    SIM_TX_GET_CDMA_SUBSCRIPTION = 6,
    SIM_TX_GET_CDMA_SUB_SOURCE = 7,
    SIM_TX_GET_FACILITY_LOCK = 8,
    SIM_TX_GET_ICC_CARD_STATUS = 9,
    SIM_TX_GET_IMSI_FOR_APP = 10,
    SIM_TX_GET_PB_CAPACITY = 11,
    SIM_TX_GET_PB_RECORDS = 12,
    SIM_TX_ICC_CLOSE_LOGICAL_CH = 13,
    SIM_TX_ICC_IO_FOR_APP = 14,
    SIM_TX_ICC_OPEN_LOGICAL_CH = 15,
    SIM_TX_ICC_TX_APDU_BASIC = 16,
    SIM_TX_ICC_TX_APDU_LOGICAL = 17,
    SIM_TX_REPORT_STK_RUNNING = 18,
    SIM_TX_REQ_ICC_SIM_AUTH = 19,
    SIM_TX_RESP_ACK = 20,
    SIM_TX_SEND_ENVELOPE = 21,
    SIM_TX_SEND_ENVELOPE_STATUS = 22,
    SIM_TX_SEND_TERMINAL_RESP = 23,
    SIM_TX_SET_ALLOWED_CARRIERS = 24,
    SIM_TX_SET_CARRIER_IMSI_ENC = 25,
    SIM_TX_SET_CDMA_SUB_SOURCE = 26,
    SIM_TX_SET_FACILITY_LOCK = 27,
    SIM_TX_SET_RESPONSE_FUNCTIONS = 28,
    SIM_TX_SET_SIM_CARD_POWER = 29,
    SIM_TX_SET_UICC_SUBSCRIPTION = 30,
    SIM_TX_SUPPLY_ICC_PIN2 = 31,
    SIM_TX_SUPPLY_ICC_PIN = 32,
    SIM_TX_SUPPLY_ICC_PUK2 = 33,
    SIM_TX_SUPPLY_ICC_PUK = 34,
    SIM_TX_SUPPLY_SIM_DEPERSO = 35,
    SIM_TX_UPDATE_PB_RECORDS = 36,
};

enum {
    SIM_RSP_ACK = 1,
    SIM_RSP_ARE_UICC_APPS_ENABLED = 2,
    SIM_RSP_CHANGE_ICC_PIN2 = 3,
    SIM_RSP_CHANGE_ICC_PIN = 4,
    SIM_RSP_ENABLE_UICC_APPS = 5,
    SIM_RSP_GET_ALLOWED_CARRIERS = 6,
    SIM_RSP_GET_CDMA_SUBSCRIPTION = 7,
    SIM_RSP_GET_CDMA_SUB_SOURCE = 8,
    SIM_RSP_GET_FACILITY_LOCK = 9,
    SIM_RSP_GET_ICC_CARD_STATUS = 10,
    SIM_RSP_GET_IMSI_FOR_APP = 11,
    SIM_RSP_GET_PB_CAPACITY = 12,
    SIM_RSP_GET_PB_RECORDS = 13,
    SIM_RSP_ICC_CLOSE_LOGICAL_CH = 14,
    SIM_RSP_ICC_IO_FOR_APP = 15,
    SIM_RSP_ICC_OPEN_LOGICAL_CH = 16,
    SIM_RSP_ICC_TX_APDU_BASIC = 17,
    SIM_RSP_ICC_TX_APDU_LOGICAL = 18,
    SIM_RSP_REPORT_STK_RUNNING = 19,
    SIM_RSP_REQ_ICC_SIM_AUTH = 20,
    SIM_RSP_SEND_ENVELOPE = 21,
    SIM_RSP_SEND_ENVELOPE_STATUS = 22,
    SIM_RSP_SEND_TERMINAL_RESP = 23,
    SIM_RSP_SET_ALLOWED_CARRIERS = 24,
    SIM_RSP_SET_CARRIER_IMSI_ENC = 25,
    SIM_RSP_SET_CDMA_SUB_SOURCE = 26,
    SIM_RSP_SET_FACILITY_LOCK = 27,
    SIM_RSP_SET_SIM_CARD_POWER = 28,
    SIM_RSP_SET_UICC_SUBSCRIPTION = 29,
    SIM_RSP_SUPPLY_ICC_PIN2 = 30,
    SIM_RSP_SUPPLY_ICC_PIN = 31,
    SIM_RSP_SUPPLY_ICC_PUK2 = 32,
    SIM_RSP_SUPPLY_ICC_PUK = 33,
    SIM_RSP_SUPPLY_SIM_DEPERSO = 34,
    SIM_RSP_UPDATE_PB_RECORDS = 35,
};

enum {
    SIM_IND_SIM_STATUS_CHANGED = 6,
};

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t parse_ef_path(const char *path_hex, uint8_t *out, size_t cap) {
    size_t slen = strlen(path_hex);
    size_t n = 0;
    /* QMI UIM path is pairs of 16-bit LE file IDs, whereas Android passes BE hex e.g. "3F007FFF" */
    for (size_t i = 0; i + 3 < slen && n + 2 <= cap; i += 4) {
        int b0 = (hex_nibble(path_hex[i]) << 4) | hex_nibble(path_hex[i + 1]);
        int b1 = (hex_nibble(path_hex[i + 2]) << 4) | hex_nibble(path_hex[i + 3]);
        if (b0 < 0 || b1 < 0) break;
        out[n++] = (uint8_t)b1;
        out[n++] = (uint8_t)b0;
    }
    return n;
}

static int32_t sim_on_transact(AIBinder *b, uint32_t code, const AParcel *in, AParcel *out) {
    (void)b;
    if (code == TX_GET_INTERFACE_VERSION || code == TX_GET_INTERFACE_HASH)
        return reply_version_or_hash("IRadioSim", code, out, ISIM_HASH);

    if (code == SIM_TX_RESP_ACK)
        return STATUS_OK_VAL;

    if (code == SIM_TX_SET_RESPONSE_FUNCTIONS) {
        AIBinder *rsp = NULL, *ind = NULL;
        g_ndk.readStrongBinder(in, &rsp);
        g_ndk.readStrongBinder(in, &ind);
        replace_cb_binder(&g_sim_rsp, rsp, g_sim_rsp_cls);
        replace_cb_binder(&g_sim_ind, ind, g_sim_ind_cls);
        klog("IRadioSim.setResponseFunctions");
        if (g_qmi_ready || g_sim_present) {
            g_sim_notified = true;
            send_unsol_empty_ind(&g_sim_ind, SIM_IND_SIM_STATUS_CHANGED, "simStatusChanged");
        }
        return STATUS_OK_VAL;
    }

    int32_t serial = 0;
    g_ndk.readInt32(in, &serial);
    AIBinder *cb = get_binder(&g_sim_rsp);
    if (!cb) return STATUS_OK_VAL;

    switch (code) {
    case SIM_TX_ARE_UICC_APPS_ENABLED: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "areUiccApplicationsEnabledResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeBool(p, true);
            send_oneway(cb, SIM_RSP_ARE_UICC_APPS_ENABLED, p);
        }
        break;
    }
    case SIM_TX_GET_ICC_CARD_STATUS: {
        pthread_mutex_lock(&g_qmi_mu);
        qmi_refresh_identity_locked();
        bool present = g_sim_present;
        bool ready   = g_sim_ready;
        char iccid[32], aid[64];
        snprintf(iccid, sizeof(iccid), "%s", g_iccid);
        snprintf(aid, sizeof(aid), "%s", g_aid_hex);
        pthread_mutex_unlock(&g_qmi_mu);

        klog("IRadioSim.getIccCardStatus present=%d ready=%d iccid=%s",
             (int)present, (int)ready, iccid);

        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getIccCardStatusResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            int32_t cs = parcelable_begin(p);
            g_ndk.writeInt32(p, present ? 1 : 0);      /* cardState */
            g_ndk.writeInt32(p, 3);                    /* universalPinState=DISABLED */
            g_ndk.writeInt32(p, present ? 0 : -1);     /* gsmUmtsSubscriptionAppIndex */
            g_ndk.writeInt32(p, -1);                   /* cdmaSubscriptionAppIndex */
            g_ndk.writeInt32(p, -1);                   /* imsSubscriptionAppIndex */
            if (present) {
                g_ndk.writeInt32(p, 1);                /* AppStatus[1] */
                int32_t as = parcelable_begin(p);
                g_ndk.writeInt32(p, 2);                /* APPTYPE_USIM */
                g_ndk.writeInt32(p, ready ? 5 : 1);    /* APPSTATE_READY=5 */
                g_ndk.writeInt32(p, 0);                /* persoSubstate */
                wr_str(p, aid);
                wr_str(p, "USIM");
                g_ndk.writeBool(p, false);
                g_ndk.writeInt32(p, 3);                /* pin1=DISABLED */
                g_ndk.writeInt32(p, 3);                /* pin2=DISABLED */
                parcelable_end(p, as);
            } else {
                g_ndk.writeInt32(p, 0);
            }
            wr_str(p, "");                             /* atr */
            wr_str(p, iccid);                          /* iccid */
            wr_str(p, "");                             /* eid */
            int32_t sm = parcelable_begin(p);          /* SlotPortMapping */
            g_ndk.writeInt32(p, 0);                    /* physicalSlotId */
            g_ndk.writeInt32(p, 0);                    /* portId */
            parcelable_end(p, sm);
            parcelable_end(p, cs);
            send_oneway(cb, SIM_RSP_GET_ICC_CARD_STATUS, p);
        }
        break;
    }
    case SIM_TX_GET_IMSI_FOR_APP: {
        pthread_mutex_lock(&g_qmi_mu);
        if (!g_imsi[0]) qmi_refresh_identity_locked();
        char imsi[32];
        snprintf(imsi, sizeof(imsi), "%s", g_imsi);
        pthread_mutex_unlock(&g_qmi_mu);

        klog("IRadioSim.getImsiForApp -> %s", imsi);
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getImsiForAppResp")) {
            wr_resp_info(p, serial, imsi[0] ? RADIO_ERROR_NONE : RADIO_ERROR_GENERIC_FAILURE);
            wr_str(p, imsi);
            send_oneway(cb, SIM_RSP_GET_IMSI_FOR_APP, p);
        }
        break;
    }
    case SIM_TX_ICC_IO_FOR_APP: {
        /* Read IccIo parcelable: [non_null][size][command, fileId, path, p1, p2, p3, data, pin2, aid] */
        int32_t non_null = 0, psize = 0;
        g_ndk.readInt32(in, &non_null);
        int32_t start = g_ndk.getDataPosition(in);
        g_ndk.readInt32(in, &psize);
        int32_t cmd = 0, file_id = 0, p1 = 0, p2 = 0, p3 = 0;
        g_ndk.readInt32(in, &cmd);
        g_ndk.readInt32(in, &file_id);
        char *path_str = rd_str_dup(in);
        g_ndk.readInt32(in, &p1);
        g_ndk.readInt32(in, &p2);
        g_ndk.readInt32(in, &p3);
        (void)start;

        uint8_t path_bin[16];
        size_t plen = parse_ef_path(path_str, path_bin, sizeof(path_bin));
        if (plen == 0) {
            path_bin[0] = 0x00; path_bin[1] = 0x3F;
            path_bin[2] = 0xFF; path_bin[3] = 0x7F;
            plen = 4;
        }
        uint8_t sw1 = 0x6F, sw2 = 0x00;
        uint8_t data[256];
        size_t dlen = 0;
        bool ok = false;
        bool got_sw = false;

        if (cmd == 0xB0 /* READ_BINARY */ ||
            cmd == 0xB2 /* READ_RECORD */ ||
            cmd == 0xC0 /* GET_RESPONSE */) {
            uint8_t sess = (file_id == 0x2FE2 ||
                            (plen == 2 && path_bin[0] == 0x00 && path_bin[1] == 0x3F)) ? 6 : 0;
            pthread_mutex_lock(&g_qmi_mu);
            if (cmd == 0xC0) {
                ok = qmi_uim_get_file_attrs_locked(sess, (uint16_t)file_id, path_bin,
                                                   (uint8_t)plen, data, sizeof(data), &dlen,
                                                   &sw1, &sw2, &got_sw);
            } else if (cmd == 0xB2) {
                ok = qmi_uim_read_ef_locked(0x0021, sess, (uint16_t)file_id, path_bin,
                                            (uint8_t)plen, (uint16_t)p1, (uint16_t)p3,
                                            data, sizeof(data), &dlen, &sw1, &sw2, &got_sw);
            } else {
                ok = qmi_uim_read_transparent_locked(sess, (uint16_t)file_id, path_bin,
                                                     (uint8_t)plen, (uint16_t)((p1 << 8) | p2),
                                                     (uint16_t)p3, data, sizeof(data), &dlen,
                                                     &sw1, &sw2, &got_sw);
            }
            pthread_mutex_unlock(&g_qmi_mu);
        }

        char hex[520];
        hex[0] = '\0';
        for (size_t i = 0; i < dlen && i * 2 + 2 < sizeof(hex); i++)
            snprintf(hex + i * 2, 3, "%02X", data[i]);

        free(path_str);
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "iccIoForAppResp")) {
            wr_resp_info(p, serial, (ok || got_sw) ? RADIO_ERROR_NONE : RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            wr_icc_io_result(p, sw1, sw2, hex);
            send_oneway(cb, SIM_RSP_ICC_IO_FOR_APP, p);
        }
        break;
    }
    case SIM_TX_GET_FACILITY_LOCK: {
        char *fac = rd_str_dup(in);
        klog("IRadioSim.getFacilityLockForApp serial=%d fac=%s -> unlocked", serial, fac);
        free(fac);
        send_i32_resp(cb, SIM_RSP_GET_FACILITY_LOCK, serial, RADIO_ERROR_NONE, 0, "getFacilityLockForAppResp");
        break;
    }
    case SIM_TX_ICC_OPEN_LOGICAL_CH: {
        char *aid = rd_str_dup(in);
        klog("IRadioSim.iccOpenLogicalChannel serial=%d aid=%s -> NO_SUCH_ELEMENT", serial, aid);
        free(aid);
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "iccOpenLogicalChannelResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NO_SUCH_ELEMENT);
            g_ndk.writeInt32(p, -1); /* channelId */
            int8_t sw[2] = { (int8_t)0x6A, (int8_t)0x82 };
            g_ndk.writeByteArray(p, sw, 2);
            send_oneway(cb, SIM_RSP_ICC_OPEN_LOGICAL_CH, p);
        }
        break;
    }
    case SIM_TX_GET_ALLOWED_CARRIERS: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getAllowedCarriersResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            int32_t cr = parcelable_begin(p);
            g_ndk.writeInt32(p, 0);    /* allowedCarriers[0] */
            g_ndk.writeInt32(p, 0);    /* excludedCarriers[0] */
            g_ndk.writeBool(p, true);  /* allowedCarriersPrioritized */
            parcelable_end(p, cr);
            g_ndk.writeInt32(p, 0);    /* SimLockMultiSimPolicy::NO_MULTISIM_POLICY */
            send_oneway(cb, SIM_RSP_GET_ALLOWED_CARRIERS, p);
        }
        break;
    }
    case SIM_TX_GET_PB_CAPACITY: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getSimPhonebookCapacityResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            int32_t pb = parcelable_begin(p);
            for (int i = 0; i < 10; i++)
                g_ndk.writeInt32(p, 0);
            parcelable_end(p, pb);
            send_oneway(cb, SIM_RSP_GET_PB_CAPACITY, p);
        }
        break;
    }
    case SIM_TX_ENABLE_UICC_APPS:
        send_simple_resp(cb, SIM_RSP_ENABLE_UICC_APPS, serial, RADIO_ERROR_NONE, "enableUiccApplicationsResp");
        break;
    case SIM_TX_ICC_CLOSE_LOGICAL_CH:
        send_simple_resp(cb, SIM_RSP_ICC_CLOSE_LOGICAL_CH, serial, RADIO_ERROR_NONE, "iccCloseLogicalChannelResp");
        break;
    case SIM_TX_SEND_TERMINAL_RESP:
        send_simple_resp(cb, SIM_RSP_SEND_TERMINAL_RESP, serial, RADIO_ERROR_NONE, "sendTerminalResponseToSimResp");
        break;
    case SIM_TX_SET_UICC_SUBSCRIPTION:
        send_simple_resp(cb, SIM_RSP_SET_UICC_SUBSCRIPTION, serial, RADIO_ERROR_NONE, "setUiccSubscriptionResp");
        break;
    case SIM_TX_GET_PB_RECORDS:
        send_simple_resp(cb, SIM_RSP_GET_PB_RECORDS, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "getSimPhonebookRecordsResp");
        break;
    case SIM_TX_SET_ALLOWED_CARRIERS:
        send_simple_resp(cb, SIM_RSP_SET_ALLOWED_CARRIERS, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "setAllowedCarriersResp");
        break;
    case SIM_TX_SET_CARRIER_IMSI_ENC:
        send_simple_resp(cb, SIM_RSP_SET_CARRIER_IMSI_ENC, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "setCarrierInfoForImsiEncryptionResp");
        break;
    case SIM_TX_SET_CDMA_SUB_SOURCE:
        send_simple_resp(cb, SIM_RSP_SET_CDMA_SUB_SOURCE, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "setCdmaSubscriptionSourceResp");
        break;
    case SIM_TX_ICC_TX_APDU_BASIC:
    case SIM_TX_ICC_TX_APDU_LOGICAL:
    case SIM_TX_REQ_ICC_SIM_AUTH:
    case SIM_TX_SEND_ENVELOPE_STATUS: {
        uint32_t rsp_code = (code == SIM_TX_ICC_TX_APDU_BASIC) ? SIM_RSP_ICC_TX_APDU_BASIC :
                            (code == SIM_TX_ICC_TX_APDU_LOGICAL) ? SIM_RSP_ICC_TX_APDU_LOGICAL :
                            (code == SIM_TX_REQ_ICC_SIM_AUTH) ? SIM_RSP_REQ_ICC_SIM_AUTH :
                            SIM_RSP_SEND_ENVELOPE_STATUS;
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "simIccIoErrResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            wr_icc_io_result(p, 0x6F, 0x00, "");
            send_oneway(cb, rsp_code, p);
        }
        break;
    }
    case SIM_TX_SEND_ENVELOPE: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "sendEnvelopeResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            wr_str(p, "");
            send_oneway(cb, SIM_RSP_SEND_ENVELOPE, p);
        }
        break;
    }
    case SIM_TX_GET_CDMA_SUBSCRIPTION: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getCdmaSubscriptionResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            for (int i = 0; i < 5; i++)
                wr_str(p, "");
            send_oneway(cb, SIM_RSP_GET_CDMA_SUBSCRIPTION, p);
        }
        break;
    }
    case SIM_TX_GET_CDMA_SUB_SOURCE:
        send_i32_resp(cb, SIM_RSP_GET_CDMA_SUB_SOURCE, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, 0, "getCdmaSubscriptionSourceResp");
        break;
    case SIM_TX_CHANGE_ICC_PIN2:
        send_i32_resp(cb, SIM_RSP_CHANGE_ICC_PIN2, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, -1, "changeIccPin2ForAppResp");
        break;
    case SIM_TX_CHANGE_ICC_PIN:
        send_i32_resp(cb, SIM_RSP_CHANGE_ICC_PIN, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, -1, "changeIccPinForAppResp");
        break;
    case SIM_TX_SET_FACILITY_LOCK:
        send_i32_resp(cb, SIM_RSP_SET_FACILITY_LOCK, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, -1, "setFacilityLockForAppResp");
        break;
    case SIM_TX_SUPPLY_ICC_PIN2:
        send_i32_resp(cb, SIM_RSP_SUPPLY_ICC_PIN2, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, -1, "supplyIccPin2ForAppResp");
        break;
    case SIM_TX_SUPPLY_ICC_PIN:
        send_i32_resp(cb, SIM_RSP_SUPPLY_ICC_PIN, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, -1, "supplyIccPinForAppResp");
        break;
    case SIM_TX_SUPPLY_ICC_PUK2:
        send_i32_resp(cb, SIM_RSP_SUPPLY_ICC_PUK2, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, -1, "supplyIccPuk2ForAppResp");
        break;
    case SIM_TX_SUPPLY_ICC_PUK:
        send_i32_resp(cb, SIM_RSP_SUPPLY_ICC_PUK, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, -1, "supplyIccPukForAppResp");
        break;
    case SIM_TX_UPDATE_PB_RECORDS:
        send_i32_resp(cb, SIM_RSP_UPDATE_PB_RECORDS, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, -1, "updateSimPhonebookRecordsResp");
        break;
    case SIM_TX_SUPPLY_SIM_DEPERSO: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "supplySimDepersonalizationResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            g_ndk.writeInt32(p, 0);
            g_ndk.writeInt32(p, -1);
            send_oneway(cb, SIM_RSP_SUPPLY_SIM_DEPERSO, p);
        }
        break;
    }
    case SIM_TX_REPORT_STK_RUNNING:
        send_simple_resp(cb, SIM_RSP_REPORT_STK_RUNNING, serial, RADIO_ERROR_NONE, "reportStkServiceIsRunningResp");
        break;
    case SIM_TX_SET_SIM_CARD_POWER:
        send_simple_resp(cb, SIM_RSP_SET_SIM_CARD_POWER, serial, RADIO_ERROR_NONE, "setSimCardPowerResp");
        break;
    default:
        klog("IRadioSim unhandled tx=%u serial=%d", code, serial);
        break;
    }
    g_ndk.decStrong(cb);
    return STATUS_OK_VAL;
}

/* =========================================================================
 * 4. android.hardware.radio.network.IRadioNetwork (V1)
 * ========================================================================= */
static const char INET_DESC[] = "android.hardware.radio.network.IRadioNetwork";
static const char INET_INST[] = "android.hardware.radio.network.IRadioNetwork/slot1";
static const char INET_HASH[] = "57e8e923513d80a26102e450d335e89b4346be66";

enum {
    NET_TX_GET_ALLOWED_NET_TYPES = 1,
    NET_TX_GET_AVAIL_BAND_MODES = 2,
    NET_TX_GET_AVAIL_NETWORKS = 3,
    NET_TX_GET_BARRING_INFO = 4,
    NET_TX_GET_CDMA_ROAMING = 5,
    NET_TX_GET_CELL_INFO_LIST = 6,
    NET_TX_GET_DATA_REG_STATE = 7,
    NET_TX_GET_IMS_REG_STATE = 8,
    NET_TX_GET_NET_SEL_MODE = 9,
    NET_TX_GET_OPERATOR = 10,
    NET_TX_GET_SIGNAL_STRENGTH = 11,
    NET_TX_GET_SYS_SEL_CHANNELS = 12,
    NET_TX_GET_VOICE_RAT = 13,
    NET_TX_GET_VOICE_REG_STATE = 14,
    NET_TX_IS_NR_DC_ENABLED = 15,
    NET_TX_RESP_ACK = 16,
    NET_TX_SET_ALLOWED_NET_TYPES = 17,
    NET_TX_SET_BAND_MODE = 18,
    NET_TX_SET_BARRING_PASS = 19,
    NET_TX_SET_CDMA_ROAMING = 20,
    NET_TX_SET_CELL_INFO_RATE = 21,
    NET_TX_SET_IND_FILTER = 22,
    NET_TX_SET_LINK_CAP_CRITERIA = 23,
    NET_TX_SET_LOC_UPDATES = 24,
    NET_TX_SET_NET_SEL_AUTO = 25,
    NET_TX_SET_NET_SEL_MANUAL = 26,
    NET_TX_SET_NR_DC_STATE = 27,
    NET_TX_SET_RESPONSE_FUNCTIONS = 28,
    NET_TX_SET_SIGNAL_CRITERIA = 29,
    NET_TX_SET_SUPP_SVC_NOTIF = 30,
    NET_TX_SET_SYS_SEL_CHANNELS = 31,
    NET_TX_START_NET_SCAN = 32,
    NET_TX_STOP_NET_SCAN = 33,
    NET_TX_SUPPLY_NET_DEPERSO = 34,
    NET_TX_SET_USAGE_SETTING = 35,
    NET_TX_GET_USAGE_SETTING = 36,
};

enum {
    NET_RSP_ACK = 1,
    NET_RSP_GET_ALLOWED_NET_TYPES = 2,
    NET_RSP_GET_AVAIL_BAND_MODES = 3,
    NET_RSP_GET_AVAIL_NETWORKS = 4,
    NET_RSP_GET_BARRING_INFO = 5,
    NET_RSP_GET_CDMA_ROAMING = 6,
    NET_RSP_GET_CELL_INFO_LIST = 7,
    NET_RSP_GET_DATA_REG_STATE = 8,
    NET_RSP_GET_IMS_REG_STATE = 9,
    NET_RSP_GET_NET_SEL_MODE = 10,
    NET_RSP_GET_OPERATOR = 11,
    NET_RSP_GET_SIGNAL_STRENGTH = 12,
    NET_RSP_GET_SYS_SEL_CHANNELS = 13,
    NET_RSP_GET_VOICE_RAT = 14,
    NET_RSP_GET_VOICE_REG_STATE = 15,
    NET_RSP_IS_NR_DC_ENABLED = 16,
    NET_RSP_SET_ALLOWED_NET_TYPES = 17,
    NET_RSP_SET_BAND_MODE = 18,
    NET_RSP_SET_BARRING_PASS = 19,
    NET_RSP_SET_CDMA_ROAMING = 20,
    NET_RSP_SET_CELL_INFO_RATE = 21,
    NET_RSP_SET_IND_FILTER = 22,
    NET_RSP_SET_LINK_CAP_CRITERIA = 23,
    NET_RSP_SET_LOC_UPDATES = 24,
    NET_RSP_SET_NET_SEL_AUTO = 25,
    NET_RSP_SET_NET_SEL_MANUAL = 26,
    NET_RSP_SET_NR_DC_STATE = 27,
    NET_RSP_SET_SIGNAL_CRITERIA = 28,
    NET_RSP_SET_SUPP_SVC_NOTIF = 29,
    NET_RSP_SET_SYS_SEL_CHANNELS = 30,
    NET_RSP_START_NET_SCAN = 31,
    NET_RSP_STOP_NET_SCAN = 32,
    NET_RSP_SUPPLY_NET_DEPERSO = 33,
    NET_RSP_SET_USAGE_SETTING = 34,
    NET_RSP_GET_USAGE_SETTING = 35,
};

enum {
    NET_IND_CURRENT_SIGNAL_STRENGTH = 6,
    NET_IND_NETWORK_STATE_CHANGED = 9,
};

struct nas_snapshot {
    int32_t  reg_state; /* Android RegState: 0=NOT_REG, 1=REG_HOME, 2=SEARCHING */
    int32_t  rat;       /* Android RadioTechnology: 14=LTE, 3=UMTS, 16=GSM */
    char     mcc[8];
    char     mnc[8];
    char     plmn[16];
    char     op_name[64];
    uint32_t cell_id;
    uint16_t tac;
    int32_t  lte_asu;
    int32_t  lte_rsrp;  /* positive magnitude e.g. 85 for -85 dBm */
    int32_t  lte_rsrq;  /* positive magnitude e.g. 11 for -11 dB */
    int32_t  lte_snr;
    int32_t  gsm_asu;
    int32_t  wcdma_rscp;
    int32_t  wcdma_ecno;
};

static bool is_printable_ascii(const uint8_t *p, size_t len, int min_alnum) {
    int alnum = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t ch = p[i];
        if (ch < 0x20 || ch > 0x7e)
            return false;
        if ((ch >= '0' && ch <= '9') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z')) {
            alnum++;
        }
    }
    return alnum >= min_alnum;
}

static int32_t rssi_to_asu(int8_t rssi) {
    if (rssi == 0 || rssi < -113)
        return 99;
    int asu = (rssi + 113) / 2;
    if (asu < 0) asu = 0;
    if (asu > 31) asu = 31;
    return asu;
}

static void qmi_query_nas(struct nas_snapshot *s) {
    memset(s, 0, sizeof(*s));
    s->reg_state  = 0;
    s->rat        = 0;
    s->lte_asu    = 99;
    s->lte_rsrp   = INT32_MAX_VAL;
    s->lte_rsrq   = INT32_MAX_VAL;
    s->lte_snr    = INT32_MAX_VAL;
    s->gsm_asu    = 99;
    s->wcdma_rscp = INT32_MAX_VAL;
    s->wcdma_ecno = INT32_MAX_VAL;

    uint8_t rsp[1024];
    size_t rlen = 0;
    pthread_mutex_lock(&g_qmi_mu);
    if (qmi_call_locked(QMI_SVC_NAS, &g_svc_nas, 0x0024, NULL, 0, rsp, sizeof(rsp), &rlen) == 0) {
        uint16_t l = 0;
        const uint8_t *ss = qmi_tlv_find(rsp, rlen, 0x01, &l);
        if (ss && l >= 5) {
            if (ss[0] == 1) s->reg_state = 1;      /* REG_HOME */
            else if (ss[0] == 2) s->reg_state = 2; /* NOT_REG_SEARCHING */
            uint8_t nrat = ss[4];
            if (nrat > 0 && l >= 6) {
                switch (ss[5]) {
                case 8: s->rat = 14; break; /* LTE */
                case 5: s->rat = 3;  break; /* UMTS */
                case 4: s->rat = 16; break; /* GSM */
                default: s->rat = 14; break;
                }
            }
        }
        const uint8_t *plmn = qmi_tlv_find(rsp, rlen, 0x12, &l);
        if (plmn && l >= 5) {
            uint16_t mcc = qmi_rd16(plmn);
            uint16_t mnc = qmi_rd16(plmn + 2);
            uint8_t dlen = plmn[4];
            if (5 + dlen > l) dlen = (uint8_t)(l - 5);
            snprintf(s->mcc, sizeof(s->mcc), "%03u", mcc);
            snprintf(s->mnc, sizeof(s->mnc), "%02u", mnc);
            snprintf(s->plmn, sizeof(s->plmn), "%03u%02u", mcc, mnc);
            if (dlen >= 3 && is_printable_ascii(plmn + 5, dlen, 2)) {
                size_t c = dlen < sizeof(s->op_name) - 1 ? dlen : sizeof(s->op_name) - 1;
                memcpy(s->op_name, plmn + 5, c);
                s->op_name[c] = '\0';
            }
        }
        const uint8_t *cid = qmi_tlv_find(rsp, rlen, 0x1E, &l);
        if (cid && l >= 4)
            s->cell_id = qmi_rd32(cid);
        const uint8_t *tac = qmi_tlv_find(rsp, rlen, 0x24, &l);
        if (tac && l >= 2)
            s->tac = qmi_rd16(tac);
    }
    if (s->op_name[0] == '\0' &&
        qmi_call_locked(QMI_SVC_NAS, &g_svc_nas, 0x0039, NULL, 0, rsp, sizeof(rsp), &rlen) == 0) {
        uint16_t l = 0;
        const uint8_t *spn = qmi_tlv_find(rsp, rlen, 0x10, &l);
        if (spn && l >= 2) {
            uint8_t spn_len = spn[1];
            if (2u + spn_len <= l && spn_len >= 2 && is_printable_ascii(spn + 2, spn_len, 1)) {
                size_t c = spn_len < sizeof(s->op_name) - 1 ? spn_len : sizeof(s->op_name) - 1;
                memcpy(s->op_name, spn + 2, c);
                s->op_name[c] = '\0';
            }
        }
    }
    if (qmi_call_locked(QMI_SVC_NAS, &g_svc_nas, 0x004F, NULL, 0, rsp, sizeof(rsp), &rlen) == 0) {
        uint16_t l = 0;
        const uint8_t *lte = qmi_tlv_find(rsp, rlen, 0x14, &l);
        if (lte && l >= 6) {
            int8_t rssi = (int8_t)lte[0];
            int8_t rsrq = (int8_t)lte[1];
            int16_t rsrp = (int16_t)qmi_rd16(lte + 2);
            int16_t snr  = (int16_t)qmi_rd16(lte + 4);
            s->lte_asu  = rssi_to_asu(rssi);
            s->lte_rsrp = rsrp < 0 ? -rsrp : rsrp;
            s->lte_rsrq = rsrq < 0 ? -rsrq : rsrq;
            s->lte_snr  = snr;
        }
        const uint8_t *wcdma = qmi_tlv_find(rsp, rlen, 0x13, &l);
        if (wcdma && l >= 3) {
            int8_t wrssi = (int8_t)wcdma[0];
            int16_t wecio = (int16_t)qmi_rd16(wcdma + 1);
            s->wcdma_rscp = (wrssi == 0) ? INT32_MAX_VAL : (wrssi < 0 ? -wrssi : wrssi);
            s->wcdma_ecno = (wecio < 0) ? (-wecio) / 2 : wecio / 2;
        }
        const uint8_t *gsm = qmi_tlv_find(rsp, rlen, 0x11, &l);
        if (gsm && l >= 1)
            s->gsm_asu = rssi_to_asu((int8_t)gsm[0]);
    }
    pthread_mutex_unlock(&g_qmi_mu);
}

static void wr_cell_identity(AParcel *p, const struct nas_snapshot *ns) {
    if (ns->reg_state == 1 && ns->rat == 14 && ns->mcc[0] && ns->mnc[0]) {
        g_ndk.writeInt32(p, 1); /* non-null union */
        g_ndk.writeInt32(p, 5); /* tag = lte */
        int32_t lte = parcelable_begin(p);
        wr_str(p, ns->mcc);
        wr_str(p, ns->mnc);
        g_ndk.writeInt32(p, ns->cell_id ? (int32_t)(ns->cell_id & 0x0fffffffu) : INT32_MAX_VAL);
        g_ndk.writeInt32(p, INT32_MAX_VAL); /* pci */
        g_ndk.writeInt32(p, ns->tac ? (int32_t)ns->tac : INT32_MAX_VAL);
        g_ndk.writeInt32(p, INT32_MAX_VAL); /* earfcn */
        int32_t on = parcelable_begin(p);
        wr_str(p, ns->op_name);
        wr_str(p, ns->op_name);
        parcelable_end(p, on);
        g_ndk.writeInt32(p, INT32_MAX_VAL); /* bandwidth */
        g_ndk.writeInt32(p, 0);             /* additionalPlmns String[0] */
        g_ndk.writeInt32(p, 0);             /* csgInfo = null */
        int32_t empty_i32 = 0;
        g_ndk.writeInt32Array(p, &empty_i32, 0); /* bands */
        parcelable_end(p, lte);
    } else {
        /* CellIdentity union: tag 0 = noinit(bool false) */
        g_ndk.writeInt32(p, 1); /* non-null union */
        g_ndk.writeInt32(p, 0); /* tag = noinit */
        g_ndk.writeBool(p, false);
    }
}

static void wr_reg_state_result(AParcel *p, const struct nas_snapshot *ns) {
    int32_t s = parcelable_begin(p);
    g_ndk.writeInt32(p, ns->reg_state);
    g_ndk.writeInt32(p, ns->rat);
    g_ndk.writeInt32(p, 0); /* reasonForDenial */
    wr_cell_identity(p, ns);
    wr_str(p, ns->plmn);    /* registeredPlmn */
    /* AccessTechnologySpecificInfo union: tag 0 = noinit(bool false) */
    g_ndk.writeInt32(p, 1); /* non-null union */
    g_ndk.writeInt32(p, 0); /* tag = noinit */
    g_ndk.writeBool(p, false);
    parcelable_end(p, s);
}

static void wr_signal_strength(AParcel *p, const struct nas_snapshot *ns) {
    int32_t ss = parcelable_begin(p);
    /* gsm */
    int32_t g = parcelable_begin(p);
    g_ndk.writeInt32(p, ns->gsm_asu);
    g_ndk.writeInt32(p, 99);
    g_ndk.writeInt32(p, INT32_MAX_VAL);
    parcelable_end(p, g);
    /* cdma */
    int32_t c = parcelable_begin(p);
    g_ndk.writeInt32(p, -1);
    g_ndk.writeInt32(p, -1);
    parcelable_end(p, c);
    /* evdo */
    int32_t e = parcelable_begin(p);
    g_ndk.writeInt32(p, -1);
    g_ndk.writeInt32(p, -1);
    g_ndk.writeInt32(p, -1);
    parcelable_end(p, e);
    /* lte */
    int32_t l = parcelable_begin(p);
    g_ndk.writeInt32(p, ns->lte_asu);
    g_ndk.writeInt32(p, ns->lte_rsrp);
    g_ndk.writeInt32(p, ns->lte_rsrq);
    g_ndk.writeInt32(p, ns->lte_snr);
    g_ndk.writeInt32(p, INT32_MAX_VAL);
    g_ndk.writeInt32(p, INT32_MAX_VAL);
    g_ndk.writeInt32(p, INT32_MAX_VAL);
    parcelable_end(p, l);
    /* tdscdma */
    int32_t t = parcelable_begin(p);
    g_ndk.writeInt32(p, 99);
    g_ndk.writeInt32(p, 99);
    g_ndk.writeInt32(p, INT32_MAX_VAL);
    parcelable_end(p, t);
    /* wcdma */
    int32_t w = parcelable_begin(p);
    g_ndk.writeInt32(p, 99);
    g_ndk.writeInt32(p, 99);
    g_ndk.writeInt32(p, ns->wcdma_rscp);
    g_ndk.writeInt32(p, ns->wcdma_ecno);
    parcelable_end(p, w);
    /* nr */
    int32_t n = parcelable_begin(p);
    g_ndk.writeInt32(p, INT32_MAX_VAL);
    g_ndk.writeInt32(p, INT32_MAX_VAL);
    g_ndk.writeInt32(p, INT32_MAX_VAL);
    g_ndk.writeInt32(p, INT32_MAX_VAL);
    g_ndk.writeInt32(p, INT32_MAX_VAL);
    g_ndk.writeInt32(p, INT32_MAX_VAL);
    g_ndk.writeInt32(p, INT32_MAX_VAL);
    int8_t empty_b = 0;
    g_ndk.writeByteArray(p, &empty_b, 0);
    parcelable_end(p, n);
    parcelable_end(p, ss);
}

static void send_signal_strength_ind(const struct nas_snapshot *ns) {
    AIBinder *ind = get_binder(&g_net_ind);
    if (!ind) return;
    AParcel *p = NULL;
    if (prep_tx(ind, &p, "currentSignalStrength")) {
        g_ndk.writeInt32(p, RADIO_IND_UNSOLICITED);
        wr_signal_strength(p, ns);
        send_oneway(ind, NET_IND_CURRENT_SIGNAL_STRENGTH, p);
    }
    g_ndk.decStrong(ind);
}

static int32_t net_on_transact(AIBinder *b, uint32_t code, const AParcel *in, AParcel *out) {
    (void)b;
    if (code == TX_GET_INTERFACE_VERSION || code == TX_GET_INTERFACE_HASH)
        return reply_version_or_hash("IRadioNetwork", code, out, INET_HASH);

    if (code == NET_TX_RESP_ACK)
        return STATUS_OK_VAL;

    if (code == NET_TX_SET_RESPONSE_FUNCTIONS) {
        AIBinder *rsp = NULL, *ind = NULL;
        g_ndk.readStrongBinder(in, &rsp);
        g_ndk.readStrongBinder(in, &ind);
        replace_cb_binder(&g_net_rsp, rsp, g_net_rsp_cls);
        replace_cb_binder(&g_net_ind, ind, g_net_ind_cls);
        g_nas_force_notify = true;
        klog("IRadioNetwork.setResponseFunctions");
        return STATUS_OK_VAL;
    }

    int32_t serial = 0;
    g_ndk.readInt32(in, &serial);
    AIBinder *cb = get_binder(&g_net_rsp);
    if (!cb) return STATUS_OK_VAL;

    switch (code) {
    case NET_TX_GET_ALLOWED_NET_TYPES: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getAllowedNetworkTypesBitmapResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeInt32(p, 0x0001401f); /* GSM|UMTS|LTE */
            send_oneway(cb, NET_RSP_GET_ALLOWED_NET_TYPES, p);
        }
        break;
    }
    case NET_TX_GET_CELL_INFO_LIST: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getCellInfoListResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeInt32(p, 0); /* CellInfo[0] */
            send_oneway(cb, NET_RSP_GET_CELL_INFO_LIST, p);
        }
        break;
    }
    case NET_TX_GET_DATA_REG_STATE:
    case NET_TX_GET_VOICE_REG_STATE: {
        struct nas_snapshot ns;
        qmi_query_nas(&ns);
        AParcel *p = NULL;
        if (prep_tx(cb, &p, code == NET_TX_GET_DATA_REG_STATE ?
                    "getDataRegistrationStateResp" : "getVoiceRegistrationStateResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            wr_reg_state_result(p, &ns);
            send_oneway(cb, code == NET_TX_GET_DATA_REG_STATE ?
                        NET_RSP_GET_DATA_REG_STATE : NET_RSP_GET_VOICE_REG_STATE, p);
        }
        break;
    }
    case NET_TX_GET_IMS_REG_STATE: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getImsRegistrationStateResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeBool(p, false);
            g_ndk.writeInt32(p, 1); /* 3GPP */
            send_oneway(cb, NET_RSP_GET_IMS_REG_STATE, p);
        }
        break;
    }
    case NET_TX_GET_NET_SEL_MODE: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getNetworkSelectionModeResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeBool(p, false); /* false = automatic */
            send_oneway(cb, NET_RSP_GET_NET_SEL_MODE, p);
        }
        break;
    }
    case NET_TX_GET_OPERATOR: {
        struct nas_snapshot ns;
        qmi_query_nas(&ns);
        const char *name = ns.op_name[0] ? ns.op_name : "";
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getOperatorResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            wr_str(p, name);
            wr_str(p, name);
            wr_str(p, ns.plmn);
            send_oneway(cb, NET_RSP_GET_OPERATOR, p);
        }
        break;
    }
    case NET_TX_GET_SIGNAL_STRENGTH: {
        struct nas_snapshot ns;
        qmi_query_nas(&ns);
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getSignalStrengthResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            wr_signal_strength(p, &ns);
            send_oneway(cb, NET_RSP_GET_SIGNAL_STRENGTH, p);
        }
        break;
    }
    case NET_TX_GET_VOICE_RAT: {
        struct nas_snapshot ns;
        qmi_query_nas(&ns);
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getVoiceRadioTechnologyResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeInt32(p, ns.rat ? ns.rat : 14);
            send_oneway(cb, NET_RSP_GET_VOICE_RAT, p);
        }
        break;
    }
    case NET_TX_SET_ALLOWED_NET_TYPES:
        send_simple_resp(cb, NET_RSP_SET_ALLOWED_NET_TYPES, serial, RADIO_ERROR_NONE, "setAllowedNetTypesResp");
        break;
    case NET_TX_SET_CELL_INFO_RATE:
        send_simple_resp(cb, NET_RSP_SET_CELL_INFO_RATE, serial, RADIO_ERROR_NONE, "setCellInfoListRateResp");
        break;
    case NET_TX_SET_IND_FILTER:
        send_simple_resp(cb, NET_RSP_SET_IND_FILTER, serial, RADIO_ERROR_NONE, "setIndicationFilterResp");
        break;
    case NET_TX_SET_LINK_CAP_CRITERIA:
        send_simple_resp(cb, NET_RSP_SET_LINK_CAP_CRITERIA, serial, RADIO_ERROR_NONE, "setLinkCapacityCriteriaResp");
        break;
    case NET_TX_SET_LOC_UPDATES:
        send_simple_resp(cb, NET_RSP_SET_LOC_UPDATES, serial, RADIO_ERROR_NONE, "setLocationUpdatesResp");
        break;
    case NET_TX_SET_NET_SEL_AUTO:
        send_simple_resp(cb, NET_RSP_SET_NET_SEL_AUTO, serial, RADIO_ERROR_NONE, "setNetSelAutoResp");
        break;
    case NET_TX_SET_SIGNAL_CRITERIA:
        send_simple_resp(cb, NET_RSP_SET_SIGNAL_CRITERIA, serial, RADIO_ERROR_NONE, "setSignalCriteriaResp");
        break;
    case NET_TX_SET_SUPP_SVC_NOTIF:
        send_simple_resp(cb, NET_RSP_SET_SUPP_SVC_NOTIF, serial, RADIO_ERROR_NONE, "setSuppSvcNotifResp");
        break;
    case NET_TX_SET_USAGE_SETTING:
        send_simple_resp(cb, NET_RSP_SET_USAGE_SETTING, serial, RADIO_ERROR_NONE, "setUsageSettingResp");
        break;
    case NET_TX_GET_USAGE_SETTING: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getUsageSettingResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeInt32(p, 1); /* VOICE_CENTRIC */
            send_oneway(cb, NET_RSP_GET_USAGE_SETTING, p);
        }
        break;
    }
    case NET_TX_GET_BARRING_INFO: {
        struct nas_snapshot ns;
        qmi_query_nas(&ns);
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getBarringInfoResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            wr_cell_identity(p, &ns);
            g_ndk.writeInt32(p, 0); /* BarringInfo[0] */
            send_oneway(cb, NET_RSP_GET_BARRING_INFO, p);
        }
        break;
    }
    case NET_TX_GET_AVAIL_BAND_MODES: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getAvailableBandModesResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            int32_t band0 = 0; /* BAND_MODE_UNSPECIFIED */
            g_ndk.writeInt32Array(p, &band0, 1);
            send_oneway(cb, NET_RSP_GET_AVAIL_BAND_MODES, p);
        }
        break;
    }
    case NET_TX_GET_AVAIL_NETWORKS:
        send_empty_array_resp(cb, NET_RSP_GET_AVAIL_NETWORKS, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "getAvailableNetworksResp");
        break;
    case NET_TX_GET_CDMA_ROAMING:
        send_i32_resp(cb, NET_RSP_GET_CDMA_ROAMING, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, 0, "getCdmaRoamingPrefResp");
        break;
    case NET_TX_GET_SYS_SEL_CHANNELS:
        send_empty_array_resp(cb, NET_RSP_GET_SYS_SEL_CHANNELS, serial, RADIO_ERROR_NONE, "getSysSelChannelsResp");
        break;
    case NET_TX_IS_NR_DC_ENABLED:
        send_bool_resp(cb, NET_RSP_IS_NR_DC_ENABLED, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, false, "isNrDcEnabledResp");
        break;
    case NET_TX_SET_BAND_MODE:
        send_simple_resp(cb, NET_RSP_SET_BAND_MODE, serial, RADIO_ERROR_NONE, "setBandModeResp");
        break;
    case NET_TX_SET_SYS_SEL_CHANNELS:
        send_simple_resp(cb, NET_RSP_SET_SYS_SEL_CHANNELS, serial, RADIO_ERROR_NONE, "setSysSelChannelsResp");
        break;
    case NET_TX_STOP_NET_SCAN:
        send_simple_resp(cb, NET_RSP_STOP_NET_SCAN, serial, RADIO_ERROR_NONE, "stopNetworkScanResp");
        break;
    case NET_TX_SET_BARRING_PASS:
        send_simple_resp(cb, NET_RSP_SET_BARRING_PASS, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "setBarringPasswordResp");
        break;
    case NET_TX_SET_CDMA_ROAMING:
        send_simple_resp(cb, NET_RSP_SET_CDMA_ROAMING, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "setCdmaRoamingPreferenceResp");
        break;
    case NET_TX_SET_NET_SEL_MANUAL:
        send_simple_resp(cb, NET_RSP_SET_NET_SEL_MANUAL, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "setNetworkSelectionModeManualResp");
        break;
    case NET_TX_SET_NR_DC_STATE:
        send_simple_resp(cb, NET_RSP_SET_NR_DC_STATE, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "setNrDualConnectivityStateResp");
        break;
    case NET_TX_START_NET_SCAN:
        send_simple_resp(cb, NET_RSP_START_NET_SCAN, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "startNetworkScanResp");
        break;
    case NET_TX_SUPPLY_NET_DEPERSO:
        send_i32_resp(cb, NET_RSP_SUPPLY_NET_DEPERSO, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, -1, "supplyNetDepersoResp");
        break;
    default:
        klog("IRadioNetwork unhandled tx=%u serial=%d", code, serial);
        break;
    }
    g_ndk.decStrong(cb);
    return STATUS_OK_VAL;
}

/* =========================================================================
 * 5. IRadioData / IRadioVoice / IRadioMessaging (V1)
 * ========================================================================= */
static const char IDAT_DESC[] = "android.hardware.radio.data.IRadioData";
static const char IDAT_INST[] = "android.hardware.radio.data.IRadioData/slot1";
static const char IDAT_HASH[] = "cb458326a82e0b621c39a7d6f5c9f1a68e289811";

static const char IVOC_DESC[] = "android.hardware.radio.voice.IRadioVoice";
static const char IVOC_INST[] = "android.hardware.radio.voice.IRadioVoice/slot1";
static const char IVOC_HASH[] = "12113a27a82e0b621c39a7d6f5c9f1a68e289812";

static const char IMSG_DESC[] = "android.hardware.radio.messaging.IRadioMessaging";
static const char IMSG_INST[] = "android.hardware.radio.messaging.IRadioMessaging/slot1";
static const char IMSG_HASH[] = "23224b38a82e0b621c39a7d6f5c9f1a68e289813";

enum {
    DAT_TX_ALLOCATE_PDU_SESSION_ID = 1,
    DAT_TX_CANCEL_HANDOVER = 2,
    DAT_TX_DEACTIVATE_DATA_CALL = 3,
    DAT_TX_GET_DATA_CALL_LIST = 4,
    DAT_TX_GET_SLICING_CONFIG = 5,
    DAT_TX_RELEASE_PDU_SESSION_ID = 6,
    DAT_TX_RESP_ACK = 7,
    DAT_TX_SET_DATA_ALLOWED = 8,
    DAT_TX_SET_DATA_PROFILE = 9,
    DAT_TX_SET_DATA_THROTTLING = 10,
    DAT_TX_SET_INITIAL_ATTACH_APN = 11,
    DAT_TX_SET_RESPONSE_FUNCTIONS = 12,
    DAT_TX_SETUP_DATA_CALL = 13,
    DAT_TX_START_HANDOVER = 14,
    DAT_TX_START_KEEPALIVE = 15,
    DAT_TX_STOP_KEEPALIVE = 16,
    DAT_TX_SET_USER_DATA_ENABLED = 17,
    DAT_TX_SET_USER_DATA_ROAMING_ENABLED = 18,
    DAT_TX_NOTIFY_IMS_DATA_NETWORK = 19,
};

enum {
    DAT_RSP_ALLOCATE_PDU_SESSION_ID = 2,
    DAT_RSP_CANCEL_HANDOVER = 3,
    DAT_RSP_DEACTIVATE_DATA_CALL = 4,
    DAT_RSP_GET_DATA_CALL_LIST = 5,
    DAT_RSP_GET_SLICING_CONFIG = 6,
    DAT_RSP_RELEASE_PDU_SESSION_ID = 7,
    DAT_RSP_SET_DATA_ALLOWED = 8,
    DAT_RSP_SET_DATA_PROFILE = 9,
    DAT_RSP_SET_DATA_THROTTLING = 10,
    DAT_RSP_SET_INITIAL_ATTACH_APN = 11,
    DAT_RSP_SETUP_DATA_CALL = 12,
    DAT_RSP_START_HANDOVER = 13,
    DAT_RSP_START_KEEPALIVE = 14,
    DAT_RSP_STOP_KEEPALIVE = 15,
    DAT_RSP_SET_USER_DATA_ENABLED = 16,
    DAT_RSP_SET_USER_DATA_ROAMING_ENABLED = 17,
    DAT_RSP_NOTIFY_IMS_DATA_NETWORK = 18,
};

enum {
    DAT_IND_DATA_CALL_LIST_CHANGED = 1,
};

enum {
    VOC_TX_ACCEPT_CALL = 1,
    VOC_TX_CANCEL_USSD = 2,
    VOC_TX_CONFERENCE = 3,
    VOC_TX_DIAL = 4,
    VOC_TX_EMERGENCY_DIAL = 5,
    VOC_TX_EXIT_ECBM = 6,
    VOC_TX_EXPLICIT_CALL_TRANSFER = 7,
    VOC_TX_GET_CALL_FWD = 8,
    VOC_TX_GET_CALL_WAITING = 9,
    VOC_TX_GET_CLIP = 10,
    VOC_TX_GET_CLIR = 11,
    VOC_TX_GET_CURRENT_CALLS = 12,
    VOC_TX_GET_LAST_CALL_FAIL_CAUSE = 13,
    VOC_TX_GET_MUTE = 14,
    VOC_TX_GET_PREF_PRIVACY = 15,
    VOC_TX_GET_TTY_MODE = 16,
    VOC_TX_HANDLE_STK_CALL_SETUP = 17,
    VOC_TX_HANGUP = 18,
    VOC_TX_HANGUP_FG_RESUME_BG = 19,
    VOC_TX_HANGUP_WAITING_OR_BG = 20,
    VOC_TX_IS_VONR_ENABLED = 21,
    VOC_TX_REJECT_CALL = 22,
    VOC_TX_RESP_ACK = 23,
    VOC_TX_SEND_BURST_DTMF = 24,
    VOC_TX_SEND_CDMA_FEATURE_CODE = 25,
    VOC_TX_SEND_DTMF = 26,
    VOC_TX_SEND_USSD = 27,
    VOC_TX_SEPARATE_CONNECTION = 28,
    VOC_TX_SET_CALL_FWD = 29,
    VOC_TX_SET_CALL_WAITING = 30,
    VOC_TX_SET_CLIR = 31,
    VOC_TX_SET_MUTE = 32,
    VOC_TX_SET_PREF_PRIVACY = 33,
    VOC_TX_SET_RESPONSE_FUNCTIONS = 34,
    VOC_TX_SET_TTY_MODE = 35,
    VOC_TX_SET_VONR_ENABLED = 36,
    VOC_TX_START_DTMF = 37,
    VOC_TX_STOP_DTMF = 38,
    VOC_TX_SWITCH_HOLDING_ACTIVE = 39,
};

enum {
    VOC_RSP_ACCEPT_CALL = 1,
    VOC_RSP_ACK = 2,
    VOC_RSP_CANCEL_USSD = 3,
    VOC_RSP_CONFERENCE = 4,
    VOC_RSP_DIAL = 5,
    VOC_RSP_EMERGENCY_DIAL = 6,
    VOC_RSP_EXIT_ECBM = 7,
    VOC_RSP_EXPLICIT_CALL_TRANSFER = 8,
    VOC_RSP_GET_CALL_FWD = 9,
    VOC_RSP_GET_CALL_WAITING = 10,
    VOC_RSP_GET_CLIP = 11,
    VOC_RSP_GET_CLIR = 12,
    VOC_RSP_GET_CURRENT_CALLS = 13,
    VOC_RSP_GET_LAST_CALL_FAIL_CAUSE = 14,
    VOC_RSP_GET_MUTE = 15,
    VOC_RSP_GET_PREF_PRIVACY = 16,
    VOC_RSP_GET_TTY_MODE = 17,
    VOC_RSP_HANDLE_STK_CALL_SETUP = 18,
    VOC_RSP_HANGUP = 19,
    VOC_RSP_HANGUP_FG_RESUME_BG = 20,
    VOC_RSP_HANGUP_WAITING_OR_BG = 21,
    VOC_RSP_IS_VONR_ENABLED = 22,
    VOC_RSP_REJECT_CALL = 23,
    VOC_RSP_SEND_BURST_DTMF = 24,
    VOC_RSP_SEND_CDMA_FEATURE_CODE = 25,
    VOC_RSP_SEND_DTMF = 26,
    VOC_RSP_SEND_USSD = 27,
    VOC_RSP_SEPARATE_CONNECTION = 28,
    VOC_RSP_SET_CALL_FWD = 29,
    VOC_RSP_SET_CALL_WAITING = 30,
    VOC_RSP_SET_CLIR = 31,
    VOC_RSP_SET_MUTE = 32,
    VOC_RSP_SET_PREF_PRIVACY = 33,
    VOC_RSP_SET_TTY_MODE = 34,
    VOC_RSP_SET_VONR_ENABLED = 35,
    VOC_RSP_START_DTMF = 36,
    VOC_RSP_STOP_DTMF = 37,
    VOC_RSP_SWITCH_HOLDING_ACTIVE = 38,
};

enum {
    VOC_IND_CALL_RING = 1,
    VOC_IND_CALL_STATE_CHANGED = 2,
};

enum {
    MSG_IND_NEW_SMS = 4,
};

enum {
    MSG_TX_ACK_INCOMING_GSM_PDU = 1,
    MSG_TX_ACK_LAST_CDMA = 2,
    MSG_TX_ACK_LAST_GSM = 3,
    MSG_TX_DELETE_SMS_RUIM = 4,
    MSG_TX_DELETE_SMS_SIM = 5,
    MSG_TX_GET_CDMA_BCAST_CFG = 6,
    MSG_TX_GET_GSM_BCAST_CFG = 7,
    MSG_TX_GET_SMSC_ADDRESS = 8,
    MSG_TX_REPORT_SMS_MEM = 9,
    MSG_TX_RESP_ACK = 10,
    MSG_TX_SEND_CDMA_SMS = 11,
    MSG_TX_SEND_CDMA_SMS_MORE = 12,
    MSG_TX_SEND_IMS_SMS = 13,
    MSG_TX_SEND_SMS = 14,
    MSG_TX_SEND_SMS_MORE = 15,
    MSG_TX_SET_CDMA_BCAST_ACT = 16,
    MSG_TX_SET_CDMA_BCAST_CFG = 17,
    MSG_TX_SET_GSM_BCAST_ACT = 18,
    MSG_TX_SET_GSM_BCAST_CFG = 19,
    MSG_TX_SET_RESPONSE_FUNCTIONS = 20,
    MSG_TX_SET_SMSC_ADDRESS = 21,
    MSG_TX_WRITE_SMS_RUIM = 22,
    MSG_TX_WRITE_SMS_SIM = 23,
};

enum {
    MSG_RSP_ACK_INCOMING_GSM_PDU = 1,
    MSG_RSP_ACK_LAST_CDMA = 2,
    MSG_RSP_ACK_LAST_GSM = 3,
    MSG_RSP_ACK = 4,
    MSG_RSP_DELETE_SMS_RUIM = 5,
    MSG_RSP_DELETE_SMS_SIM = 6,
    MSG_RSP_GET_CDMA_BCAST_CFG = 7,
    MSG_RSP_GET_GSM_BCAST_CFG = 8,
    MSG_RSP_GET_SMSC_ADDRESS = 9,
    MSG_RSP_REPORT_SMS_MEM = 10,
    MSG_RSP_SEND_CDMA_SMS_MORE = 11,
    MSG_RSP_SEND_CDMA_SMS = 12,
    MSG_RSP_SEND_IMS_SMS = 13,
    MSG_RSP_SEND_SMS_MORE = 14,
    MSG_RSP_SEND_SMS = 15,
    MSG_RSP_SET_CDMA_BCAST_ACT = 16,
    MSG_RSP_SET_CDMA_BCAST_CFG = 17,
    MSG_RSP_SET_GSM_BCAST_ACT = 18,
    MSG_RSP_SET_GSM_BCAST_CFG = 19,
    MSG_RSP_SET_SMSC_ADDRESS = 20,
    MSG_RSP_WRITE_SMS_RUIM = 21,
    MSG_RSP_WRITE_SMS_SIM = 22,
};

/* ---- Netlink / ioctl helpers for rmnet_ipa0 -> rmnet_data0 ---- */
static int net_set_if_up(const char *ifname, bool up) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        int e = -errno;
        close(fd);
        return e;
    }
    if (up)
        ifr.ifr_flags |= IFF_UP;
    else
        ifr.ifr_flags &= ~IFF_UP;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        int e = -errno;
        close(fd);
        return e;
    }
    close(fd);
    return 0;
}

static struct rtattr *nl_add_attr(struct nlmsghdr *nlh, size_t maxlen,
                                  int type, const void *data, size_t alen) {
    size_t len = RTA_LENGTH(alen);
    if (NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(len) > maxlen)
        return NULL;
    struct rtattr *rta = (struct rtattr *)(void *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)len;
    if (alen && data)
        memcpy(RTA_DATA(rta), data, alen);
    nlh->nlmsg_len = (uint32_t)(NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(len));
    return rta;
}

static int net_ensure_rmnet_data(const char *master, const char *vnd, uint16_t mux_id) {
    int rc = net_set_if_up(master, true);
    if (rc < 0) {
        klog("net_set_if_up(%s) failed rc=%d", master, rc);
        return rc;
    }

    if (if_nametoindex(vnd) != 0) {
        net_set_if_up(vnd, true);
        return 0;
    }

    unsigned int master_idx = if_nametoindex(master);
    if (master_idx == 0) {
        klog("if_nametoindex(%s) returned 0", master);
        return -ENODEV;
    }

    struct {
        struct nlmsghdr nh;
        struct ifinfomsg ifi;
        char buf[512];
    } req;
    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifi));
    req.nh.nlmsg_type = RTM_NEWLINK;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    req.nh.nlmsg_seq = 1;
    req.ifi.ifi_family = AF_UNSPEC;

    int32_t link_idx = (int32_t)master_idx;
    nl_add_attr(&req.nh, sizeof(req), IFLA_IFNAME, vnd, strlen(vnd) + 1);
    nl_add_attr(&req.nh, sizeof(req), IFLA_LINK, &link_idx, sizeof(link_idx));

    struct rtattr *linkinfo = nl_add_attr(&req.nh, sizeof(req), IFLA_LINKINFO, NULL, 0);
    if (!linkinfo) return -EMSGSIZE;
    nl_add_attr(&req.nh, sizeof(req), IFLA_INFO_KIND, "rmnet", 6);
    struct rtattr *infodata = nl_add_attr(&req.nh, sizeof(req), IFLA_INFO_DATA, NULL, 0);
    if (!infodata) return -EMSGSIZE;
    nl_add_attr(&req.nh, sizeof(req), IFLA_RMNET_MUX_ID, &mux_id, sizeof(mux_id));
    struct ifla_rmnet_flags rflags = {
        .flags = RMNET_FLAGS_INGRESS_DEAGGREGATION | RMNET_FLAGS_INGRESS_MAP_COMMANDS,
        .mask  = RMNET_FLAGS_INGRESS_DEAGGREGATION | RMNET_FLAGS_INGRESS_MAP_COMMANDS,
    };
    nl_add_attr(&req.nh, sizeof(req), IFLA_RMNET_FLAGS, &rflags, sizeof(rflags));
    infodata->rta_len = (unsigned short)((char *)&req.nh + req.nh.nlmsg_len - (char *)infodata);
    linkinfo->rta_len = (unsigned short)((char *)&req.nh + req.nh.nlmsg_len - (char *)linkinfo);

    int nlfd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (nlfd < 0) return -errno;
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    if (bind(nlfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        rc = -errno;
        close(nlfd);
        return rc;
    }

    if (send(nlfd, &req.nh, req.nh.nlmsg_len, 0) < 0) {
        rc = -errno;
        close(nlfd);
        return rc;
    }

    uint8_t rsp[1024];
    ssize_t n = recv(nlfd, rsp, sizeof(rsp), 0);
    close(nlfd);
    if (n >= (ssize_t)sizeof(struct nlmsghdr)) {
        struct nlmsghdr *rh = (struct nlmsghdr *)(void *)rsp;
        if (rh->nlmsg_type == NLMSG_ERROR &&
            n >= (ssize_t)(sizeof(struct nlmsghdr) + sizeof(struct nlmsgerr))) {
            struct nlmsgerr *ne = (struct nlmsgerr *)(void *)NLMSG_DATA(rh);
            if (ne->error != 0 && ne->error != -EEXIST) {
                klog("RTM_NEWLINK(%s) failed err=%d", vnd, ne->error);
                return ne->error;
            }
        }
    }

    net_set_if_up(vnd, true);
    return 0;
}

/* ---- QMI WDA / WDS helpers (called under g_data_mu) ---- */
static void data_ensure_dpm_opened(void) {
    if (g_dpm_opened) return;

    uint8_t hw_port[17], req[64], rsp[256];
    size_t off = 0, rlen = 0;
    hw_port[0] = 1;              /* 1 hardware data port entry */
    qmi_wr32(hw_port + 1, 4);    /* ep_type = EMBEDDED */
    qmi_wr32(hw_port + 5, 1);    /* iface_id = 1 */
    qmi_wr32(hw_port + 9, 4);    /* rx_id = IPA pipe 4 (AP->modem) */
    qmi_wr32(hw_port + 13, 5);   /* tx_id = IPA pipe 5 (modem->AP) */
    off = qmi_tlv_append(req, off, sizeof(req), 0x11, hw_port, sizeof(hw_port));

    pthread_mutex_lock(&g_qmi_mu);
    int rc = qmi_call_locked(QMI_SVC_DPM, &g_svc_dpm, 0x0020,
                             req, (uint16_t)off, rsp, sizeof(rsp), &rlen);
    pthread_mutex_unlock(&g_qmi_mu);

    klog("DPM_OPEN_PORT(ep=4/1 rx=4 tx=5) rc=%d svc=%u:%u v%u",
         rc, g_svc_dpm.node, g_svc_dpm.port, g_svc_dpm.version);
    if (rc == 0)
        g_dpm_opened = true;
}

static void data_ensure_wda_configured(void) {
    data_ensure_dpm_opened();
    if (g_wda_configured) return;

    uint8_t ep[8], u32buf[4], req[128], rsp[512];
    size_t off = 0, rlen = 0;
    uint8_t qos = 0;

    off = qmi_tlv_append(req, off, sizeof(req), 0x10, &qos, 1);
    qmi_wr32(u32buf, 2); /* RAW_IP */
    off = qmi_tlv_append(req, off, sizeof(req), 0x11, u32buf, 4);
    qmi_wr32(u32buf, 5); /* UL QMAP */
    off = qmi_tlv_append(req, off, sizeof(req), 0x12, u32buf, 4);
    qmi_wr32(u32buf, 5); /* DL QMAP */
    off = qmi_tlv_append(req, off, sizeof(req), 0x13, u32buf, 4);
    qmi_wr32(u32buf, 16); /* DL max datagrams */
    off = qmi_tlv_append(req, off, sizeof(req), 0x15, u32buf, 4);
    qmi_wr32(u32buf, 4096); /* DL max size (fits 8 KiB IPA RX buffer) */
    off = qmi_tlv_append(req, off, sizeof(req), 0x16, u32buf, 4);
    qmi_wr32(ep, 4);     /* ep_type = BAM_DMUX / IPA embedded */
    qmi_wr32(ep + 4, 1); /* iface_id = 1 */
    off = qmi_tlv_append(req, off, sizeof(req), 0x17, ep, sizeof(ep));

    pthread_mutex_lock(&g_qmi_mu);
    int rc = qmi_call_locked(QMI_SVC_WDA, &g_svc_wda, 0x0020,
                             req, (uint16_t)off, rsp, sizeof(rsp), &rlen);
    if (rc != 0) {
        /* Fallback without aggregation size caps if modem rejects them */
        off = 0;
        off = qmi_tlv_append(req, off, sizeof(req), 0x10, &qos, 1);
        qmi_wr32(u32buf, 2);
        off = qmi_tlv_append(req, off, sizeof(req), 0x11, u32buf, 4);
        qmi_wr32(u32buf, 5);
        off = qmi_tlv_append(req, off, sizeof(req), 0x12, u32buf, 4);
        qmi_wr32(u32buf, 5);
        off = qmi_tlv_append(req, off, sizeof(req), 0x13, u32buf, 4);
        off = qmi_tlv_append(req, off, sizeof(req), 0x17, ep, sizeof(ep));
        rc = qmi_call_locked(QMI_SVC_WDA, &g_svc_wda, 0x0020,
                             req, (uint16_t)off, rsp, sizeof(rsp), &rlen);
    }
    pthread_mutex_unlock(&g_qmi_mu);

    if (rc == 0) {
        uint32_t vals[5] = { 0, 0, 0, 0, 0 };
        static const uint8_t tl[5] = { 0x11, 0x12, 0x13, 0x15, 0x16 };
        for (int i = 0; i < 5; i++) {
            uint16_t l = 0;
            const uint8_t *v = qmi_tlv_find(rsp, rlen, tl[i], &l);
            if (v && l >= 4) vals[i] = qmi_rd32(v);
        }
        klog("WDA_SET_DATA_FORMAT OK llp=%u ul=%u dl=%u dgrams=%u dl_sz=%u",
             vals[0], vals[1], vals[2], vals[3], vals[4]);
        g_wda_configured = true;
    } else {
        klog("WDA_SET_DATA_FORMAT(RAW_IP+QMAP ep=4/1) rc=%d", rc);
    }
}

static void data_add_str_uniq(char arr[][64], int32_t *cnt, int32_t max_cnt, const char *s) {
    if (!s || !s[0] || *cnt >= max_cnt) return;
    for (int32_t i = 0; i < *cnt; i++) {
        if (strcmp(arr[i], s) == 0) return;
    }
    snprintf(arr[*cnt], 64, "%s", s);
    (*cnt)++;
}

static bool ipv6_is_nonzero(const uint8_t *b) {
    for (size_t i = 0; i < 16; i++) {
        if (b[i] != 0) return true;
    }
    return false;
}

static void wds_stop_handle(int *fd_p, uint32_t *handle_p) {
    if (*fd_p < 0) {
        *handle_p = 0;
        return;
    }
    if (*handle_p != 0) {
        uint32_t local_node = 0;
        struct qmi_service_addr svc;
        (void)local_node;
        memset(&svc, 0, sizeof(svc));
        struct sockaddr_qrtr me;
        socklen_t sl = sizeof(me);
        if (getsockname(*fd_p, (struct sockaddr *)&me, &sl) == 0 &&
            qmi_lookup_service(*fd_p, me.sq_node, QMI_SVC_WDS, &svc, 1500) == 0) {
            uint8_t req[16], rsp[128], hbuf[4];
            size_t off = 0, rlen = 0;
            qmi_wr32(hbuf, *handle_p);
            off = qmi_tlv_append(req, off, sizeof(req), 0x01, hbuf, 4);
            int rc = qmi_transact(*fd_p, &svc, 100, 0x0021, req, (uint16_t)off,
                                  rsp, sizeof(rsp), &rlen, 2500);
            uint16_t qerr = 0;
            int qrc = (rc == 0) ? qmi_result_code(rsp, rlen, &qerr) : rc;
            klog("WDS_STOP_NETWORK_INTERFACE(handle=0x%x) rc=%d", *handle_p, qrc);
        }
        *handle_p = 0;
    }
    close(*fd_p);
    *fd_p = -1;
}

static void data_call_reset_locked(void) {
    wds_stop_handle(&g_data_call.wds_fd_v4, &g_data_call.handle_v4);
    wds_stop_handle(&g_data_call.wds_fd_v6, &g_data_call.handle_v6);
    g_data_call.active = false;
    g_data_call.n_addrs = 0;
    g_data_call.n_dns = 0;
    g_data_call.n_gws = 0;
    g_data_call.mtu_v4 = 0;
    g_data_call.mtu_v6 = 0;
    g_data_call.has_v4 = false;
    g_data_call.has_v6 = false;
}

static int wds_start_family(struct data_call_state *c, uint8_t ip_pref,
                            const char *apn, int32_t auth_type,
                            const char *user, const char *pass,
                            int *out_fd, uint32_t *out_handle) {
    uint32_t local_node = 0;
    int fd = qmi_open_socket(&local_node);
    if (fd < 0) return -errno;

    struct qmi_service_addr svc;
    if (qmi_lookup_service(fd, local_node, QMI_SVC_WDS, &svc, 2500) < 0) {
        close(fd);
        return -ENOENT;
    }

    uint16_t txn = 1, qerr = 0;
    uint8_t req[512], rsp[1024];
    size_t off = 0, rlen = 0;
    uint8_t ep[8], mux = 1;

    /* 1. WDS_BIND_MUX_DATA_PORT (0x00A2): ep_type=4, iface_id=1, mux_id=1 */
    qmi_wr32(ep, 4);
    qmi_wr32(ep + 4, 1);
    off = qmi_tlv_append(req, off, sizeof(req), 0x10, ep, sizeof(ep));
    off = qmi_tlv_append(req, off, sizeof(req), 0x11, &mux, 1);
    int rc = qmi_transact(fd, &svc, txn++, 0x00A2, req, (uint16_t)off,
                          rsp, sizeof(rsp), &rlen, 2500);
    int qrc = (rc == 0) ? qmi_result_code(rsp, rlen, &qerr) : rc;
    klog("WDS_BIND_MUX_DATA_PORT(ep=4/1 mux=1) rc=%d qerr=%u", qrc, qerr);

    /* 2. WDS_SET_CLIENT_IP_FAMILY_PREF (0x004D) */
    off = qmi_tlv_append(req, 0, sizeof(req), 0x01, &ip_pref, 1);
    rc = qmi_transact(fd, &svc, txn++, 0x004D, req, (uint16_t)off,
                      rsp, sizeof(rsp), &rlen, 2500);
    qrc = (rc == 0) ? qmi_result_code(rsp, rlen, &qerr) : rc;
    if (qrc != 0)
        klog("WDS_SET_CLIENT_IP_FAMILY_PREF(%u) rc=%d qerr=%u", ip_pref, qrc, qerr);

    /* 3. WDS_START_NETWORK_INTERFACE (0x0020) */
    off = 0;
    if (apn && apn[0])
        off = qmi_tlv_append(req, off, sizeof(req), 0x14, apn, (uint16_t)strlen(apn));
    if (auth_type > 0) {
        uint8_t auth = (uint8_t)(auth_type & 0x03);
        off = qmi_tlv_append(req, off, sizeof(req), 0x16, &auth, 1);
    }
    if (user && user[0])
        off = qmi_tlv_append(req, off, sizeof(req), 0x17, user, (uint16_t)strlen(user));
    if (pass && pass[0])
        off = qmi_tlv_append(req, off, sizeof(req), 0x18, pass, (uint16_t)strlen(pass));
    off = qmi_tlv_append(req, off, sizeof(req), 0x19, &ip_pref, 1);

    rc = qmi_transact(fd, &svc, txn++, 0x0020, req, (uint16_t)off,
                      rsp, sizeof(rsp), &rlen, 15000);
    qrc = (rc == 0) ? qmi_result_code(rsp, rlen, &qerr) : rc;
    uint16_t tlen = 0;
    const uint8_t *hv = qmi_tlv_find(rsp, rlen, 0x01, &tlen);
    if (qrc != 0 || !hv || tlen < 4) {
        const uint8_t *cer = qmi_tlv_find(rsp, rlen, 0x10, &tlen);
        uint16_t cer_v = (cer && tlen >= 2) ? qmi_rd16(cer) : 0;
        const uint8_t *vcer = qmi_tlv_find(rsp, rlen, 0x11, &tlen);
        uint16_t v_type = (vcer && tlen >= 4) ? qmi_rd16(vcer) : 0;
        uint16_t v_code = (vcer && tlen >= 4) ? qmi_rd16(vcer + 2) : 0;
        klog("WDS_START_NETWORK_INTERFACE(apn='%s' ip=%u) failed rc=%d qerr=%u cer=%u vcer=%u/%u",
             apn ? apn : "", ip_pref, qrc, qerr, cer_v, v_type, v_code);
        close(fd);
        return -EIO;
    }

    uint32_t handle = qmi_rd32(hv);
    klog("WDS_START_NETWORK_INTERFACE(apn='%s' ip=%u) OK handle=0x%x",
         apn ? apn : "", ip_pref, handle);

    /* 4. WDS_GET_CURRENT_SETTINGS (0x002D) */
    uint8_t mask_req[4];
    qmi_wr32(mask_req, 0x0000ffffu);
    off = qmi_tlv_append(req, 0, sizeof(req), 0x10, mask_req, 4);
    rc = qmi_transact(fd, &svc, txn++, 0x002D, req, (uint16_t)off,
                      rsp, sizeof(rsp), &rlen, 2500);
    qrc = (rc == 0) ? qmi_result_code(rsp, rlen, &qerr) : rc;
    if (qrc == 0) {
        uint32_t mtu = 0;
        const uint8_t *mv = qmi_tlv_find(rsp, rlen, 0x29, &tlen);
        if (mv && tlen >= 4) mtu = qmi_rd32(mv);

        const uint8_t *v4a = qmi_tlv_find(rsp, rlen, 0x1E, &tlen);
        if (v4a && tlen >= 4) {
            uint32_t ip4 = qmi_rd32(v4a);
            if (ip4 != 0) {
                uint32_t mask4 = 0;
                const uint8_t *m4 = qmi_tlv_find(rsp, rlen, 0x21, &tlen);
                if (m4 && tlen >= 4) mask4 = qmi_rd32(m4);
                int pfx = mask4 ? __builtin_popcount(mask4) : 32;
                if (pfx <= 0 || pfx > 32) pfx = 32;
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u/%d",
                         (ip4 >> 24) & 0xffu, (ip4 >> 16) & 0xffu,
                         (ip4 >> 8) & 0xffu, ip4 & 0xffu, pfx);
                data_add_str_uniq(c->addrs, &c->n_addrs, 4, tmp);
                c->has_v4 = true;
                c->mtu_v4 = (mtu > 0) ? (int32_t)mtu : 1500;
            }
        }
        const uint8_t *gw4v = qmi_tlv_find(rsp, rlen, 0x20, &tlen);
        if (gw4v && tlen >= 4) {
            uint32_t gw4 = qmi_rd32(gw4v);
            if (gw4 != 0) {
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u",
                         (gw4 >> 24) & 0xffu, (gw4 >> 16) & 0xffu,
                         (gw4 >> 8) & 0xffu, gw4 & 0xffu);
                data_add_str_uniq(c->gws, &c->n_gws, 4, tmp);
            }
        }
        static const uint8_t dns4_tlvs[2] = { 0x15, 0x16 };
        for (int i = 0; i < 2; i++) {
            const uint8_t *dv = qmi_tlv_find(rsp, rlen, dns4_tlvs[i], &tlen);
            if (dv && tlen >= 4) {
                uint32_t d4 = qmi_rd32(dv);
                if (d4 != 0) {
                    char tmp[64];
                    snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u",
                             (d4 >> 24) & 0xffu, (d4 >> 16) & 0xffu,
                             (d4 >> 8) & 0xffu, d4 & 0xffu);
                    data_add_str_uniq(c->dns, &c->n_dns, 4, tmp);
                }
            }
        }

        const uint8_t *v6a = qmi_tlv_find(rsp, rlen, 0x25, &tlen);
        if (v6a && tlen >= 17 && ipv6_is_nonzero(v6a)) {
            char ip6[INET6_ADDRSTRLEN], tmp[64];
            if (inet_ntop(AF_INET6, v6a, ip6, sizeof(ip6))) {
                uint8_t pfx = v6a[16];
                if (pfx == 0 || pfx > 128) pfx = 64;
                snprintf(tmp, sizeof(tmp), "%s/%u", ip6, (unsigned)pfx);
                data_add_str_uniq(c->addrs, &c->n_addrs, 4, tmp);
                c->has_v6 = true;
                c->mtu_v6 = (mtu > 0) ? (int32_t)mtu : 1500;
            }
        }
        const uint8_t *gw6v = qmi_tlv_find(rsp, rlen, 0x26, &tlen);
        if (gw6v && tlen >= 16 && ipv6_is_nonzero(gw6v)) {
            char ip6[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, gw6v, ip6, sizeof(ip6)))
                data_add_str_uniq(c->gws, &c->n_gws, 4, ip6);
        }
        static const uint8_t dns6_tlvs[2] = { 0x27, 0x28 };
        for (int i = 0; i < 2; i++) {
            const uint8_t *dv = qmi_tlv_find(rsp, rlen, dns6_tlvs[i], &tlen);
            if (dv && tlen >= 16 && ipv6_is_nonzero(dv)) {
                char ip6[INET6_ADDRSTRLEN];
                if (inet_ntop(AF_INET6, dv, ip6, sizeof(ip6)))
                    data_add_str_uniq(c->dns, &c->n_dns, 4, ip6);
            }
        }
    } else {
        klog("WDS_GET_CURRENT_SETTINGS(ip=%u) rc=%d qerr=%u", ip_pref, qrc, qerr);
    }

    *out_fd = fd;
    *out_handle = handle;
    return 0;
}

static void wr_setup_data_call_result(AParcel *p, const struct data_call_state *c,
                                      int32_t cause, int64_t retry_ms) {
    bool up = (c && c->active && cause == 0);
    int32_t s = parcelable_begin(p);
    g_ndk.writeInt32(p, up ? 0 : cause);                  /* cause */
    g_ndk.writeInt64(p, up ? -1 : retry_ms);              /* suggestedRetryTime */
    g_ndk.writeInt32(p, up ? c->cid : -1);                /* cid */
    g_ndk.writeInt32(p, up ? 2 : 0);                      /* active (2=ACTIVE, 0=INACTIVE) */
    g_ndk.writeInt32(p, up ? c->pdp_type : -1);           /* type (PdpProtocolType) */
    wr_str(p, up ? c->ifname : "");                       /* ifname */

    /* LinkAddress[] addresses */
    int32_t n_addrs = up ? c->n_addrs : 0;
    g_ndk.writeInt32(p, n_addrs);
    for (int32_t i = 0; i < n_addrs; i++) {
        int32_t la_s = parcelable_begin(p);
        wr_str(p, c->addrs[i]);
        g_ndk.writeInt32(p, 0);   /* addressProperties */
        g_ndk.writeInt64(p, -1);  /* deprecationTime */
        g_ndk.writeInt64(p, -1);  /* expirationTime */
        parcelable_end(p, la_s);
    }

    /* String[] dnses */
    int32_t n_dns = up ? c->n_dns : 0;
    g_ndk.writeInt32(p, n_dns);
    for (int32_t i = 0; i < n_dns; i++)
        wr_str(p, c->dns[i]);

    /* String[] gateways */
    int32_t n_gws = up ? c->n_gws : 0;
    g_ndk.writeInt32(p, n_gws);
    for (int32_t i = 0; i < n_gws; i++)
        wr_str(p, c->gws[i]);

    /* String[] pcscf */
    g_ndk.writeInt32(p, 0);

    g_ndk.writeInt32(p, up ? c->mtu_v4 : 0);              /* mtuV4 */
    g_ndk.writeInt32(p, up ? c->mtu_v6 : 0);              /* mtuV6 */

    /* Qos defaultQos (non-null union: noinit=false) */
    g_ndk.writeInt32(p, 1);
    g_ndk.writeInt32(p, 0);
    g_ndk.writeBool(p, false);

    g_ndk.writeInt32(p, 0);                               /* QosSession[] qosSessions */
    g_ndk.writeByte(p, 0);                                /* handoverFailureMode */
    g_ndk.writeInt32(p, 0);                               /* pduSessionId */
    g_ndk.writeInt32(p, 0);                               /* sliceInfo (null) */
    g_ndk.writeInt32(p, 0);                               /* TrafficDescriptor[] */
    parcelable_end(p, s);
}

static void send_data_call_list_changed(const struct data_call_state *snap) {
    AIBinder *ind = get_binder(&g_dat_ind);
    if (!ind) return;
    AParcel *p = NULL;
    if (prep_tx(ind, &p, "dataCallListChanged")) {
        g_ndk.writeInt32(p, RADIO_IND_UNSOLICITED);
        if (snap && snap->active) {
            g_ndk.writeInt32(p, 1);
            wr_setup_data_call_result(p, snap, 0, -1);
        } else {
            g_ndk.writeInt32(p, 0);
        }
        send_oneway(ind, DAT_IND_DATA_CALL_LIST_CHANGED, p);
    }
    g_ndk.decStrong(ind);
}

static int32_t dat_on_transact(AIBinder *b, uint32_t code, const AParcel *in, AParcel *out) {
    (void)b;
    if (code == TX_GET_INTERFACE_VERSION || code == TX_GET_INTERFACE_HASH)
        return reply_version_or_hash("IRadioData", code, out, IDAT_HASH);

    if (code == DAT_TX_RESP_ACK)
        return STATUS_OK_VAL;

    if (code == DAT_TX_SET_RESPONSE_FUNCTIONS) {
        AIBinder *rsp = NULL, *ind = NULL;
        g_ndk.readStrongBinder(in, &rsp);
        g_ndk.readStrongBinder(in, &ind);
        replace_cb_binder(&g_dat_rsp, rsp, g_dat_rsp_cls);
        replace_cb_binder(&g_dat_ind, ind, g_dat_ind_cls);
        klog("IRadioData.setResponseFunctions");
        return STATUS_OK_VAL;
    }

    int32_t serial = 0;
    g_ndk.readInt32(in, &serial);
    AIBinder *cb = get_binder(&g_dat_rsp);

    switch (code) {
    case DAT_TX_SETUP_DATA_CALL: {
        int32_t access_net = 0;
        g_ndk.readInt32(in, &access_net);
        int32_t dp_non_null = 0;
        g_ndk.readInt32(in, &dp_non_null);
        int32_t profile_id = -1, protocol = 0, roaming_protocol = 0, auth_type = 0;
        char *apn = NULL, *user = NULL, *pass = NULL;
        if (dp_non_null) {
            int32_t pstart = g_ndk.getDataPosition(in);
            int32_t psize = 0;
            g_ndk.readInt32(in, &psize);
            g_ndk.readInt32(in, &profile_id);
            apn = rd_str_dup(in);
            g_ndk.readInt32(in, &protocol);
            g_ndk.readInt32(in, &roaming_protocol);
            g_ndk.readInt32(in, &auth_type);
            user = rd_str_dup(in);
            pass = rd_str_dup(in);
            if (psize >= 4)
                g_ndk.setDataPosition(in, pstart + psize);
        }
        bool roaming_allowed = true;
        int32_t reason = 0;
        g_ndk.readBool(in, &roaming_allowed);
        g_ndk.readInt32(in, &reason);
        (void)access_net;
        (void)roaming_allowed;
        (void)reason;

        int32_t req_proto = (protocol >= 0) ? protocol :
                            ((roaming_protocol >= 0) ? roaming_protocol : 0);
        klog("IRadioData.setupDataCall serial=%d profile=%d apn='%s' proto=%d auth=%d",
             serial, profile_id, apn ? apn : "", req_proto, auth_type);

        struct data_call_state snap;
        bool ok = false;
        pthread_mutex_lock(&g_data_mu);
        data_call_reset_locked();
        data_ensure_dpm_opened();
        data_ensure_wda_configured();

        if (net_ensure_rmnet_data("rmnet_ipa0", "rmnet_data0", 1) == 0) {
            g_data_call.cid = 1;
            snprintf(g_data_call.apn, sizeof(g_data_call.apn), "%s", apn ? apn : "");
            snprintf(g_data_call.ifname, sizeof(g_data_call.ifname), "rmnet_data0");

            if (req_proto == 1) {
                wds_start_family(&g_data_call, 6, apn, auth_type, user, pass,
                                 &g_data_call.wds_fd_v6, &g_data_call.handle_v6);
            } else if (req_proto == 2) {
                wds_start_family(&g_data_call, 4, apn, auth_type, user, pass,
                                 &g_data_call.wds_fd_v4, &g_data_call.handle_v4);
                wds_start_family(&g_data_call, 6, apn, auth_type, user, pass,
                                 &g_data_call.wds_fd_v6, &g_data_call.handle_v6);
            } else {
                wds_start_family(&g_data_call, 4, apn, auth_type, user, pass,
                                 &g_data_call.wds_fd_v4, &g_data_call.handle_v4);
            }

            if (g_data_call.wds_fd_v4 >= 0 || g_data_call.wds_fd_v6 >= 0) {
                g_data_call.active = true;
                if (g_data_call.has_v4 && g_data_call.has_v6)
                    g_data_call.pdp_type = 2;
                else if (g_data_call.has_v6 || (g_data_call.wds_fd_v6 >= 0 && !g_data_call.has_v4))
                    g_data_call.pdp_type = 1;
                else
                    g_data_call.pdp_type = 0;
                ok = true;
                klog("setupDataCall ACTIVE if=%s type=%d addrs=%d dns=%d gws=%d mtu4=%d mtu6=%d addr0='%s' gw0='%s' dns0='%s'",
                     g_data_call.ifname, g_data_call.pdp_type, g_data_call.n_addrs,
                     g_data_call.n_dns, g_data_call.n_gws,
                     g_data_call.mtu_v4, g_data_call.mtu_v6,
                     g_data_call.n_addrs ? g_data_call.addrs[0] : "",
                     g_data_call.n_gws ? g_data_call.gws[0] : "",
                     g_data_call.n_dns ? g_data_call.dns[0] : "");
            }
        }
        snap = g_data_call;
        pthread_mutex_unlock(&g_data_mu);

        free(apn);
        free(user);
        free(pass);

        if (cb) {
            AParcel *p = NULL;
            if (prep_tx(cb, &p, "setupDataCallResp")) {
                wr_resp_info(p, serial, RADIO_ERROR_NONE);
                wr_setup_data_call_result(p, ok ? &snap : NULL, ok ? 0 : 65535, ok ? -1 : 5000);
                send_oneway(cb, DAT_RSP_SETUP_DATA_CALL, p);
            }
        }
        if (ok)
            send_data_call_list_changed(&snap);
        break;
    }
    case DAT_TX_DEACTIVATE_DATA_CALL: {
        int32_t cid = 0, reason = 0;
        g_ndk.readInt32(in, &cid);
        g_ndk.readInt32(in, &reason);
        klog("IRadioData.deactivateDataCall serial=%d cid=%d reason=%d", serial, cid, reason);

        struct data_call_state snap;
        pthread_mutex_lock(&g_data_mu);
        data_call_reset_locked();
        snap = g_data_call;
        pthread_mutex_unlock(&g_data_mu);

        if (cb)
            send_simple_resp(cb, DAT_RSP_DEACTIVATE_DATA_CALL, serial, RADIO_ERROR_NONE,
                             "deactivateDataCallResp");
        send_data_call_list_changed(&snap);
        break;
    }
    case DAT_TX_GET_DATA_CALL_LIST: {
        if (!cb) break;
        struct data_call_state snap;
        pthread_mutex_lock(&g_data_mu);
        snap = g_data_call;
        pthread_mutex_unlock(&g_data_mu);

        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getDataCallListResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            if (snap.active) {
                g_ndk.writeInt32(p, 1);
                wr_setup_data_call_result(p, &snap, 0, -1);
            } else {
                g_ndk.writeInt32(p, 0);
            }
            send_oneway(cb, DAT_RSP_GET_DATA_CALL_LIST, p);
        }
        break;
    }
    case DAT_TX_SET_DATA_ALLOWED: {
        bool allowed = false;
        g_ndk.readBool(in, &allowed);
        klog("IRadioData.setDataAllowed serial=%d allowed=%d", serial, (int)allowed);
        if (cb) send_simple_resp(cb, DAT_RSP_SET_DATA_ALLOWED, serial, RADIO_ERROR_NONE, "setDataAllowedResp");
        break;
    }
    case DAT_TX_SET_DATA_PROFILE:
        klog("IRadioData.setDataProfile serial=%d", serial);
        if (cb) send_simple_resp(cb, DAT_RSP_SET_DATA_PROFILE, serial, RADIO_ERROR_NONE, "setDataProfileResp");
        break;
    case DAT_TX_SET_DATA_THROTTLING:
        if (cb) send_simple_resp(cb, DAT_RSP_SET_DATA_THROTTLING, serial, RADIO_ERROR_NONE, "setDataThrottlingResp");
        break;
    case DAT_TX_SET_INITIAL_ATTACH_APN:
        klog("IRadioData.setInitialAttachApn serial=%d", serial);
        if (cb) send_simple_resp(cb, DAT_RSP_SET_INITIAL_ATTACH_APN, serial, RADIO_ERROR_NONE, "setInitialAttachApnResp");
        break;
    case DAT_TX_ALLOCATE_PDU_SESSION_ID:
        if (cb) send_i32_resp(cb, DAT_RSP_ALLOCATE_PDU_SESSION_ID, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, 0, "allocatePduSessionIdResp");
        break;
    case DAT_TX_CANCEL_HANDOVER:
        if (cb) send_simple_resp(cb, DAT_RSP_CANCEL_HANDOVER, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "cancelHandoverResp");
        break;
    case DAT_TX_RELEASE_PDU_SESSION_ID:
        if (cb) send_simple_resp(cb, DAT_RSP_RELEASE_PDU_SESSION_ID, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "releasePduSessionIdResp");
        break;
    case DAT_TX_START_HANDOVER:
        if (cb) send_simple_resp(cb, DAT_RSP_START_HANDOVER, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "startHandoverResp");
        break;
    case DAT_TX_STOP_KEEPALIVE:
        if (cb) send_simple_resp(cb, DAT_RSP_STOP_KEEPALIVE, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "stopKeepaliveResp");
        break;
    case DAT_TX_GET_SLICING_CONFIG: {
        if (!cb) break;
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getSlicingConfigResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            int32_t sc = parcelable_begin(p);
            g_ndk.writeInt32(p, 0); /* urspRules[0] */
            g_ndk.writeInt32(p, 0); /* sliceInfo[0] */
            parcelable_end(p, sc);
            send_oneway(cb, DAT_RSP_GET_SLICING_CONFIG, p);
        }
        break;
    }
    case DAT_TX_START_KEEPALIVE: {
        if (!cb) break;
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "startKeepaliveResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            int32_t ks = parcelable_begin(p);
            g_ndk.writeInt32(p, 0); /* sessionHandle */
            g_ndk.writeInt32(p, 1); /* code = INACTIVE */
            parcelable_end(p, ks);
            send_oneway(cb, DAT_RSP_START_KEEPALIVE, p);
        }
        break;
    }
    case DAT_TX_SET_USER_DATA_ENABLED:
        if (cb) send_simple_resp(cb, DAT_RSP_SET_USER_DATA_ENABLED, serial, RADIO_ERROR_NONE, "setUserDataEnabledResp");
        break;
    case DAT_TX_SET_USER_DATA_ROAMING_ENABLED:
        if (cb) send_simple_resp(cb, DAT_RSP_SET_USER_DATA_ROAMING_ENABLED, serial, RADIO_ERROR_NONE, "setUserDataRoamingEnabledResp");
        break;
    case DAT_TX_NOTIFY_IMS_DATA_NETWORK:
        if (cb) send_simple_resp(cb, DAT_RSP_NOTIFY_IMS_DATA_NETWORK, serial, RADIO_ERROR_NONE, "notifyImsDataNetworkResp");
        break;
    default:
        klog("IRadioData unhandled tx=%u serial=%d", code, serial);
        break;
    }
    if (cb) g_ndk.decStrong(cb);
    return STATUS_OK_VAL;
}

/* ---- QMI VOICE & WMS helpers ---- */
static int32_t qmi_call_state_to_android(uint8_t st, bool is_mt) {
    switch (st) {
    case 1: return 2; /* ORIGINATION -> DIALING */
    case 2: return 4; /* INCOMING -> INCOMING */
    case 3: return 0; /* CONVERSATION -> ACTIVE */
    case 4: return 2; /* CC_IN_PROGRESS -> DIALING */
    case 5: return 3; /* ALERTING -> ALERTING */
    case 6: return 1; /* HOLD -> HOLDING */
    case 7: return 5; /* WAITING -> WAITING */
    case 10: return is_mt ? 4 : 2; /* SETUP */
    default: return -1; /* DISCONNECTING(8), END(9), UNKNOWN(0) */
    }
}

static int32_t qmi_pi_to_android(uint8_t pi) {
    switch (pi) {
    case 0: return 0; /* ALLOWED */
    case 1: return 1; /* RESTRICTED */
    case 4: return 3; /* PAYPHONE */
    default: return 2; /* UNKNOWN */
    }
}

static void voice_parse_calls(const uint8_t *ci, uint16_t ci_len,
                              const uint8_t *rn, uint16_t rn_len,
                              struct voice_call_info *out, int32_t *out_cnt) {
    int32_t n = 0;
    if (!ci || ci_len < 1) {
        *out_cnt = 0;
        return;
    }
    uint8_t cnt = ci[0];
    size_t pos = 1;
    for (uint8_t i = 0; i < cnt && pos + 7 <= ci_len && n < MAX_VOICE_CALLS; i++, pos += 7) {
        uint8_t call_id = ci[pos + 0];
        uint8_t state   = ci[pos + 1];
        /* uint8_t ctype = ci[pos + 2]; */
        uint8_t dir     = ci[pos + 3];
        /* uint8_t mode  = ci[pos + 4]; */
        uint8_t mpty    = ci[pos + 5];
        uint8_t als     = ci[pos + 6];
        bool is_mt = (dir == 2);
        int32_t ast = qmi_call_state_to_android(state, is_mt);
        if (ast < 0)
            continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].index = (int32_t)call_id;
        out[n].state = ast;
        out[n].is_mt = is_mt;
        out[n].is_mpty = (mpty != 0);
        out[n].als = (int8_t)als;
        out[n].num_pres = 2; /* UNKNOWN until matched */
        n++;
    }

    if (rn && rn_len >= 1) {
        uint8_t rcnt = rn[0];
        size_t rpos = 1;
        for (uint8_t i = 0; i < rcnt && rpos + 3 <= rn_len; i++) {
            uint8_t cid = rn[rpos + 0];
            uint8_t pi  = rn[rpos + 1];
            uint8_t len = rn[rpos + 2];
            rpos += 3;
            if (rpos + len > rn_len) break;
            for (int32_t j = 0; j < n; j++) {
                if (out[j].index == (int32_t)cid) {
                    size_t cpy = (len < sizeof(out[j].number) - 1) ? len : (sizeof(out[j].number) - 1);
                    memcpy(out[j].number, rn + rpos, cpy);
                    out[j].number[cpy] = '\0';
                    out[j].num_pres = qmi_pi_to_android(pi);
                    if (cpy > 0 && out[j].num_pres == 2)
                        out[j].num_pres = 0;
                    break;
                }
            }
            rpos += len;
        }
    }
    *out_cnt = n;
}

static void send_voice_call_state_changed(void) {
    send_unsol_empty_ind(&g_voc_ind, VOC_IND_CALL_STATE_CHANGED, "callStateChanged");
}

static void send_voice_call_ring(void) {
    AIBinder *ind = get_binder(&g_voc_ind);
    if (!ind) return;
    AParcel *p = NULL;
    if (prep_tx(ind, &p, "callRing")) {
        g_ndk.writeInt32(p, RADIO_IND_UNSOLICITED);
        g_ndk.writeBool(p, true); /* isGsm */
        int32_t s = parcelable_begin(p);
        g_ndk.writeBool(p, false); /* isPresent */
        g_ndk.writeByte(p, 0);     /* signalType */
        g_ndk.writeByte(p, 0);     /* alertPitch */
        g_ndk.writeByte(p, 0);     /* signal */
        parcelable_end(p, s);
        send_oneway(ind, VOC_IND_CALL_RING, p);
    }
    g_ndk.decStrong(ind);
}

static void send_msg_new_sms(const uint8_t *pdu, size_t len) {
    AIBinder *ind = get_binder(&g_msg_ind);
    if (!ind) return;

    /* Android expects 3GPP 27.005 PDU = [scLen, scAddr..., TPDU].
     * QMI WMS TRANSFER_ONLY delivers raw TPDU without the leading SMSC header
     * (e.g. byte 0 = 0x04 SMS-DELIVER). Prepend 0x00 if no SMSC header present. */
    uint8_t buf[512];
    const uint8_t *out_pdu = pdu;
    size_t out_len = len;
    bool has_smsc = (len >= 2 && pdu[0] >= 2 && pdu[0] <= 12 &&
                     (pdu[1] == 0x91 || pdu[1] == 0x81 || pdu[1] == 0xa1));
    if (!has_smsc && len < sizeof(buf)) {
        buf[0] = 0x00;
        memcpy(buf + 1, pdu, len);
        out_pdu = buf;
        out_len = len + 1;
    }

    AParcel *p = NULL;
    if (prep_tx(ind, &p, "newSms")) {
        g_ndk.writeInt32(p, RADIO_IND_UNSOLICITED);
        g_ndk.writeByteArray(p, (const int8_t *)out_pdu, (int32_t)out_len);
        send_oneway(ind, MSG_IND_NEW_SMS, p);
    }
    g_ndk.decStrong(ind);
}

static void wr_voice_call(AParcel *p, const struct voice_call_info *c) {
    int32_t s = parcelable_begin(p);
    g_ndk.writeInt32(p, c->state);
    g_ndk.writeInt32(p, c->index);
    g_ndk.writeInt32(p, (c->number[0] == '+') ? 145 : 129); /* toa */
    g_ndk.writeBool(p, c->is_mpty);
    g_ndk.writeBool(p, c->is_mt);
    g_ndk.writeByte(p, c->als);
    g_ndk.writeBool(p, true);  /* isVoice */
    g_ndk.writeBool(p, false); /* isVoicePrivacy */
    wr_str(p, c->number);
    g_ndk.writeInt32(p, c->num_pres);
    wr_str(p, "");             /* name */
    g_ndk.writeInt32(p, 2);    /* namePresentation = UNKNOWN */
    g_ndk.writeInt32(p, 0);    /* UusInfo[0] */
    g_ndk.writeInt32(p, 0);    /* audioQuality = UNSPECIFIED */
    wr_str(p, "");             /* forwardedNumber */
    parcelable_end(p, s);
}

static void voice_query_all_calls(struct voice_call_info *snap, int32_t *snap_cnt) {
    uint8_t rsp[1024];
    size_t rlen = 0;
    pthread_mutex_lock(&g_qmi_mu);
    int rc = qmi_call_locked(QMI_SVC_VOICE, &g_svc_voice, 0x002f,
                             NULL, 0, rsp, sizeof(rsp), &rlen);
    pthread_mutex_unlock(&g_qmi_mu);

    if (rc == 0) {
        uint16_t clen = 0, nlen = 0;
        const uint8_t *ci = qmi_tlv_find(rsp, rlen, 0x10, &clen);
        const uint8_t *rn = qmi_tlv_find(rsp, rlen, 0x11, &nlen);
        struct voice_call_info tmp[MAX_VOICE_CALLS];
        int32_t n = 0;
        voice_parse_calls(ci, clen, rn, nlen, tmp, &n);
        pthread_mutex_lock(&g_voice_mu);
        memcpy(g_calls, tmp, (size_t)n * sizeof(tmp[0]));
        g_num_calls = n;
        memcpy(snap, g_calls, (size_t)n * sizeof(tmp[0]));
        *snap_cnt = n;
        pthread_mutex_unlock(&g_voice_mu);
        return;
    }

    /* Fallback to cached calls from 0x002e indication */
    pthread_mutex_lock(&g_voice_mu);
    memcpy(snap, g_calls, (size_t)g_num_calls * sizeof(snap[0]));
    *snap_cnt = g_num_calls;
    pthread_mutex_unlock(&g_voice_mu);
}

static int voice_find_call_id(int32_t want_state1, int32_t want_state2) {
    int id = 1;
    pthread_mutex_lock(&g_voice_mu);
    for (int32_t i = 0; i < g_num_calls; i++) {
        if (g_calls[i].state == want_state1 || g_calls[i].state == want_state2) {
            id = g_calls[i].index;
            break;
        }
    }
    if (id == 1 && g_num_calls > 0)
        id = g_calls[0].index;
    pthread_mutex_unlock(&g_voice_mu);
    return id;
}

static size_t hex_decode_str(const char *hex, uint8_t *out, size_t cap) {
    if (!hex) return 0;
    size_t n = 0;
    while (hex[0] && hex[1] && n < cap) {
        int hi = hex_nibble(hex[0]);
        int lo = hex_nibble(hex[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

static int32_t voc_on_transact(AIBinder *b, uint32_t code, const AParcel *in, AParcel *out) {
    (void)b;
    if (code == TX_GET_INTERFACE_VERSION || code == TX_GET_INTERFACE_HASH)
        return reply_version_or_hash("IRadioVoice", code, out, IVOC_HASH);

    if (code == VOC_TX_RESP_ACK)
        return STATUS_OK_VAL;

    if (code == VOC_TX_SET_RESPONSE_FUNCTIONS) {
        AIBinder *rsp = NULL, *ind = NULL;
        g_ndk.readStrongBinder(in, &rsp);
        g_ndk.readStrongBinder(in, &ind);
        replace_cb_binder(&g_voc_rsp, rsp, g_voc_rsp_cls);
        replace_cb_binder(&g_voc_ind, ind, g_voc_ind_cls);
        klog("IRadioVoice.setResponseFunctions");
        return STATUS_OK_VAL;
    }

    int32_t serial = 0;
    g_ndk.readInt32(in, &serial);
    AIBinder *cb = get_binder(&g_voc_rsp);
    if (!cb) return STATUS_OK_VAL;

    switch (code) {
    case VOC_TX_GET_CURRENT_CALLS: {
        struct voice_call_info snap[MAX_VOICE_CALLS];
        int32_t n = 0;
        voice_query_all_calls(snap, &n);
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getCurrentCallsResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeInt32(p, n);
            for (int32_t i = 0; i < n; i++)
                wr_voice_call(p, &snap[i]);
            send_oneway(cb, VOC_RSP_GET_CURRENT_CALLS, p);
        }
        break;
    }
    case VOC_TX_DIAL:
    case VOC_TX_EMERGENCY_DIAL: {
        int32_t non_null = 0;
        g_ndk.readInt32(in, &non_null);
        char *addr = NULL;
        int32_t clir = 0;
        if (non_null) {
            int32_t pstart = g_ndk.getDataPosition(in);
            int32_t psize = 0;
            g_ndk.readInt32(in, &psize);
            addr = rd_str_dup(in);
            g_ndk.readInt32(in, &clir);
            if (psize >= 4)
                g_ndk.setDataPosition(in, pstart + psize);
        }
        (void)clir;
        bool is_emerg = (code == VOC_TX_EMERGENCY_DIAL);
        klog("IRadioVoice.%s serial=%d addr='%s'",
             is_emerg ? "emergencyDial" : "dial", serial, addr ? addr : "");

        int32_t rerr = RADIO_ERROR_GENERIC_FAILURE;
        if (addr && addr[0]) {
            uint8_t req[128], rsp[256];
            size_t off = 0, rlen = 0;
            size_t alen = strlen(addr);
            if (alen > 80) alen = 80;
            off = qmi_tlv_append(req, off, sizeof(req), 0x01, addr, (uint16_t)alen);
            uint8_t ctype = is_emerg ? 0x09 : 0x00;
            off = qmi_tlv_append(req, off, sizeof(req), 0x10, &ctype, 1);
            pthread_mutex_lock(&g_qmi_mu);
            int rc = qmi_call_locked(QMI_SVC_VOICE, &g_svc_voice, 0x0020,
                                     req, (uint16_t)off, rsp, sizeof(rsp), &rlen);
            pthread_mutex_unlock(&g_qmi_mu);
            klog("VOICE_DIAL_CALL rc=%d", rc);
            if (rc == 0) rerr = RADIO_ERROR_NONE;
        }
        free(addr);
        send_simple_resp(cb, is_emerg ? VOC_RSP_EMERGENCY_DIAL : VOC_RSP_DIAL,
                         serial, rerr, is_emerg ? "emergencyDialResp" : "dialResp");
        if (rerr == RADIO_ERROR_NONE)
            send_voice_call_state_changed();
        break;
    }
    case VOC_TX_ACCEPT_CALL: {
        uint8_t cid = (uint8_t)voice_find_call_id(4, 5);
        uint8_t req[16], rsp[128];
        size_t off = 0, rlen = 0;
        off = qmi_tlv_append(req, off, sizeof(req), 0x01, &cid, 1);
        pthread_mutex_lock(&g_qmi_mu);
        int rc = qmi_call_locked(QMI_SVC_VOICE, &g_svc_voice, 0x0022,
                                 req, (uint16_t)off, rsp, sizeof(rsp), &rlen);
        pthread_mutex_unlock(&g_qmi_mu);
        klog("IRadioVoice.acceptCall serial=%d cid=%u rc=%d", serial, cid, rc);
        send_simple_resp(cb, VOC_RSP_ACCEPT_CALL, serial,
                         rc == 0 ? RADIO_ERROR_NONE : RADIO_ERROR_GENERIC_FAILURE,
                         "acceptCallResp");
        if (rc == 0) send_voice_call_state_changed();
        break;
    }
    case VOC_TX_HANGUP: {
        int32_t gsm_idx = 1;
        g_ndk.readInt32(in, &gsm_idx);
        uint8_t cid = (uint8_t)(gsm_idx > 0 ? gsm_idx : 1);
        uint8_t req[16], rsp[128];
        size_t off = 0, rlen = 0;
        off = qmi_tlv_append(req, off, sizeof(req), 0x01, &cid, 1);
        pthread_mutex_lock(&g_qmi_mu);
        int rc = qmi_call_locked(QMI_SVC_VOICE, &g_svc_voice, 0x0021,
                                 req, (uint16_t)off, rsp, sizeof(rsp), &rlen);
        pthread_mutex_unlock(&g_qmi_mu);
        klog("IRadioVoice.hangup serial=%d cid=%u rc=%d", serial, cid, rc);
        send_simple_resp(cb, VOC_RSP_HANGUP, serial, RADIO_ERROR_NONE, "hangupResp");
        send_voice_call_state_changed();
        break;
    }
    case VOC_TX_REJECT_CALL:
    case VOC_TX_HANGUP_WAITING_OR_BG: {
        uint8_t cid = (uint8_t)voice_find_call_id(4, 5);
        uint8_t req[16], rsp[128];
        size_t off = 0, rlen = 0;
        off = qmi_tlv_append(req, off, sizeof(req), 0x01, &cid, 1);
        pthread_mutex_lock(&g_qmi_mu);
        int rc = qmi_call_locked(QMI_SVC_VOICE, &g_svc_voice, 0x0021,
                                 req, (uint16_t)off, rsp, sizeof(rsp), &rlen);
        pthread_mutex_unlock(&g_qmi_mu);
        klog("IRadioVoice.reject/hangupBg serial=%d cid=%u rc=%d", serial, cid, rc);
        send_simple_resp(cb, code == VOC_TX_REJECT_CALL ? VOC_RSP_REJECT_CALL : VOC_RSP_HANGUP_WAITING_OR_BG,
                         serial, RADIO_ERROR_NONE, "rejectCallResp");
        send_voice_call_state_changed();
        break;
    }
    case VOC_TX_HANGUP_FG_RESUME_BG:
    case VOC_TX_SWITCH_HOLDING_ACTIVE:
    case VOC_TX_CONFERENCE: {
        uint8_t sups = (code == VOC_TX_HANGUP_FG_RESUME_BG) ? 2 :
                       (code == VOC_TX_SWITCH_HOLDING_ACTIVE) ? 3 : 4;
        uint32_t rsp_code = (code == VOC_TX_HANGUP_FG_RESUME_BG) ? VOC_RSP_HANGUP_FG_RESUME_BG :
                            (code == VOC_TX_SWITCH_HOLDING_ACTIVE) ? VOC_RSP_SWITCH_HOLDING_ACTIVE :
                            VOC_RSP_CONFERENCE;
        uint8_t req[16], rsp[128];
        size_t off = 0, rlen = 0;
        off = qmi_tlv_append(req, off, sizeof(req), 0x01, &sups, 1);
        pthread_mutex_lock(&g_qmi_mu);
        int rc = qmi_call_locked(QMI_SVC_VOICE, &g_svc_voice, 0x0031,
                                 req, (uint16_t)off, rsp, sizeof(rsp), &rlen);
        pthread_mutex_unlock(&g_qmi_mu);
        klog("IRadioVoice.manageCalls(sups=%u) serial=%d rc=%d", sups, serial, rc);
        send_simple_resp(cb, rsp_code, serial,
                         rc == 0 ? RADIO_ERROR_NONE : RADIO_ERROR_GENERIC_FAILURE,
                         "manageCallsResp");
        send_voice_call_state_changed();
        break;
    }
    case VOC_TX_SEND_DTMF:
    case VOC_TX_START_DTMF: {
        char *s = rd_str_dup(in);
        if (s && s[0]) {
            uint8_t dtmf[3] = { (uint8_t)voice_find_call_id(0, 3), 1, (uint8_t)s[0] };
            uint8_t req[16], rsp[128];
            size_t off = 0, rlen = 0;
            off = qmi_tlv_append(req, off, sizeof(req), 0x01, dtmf, sizeof(dtmf));
            pthread_mutex_lock(&g_qmi_mu);
            (void)qmi_call_locked(QMI_SVC_VOICE, &g_svc_voice, 0x0028,
                                  req, (uint16_t)off, rsp, sizeof(rsp), &rlen);
            pthread_mutex_unlock(&g_qmi_mu);
        }
        free(s);
        send_simple_resp(cb, code == VOC_TX_SEND_DTMF ? VOC_RSP_SEND_DTMF : VOC_RSP_START_DTMF,
                         serial, RADIO_ERROR_NONE, "dtmfResp");
        break;
    }
    case VOC_TX_STOP_DTMF:
        send_simple_resp(cb, VOC_RSP_STOP_DTMF, serial, RADIO_ERROR_NONE, "stopDtmfResp");
        break;
    case VOC_TX_GET_LAST_CALL_FAIL_CAUSE: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getLastCallFailCauseResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            int32_t s = parcelable_begin(p);
            g_ndk.writeInt32(p, g_last_call_fail_cause);
            wr_str(p, "");
            parcelable_end(p, s);
            send_oneway(cb, VOC_RSP_GET_LAST_CALL_FAIL_CAUSE, p);
        }
        break;
    }
    case VOC_TX_GET_MUTE: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getMuteResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeBool(p, g_voice_mute);
            send_oneway(cb, VOC_RSP_GET_MUTE, p);
        }
        break;
    }
    case VOC_TX_GET_TTY_MODE: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getTtyModeResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            g_ndk.writeInt32(p, 0);
            send_oneway(cb, VOC_RSP_GET_TTY_MODE, p);
        }
        break;
    }
    case VOC_TX_GET_CALL_FWD:
        send_empty_array_resp(cb, VOC_RSP_GET_CALL_FWD, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "getCallForwardStatusResp");
        break;
    case VOC_TX_GET_CALL_WAITING: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getCallWaitingResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            g_ndk.writeBool(p, false);
            g_ndk.writeInt32(p, 0);
            send_oneway(cb, VOC_RSP_GET_CALL_WAITING, p);
        }
        break;
    }
    case VOC_TX_GET_CLIP:
        send_i32_resp(cb, VOC_RSP_GET_CLIP, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, 2, "getClipResp");
        break;
    case VOC_TX_GET_CLIR: {
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getClirResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            g_ndk.writeInt32(p, 0);
            g_ndk.writeInt32(p, 0);
            send_oneway(cb, VOC_RSP_GET_CLIR, p);
        }
        break;
    }
    case VOC_TX_GET_PREF_PRIVACY:
        send_bool_resp(cb, VOC_RSP_GET_PREF_PRIVACY, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, false, "getPreferredVoicePrivacyResp");
        break;
    case VOC_TX_IS_VONR_ENABLED:
        send_bool_resp(cb, VOC_RSP_IS_VONR_ENABLED, serial, RADIO_ERROR_NONE, false, "isVoNrEnabledResp");
        break;
    case VOC_TX_SET_MUTE: {
        bool en = false;
        g_ndk.readBool(in, &en);
        g_voice_mute = en;
        send_simple_resp(cb, VOC_RSP_SET_MUTE, serial, RADIO_ERROR_NONE, "setMuteResp");
        break;
    }
    case VOC_TX_SET_PREF_PRIVACY:
        send_simple_resp(cb, VOC_RSP_SET_PREF_PRIVACY, serial, RADIO_ERROR_NONE, "setPreferredVoicePrivacyResp");
        break;
    case VOC_TX_SET_TTY_MODE:
        send_simple_resp(cb, VOC_RSP_SET_TTY_MODE, serial, RADIO_ERROR_NONE, "setTtyModeResp");
        break;
    case VOC_TX_SET_CLIR:
        send_simple_resp(cb, VOC_RSP_SET_CLIR, serial, RADIO_ERROR_NONE, "setClirResp");
        break;
    case VOC_TX_SET_VONR_ENABLED:
        send_simple_resp(cb, VOC_RSP_SET_VONR_ENABLED, serial, RADIO_ERROR_NONE, "setVoNrEnabledResp");
        break;
    default:
        klog("IRadioVoice unhandled tx=%u serial=%d", code, serial);
        break;
    }
    g_ndk.decStrong(cb);
    return STATUS_OK_VAL;
}

static int32_t msg_on_transact(AIBinder *b, uint32_t code, const AParcel *in, AParcel *out) {
    (void)b;
    if (code == TX_GET_INTERFACE_VERSION || code == TX_GET_INTERFACE_HASH)
        return reply_version_or_hash("IRadioMessaging", code, out, IMSG_HASH);

    if (code == MSG_TX_RESP_ACK)
        return STATUS_OK_VAL;

    if (code == MSG_TX_SET_RESPONSE_FUNCTIONS) {
        AIBinder *rsp = NULL, *ind = NULL;
        g_ndk.readStrongBinder(in, &rsp);
        g_ndk.readStrongBinder(in, &ind);
        replace_cb_binder(&g_msg_rsp, rsp, g_msg_rsp_cls);
        replace_cb_binder(&g_msg_ind, ind, g_msg_ind_cls);
        klog("IRadioMessaging.setResponseFunctions");
        return STATUS_OK_VAL;
    }

    int32_t serial = 0;
    g_ndk.readInt32(in, &serial);
    AIBinder *cb = get_binder(&g_msg_rsp);
    if (!cb) return STATUS_OK_VAL;

    switch (code) {
    case MSG_TX_ACK_INCOMING_GSM_PDU:
    case MSG_TX_ACK_LAST_GSM: {
        bool success = true;
        g_ndk.readBool(in, &success);
        uint32_t txn_id = 0;
        bool need_ack = false;
        pthread_mutex_lock(&g_sms_mu);
        txn_id = g_last_sms_txn_id;
        need_ack = g_last_sms_need_ack;
        g_last_sms_need_ack = false;
        pthread_mutex_unlock(&g_sms_mu);
        if (need_ack) {
            uint8_t ack[6], req[24], rsp[128];
            size_t off = 0, rlen = 0;
            qmi_wr32(ack, txn_id);
            ack[4] = 1; /* WCDMA/GSM */
            ack[5] = success ? 1 : 0;
            off = qmi_tlv_append(req, off, sizeof(req), 0x01, ack, sizeof(ack));
            pthread_mutex_lock(&g_qmi_mu);
            int rc = qmi_call_locked(QMI_SVC_WMS, &g_svc_wms, 0x0037,
                                     req, (uint16_t)off, rsp, sizeof(rsp), &rlen);
            pthread_mutex_unlock(&g_qmi_mu);
            klog("WMS_SEND_ACK(txn=%u ok=%d) rc=%d", txn_id, (int)success, rc);
        }
        send_simple_resp(cb, code == MSG_TX_ACK_INCOMING_GSM_PDU ?
                             MSG_RSP_ACK_INCOMING_GSM_PDU : MSG_RSP_ACK_LAST_GSM,
                         serial, RADIO_ERROR_NONE, "ackGsmSmsResp");
        break;
    }
    case MSG_TX_ACK_LAST_CDMA:
        send_simple_resp(cb, MSG_RSP_ACK_LAST_CDMA, serial, RADIO_ERROR_NONE, "ackLastIncomingCdmaSmsResp");
        break;
    case MSG_TX_DELETE_SMS_RUIM:
        send_simple_resp(cb, MSG_RSP_DELETE_SMS_RUIM, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "deleteSmsOnRuimResp");
        break;
    case MSG_TX_DELETE_SMS_SIM:
        send_simple_resp(cb, MSG_RSP_DELETE_SMS_SIM, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "deleteSmsOnSimResp");
        break;
    case MSG_TX_GET_CDMA_BCAST_CFG:
        send_empty_array_resp(cb, MSG_RSP_GET_CDMA_BCAST_CFG, serial, RADIO_ERROR_NONE, "getCdmaBroadcastConfigResp");
        break;
    case MSG_TX_GET_GSM_BCAST_CFG:
        send_empty_array_resp(cb, MSG_RSP_GET_GSM_BCAST_CFG, serial, RADIO_ERROR_NONE, "getGsmBroadcastConfigResp");
        break;
    case MSG_TX_GET_SMSC_ADDRESS: {
        char smsc[64];
        pthread_mutex_lock(&g_sms_mu);
        snprintf(smsc, sizeof(smsc), "%s", g_smsc_addr);
        pthread_mutex_unlock(&g_sms_mu);
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "getSmscAddressResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_NONE);
            wr_str(p, smsc);
            send_oneway(cb, MSG_RSP_GET_SMSC_ADDRESS, p);
        }
        break;
    }
    case MSG_TX_REPORT_SMS_MEM:
        send_simple_resp(cb, MSG_RSP_REPORT_SMS_MEM, serial, RADIO_ERROR_NONE, "reportSmsMemoryStatusResp");
        break;
    case MSG_TX_SEND_SMS:
    case MSG_TX_SEND_SMS_MORE: {
        uint32_t rsp_code = (code == MSG_TX_SEND_SMS) ? MSG_RSP_SEND_SMS : MSG_RSP_SEND_SMS_MORE;
        int32_t non_null = 0;
        g_ndk.readInt32(in, &non_null);
        char *smsc_hex = NULL, *pdu_hex = NULL;
        if (non_null) {
            int32_t pstart = g_ndk.getDataPosition(in);
            int32_t psize = 0;
            g_ndk.readInt32(in, &psize);
            smsc_hex = rd_str_dup(in);
            pdu_hex = rd_str_dup(in);
            if (psize >= 4)
                g_ndk.setDataPosition(in, pstart + psize);
        }

        uint8_t raw[3 + 256];
        size_t plen = 0;
        raw[0] = 6; /* GSM/WCDMA point-to-point */
        if (smsc_hex && smsc_hex[0]) {
            plen += hex_decode_str(smsc_hex, raw + 3 + plen, 256 - plen);
        } else {
            raw[3 + plen++] = 0x00; /* use default SMSC */
        }
        if (pdu_hex && pdu_hex[0])
            plen += hex_decode_str(pdu_hex, raw + 3 + plen, 256 - plen);
        qmi_wr16(raw + 1, (uint16_t)plen);

        int32_t rerr = RADIO_ERROR_GENERIC_FAILURE;
        int32_t msg_ref = 0;
        int32_t err_code = -1;
        if (plen > 1) {
            uint8_t req[300], rsp[256];
            size_t off = 0, rlen = 0;
            off = qmi_tlv_append(req, off, sizeof(req), 0x01, raw, (uint16_t)(3 + plen));
            pthread_mutex_lock(&g_qmi_mu);
            int rc = qmi_call_locked(QMI_SVC_WMS, &g_svc_wms, 0x0020,
                                     req, (uint16_t)off, rsp, sizeof(rsp), &rlen);
            pthread_mutex_unlock(&g_qmi_mu);
            if (rc == 0) {
                uint16_t ml = 0;
                const uint8_t *mv = qmi_tlv_find(rsp, rlen, 0x01, &ml);
                if (mv && ml >= 2) msg_ref = (int32_t)qmi_rd16(mv);
                rerr = RADIO_ERROR_NONE;
            }
            klog("WMS_RAW_SEND(len=%zu) rc=%d ref=%d", plen, rc, msg_ref);
        }
        free(smsc_hex);
        free(pdu_hex);

        AParcel *p = NULL;
        if (prep_tx(cb, &p, "sendSmsResp")) {
            wr_resp_info(p, serial, rerr);
            int32_t s = parcelable_begin(p);
            g_ndk.writeInt32(p, msg_ref);
            wr_str(p, "");
            g_ndk.writeInt32(p, err_code);
            parcelable_end(p, s);
            send_oneway(cb, rsp_code, p);
        }
        break;
    }
    case MSG_TX_SEND_CDMA_SMS:
    case MSG_TX_SEND_CDMA_SMS_MORE:
    case MSG_TX_SEND_IMS_SMS: {
        uint32_t rsp_code = (code == MSG_TX_SEND_CDMA_SMS) ? MSG_RSP_SEND_CDMA_SMS :
                            (code == MSG_TX_SEND_CDMA_SMS_MORE) ? MSG_RSP_SEND_CDMA_SMS_MORE :
                            MSG_RSP_SEND_IMS_SMS;
        AParcel *p = NULL;
        if (prep_tx(cb, &p, "sendSmsErrResp")) {
            wr_resp_info(p, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED);
            int32_t s = parcelable_begin(p);
            g_ndk.writeInt32(p, 0);  /* messageRef */
            wr_str(p, "");           /* ackPDU */
            g_ndk.writeInt32(p, -1); /* errorCode */
            parcelable_end(p, s);
            send_oneway(cb, rsp_code, p);
        }
        break;
    }
    case MSG_TX_SET_CDMA_BCAST_ACT:
        send_simple_resp(cb, MSG_RSP_SET_CDMA_BCAST_ACT, serial, RADIO_ERROR_NONE, "setCdmaBroadcastActivationResp");
        break;
    case MSG_TX_SET_CDMA_BCAST_CFG:
        send_simple_resp(cb, MSG_RSP_SET_CDMA_BCAST_CFG, serial, RADIO_ERROR_NONE, "setCdmaBroadcastConfigResp");
        break;
    case MSG_TX_SET_GSM_BCAST_ACT:
        send_simple_resp(cb, MSG_RSP_SET_GSM_BCAST_ACT, serial, RADIO_ERROR_NONE, "setGsmBroadcastActivationResp");
        break;
    case MSG_TX_SET_GSM_BCAST_CFG:
        klog("IRadioMessaging.setGsmBroadcastConfig serial=%d", serial);
        send_simple_resp(cb, MSG_RSP_SET_GSM_BCAST_CFG, serial, RADIO_ERROR_NONE, "setGsmBroadcastConfigResp");
        break;
    case MSG_TX_SET_SMSC_ADDRESS:
        send_simple_resp(cb, MSG_RSP_SET_SMSC_ADDRESS, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, "setSmscAddressResp");
        break;
    case MSG_TX_WRITE_SMS_RUIM:
        send_i32_resp(cb, MSG_RSP_WRITE_SMS_RUIM, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, -1, "writeSmsToRuimResp");
        break;
    case MSG_TX_WRITE_SMS_SIM:
        send_i32_resp(cb, MSG_RSP_WRITE_SMS_SIM, serial, RADIO_ERROR_REQUEST_NOT_SUPPORTED, -1, "writeSmsToSimResp");
        break;
    default:
        klog("IRadioMessaging unhandled tx=%u serial=%d", code, serial);
        break;
    }
    g_ndk.decStrong(cb);
    return STATUS_OK_VAL;
}

static AIBinder_Class *define_cb_class(const char *desc) {
    return g_ndk.Class_define(desc, noop_create, noop_destroy, noop_transact);
}

static bool register_svc(const char *desc, const char *inst, AIBinder_Class_onTransact fn) {
    AIBinder_Class *cls = g_ndk.Class_define(desc, noop_create, noop_destroy, fn);
    if (!cls) return false;
    AIBinder *b = g_ndk.AIBinder_new(cls, NULL);
    if (!b) return false;
    g_ndk.markVintfStability(b);
    int32_t rc = g_ndk.addService(b, inst);
    if (rc != STATUS_OK_VAL) {
        klog("addService(%s) failed rc=%d", inst, rc);
        g_ndk.decStrong(b);
        return false;
    }
    klog("registered %s", inst);
    return true;
}

static void *qmi_ind_thread(void *arg) {
    (void)arg;
    while (!g_qmi_ready)
        usleep(250000);

    uint32_t local_node = 0;
    int fd = qmi_open_socket(&local_node);
    if (fd < 0) {
        klog("qmi_ind_thread: failed to open QRTR socket");
        return NULL;
    }

    struct qmi_service_addr svc_voice, svc_wms;
    memset(&svc_voice, 0, sizeof(svc_voice));
    memset(&svc_wms, 0, sizeof(svc_wms));
    uint16_t txn = 1;
    uint8_t req[64], rsp[512];
    size_t rlen = 0;

    for (int i = 0; i < 20 && (!svc_voice.found || !svc_wms.found); i++) {
        if (!svc_voice.found)
            (void)qmi_lookup_service(fd, local_node, QMI_SVC_VOICE, &svc_voice, 1000);
        if (!svc_wms.found)
            (void)qmi_lookup_service(fd, local_node, QMI_SVC_WMS, &svc_wms, 1000);
        if (!svc_voice.found || !svc_wms.found)
            usleep(500000);
    }

    if (svc_voice.found) {
        size_t off = 0;
        uint8_t one = 1;
        off = qmi_tlv_append(req, off, sizeof(req), 0x10, &one, 1); /* dtmf */
        off = qmi_tlv_append(req, off, sizeof(req), 0x12, &one, 1); /* sups */
        int rc = qmi_transact(fd, &svc_voice, txn++, 0x0003, req, (uint16_t)off,
                              rsp, sizeof(rsp), &rlen, 2000);
        /* Also issue GET_ALL_CALL_INFO so VOICE binds this client for 0x002e inds */
        int rc2 = qmi_transact(fd, &svc_voice, txn++, 0x002f, NULL, 0,
                               rsp, sizeof(rsp), &rlen, 2000);
        klog("VOICE ind client ready svc=%u:%u v%u reg=%d info=%d",
             svc_voice.node, svc_voice.port, svc_voice.version, rc, rc2);
    }

    if (svc_wms.found) {
        /* Route GSM/WCDMA MT SMS (classes 0..4) as TRANSFER_ONLY (2) to this client */
        uint8_t routes[2 + 5 * 4];
        qmi_wr16(routes, 5);
        for (uint8_t c = 0; c < 5; c++) {
            routes[2 + c * 4 + 0] = 0;    /* POINT_TO_POINT */
            routes[2 + c * 4 + 1] = c;    /* CLASS_0..3, CLASS_NONE(4) */
            routes[2 + c * 4 + 2] = 0xff; /* STORAGE_TYPE_NONE */
            routes[2 + c * 4 + 3] = 2;    /* TRANSFER_ONLY */
        }
        size_t off = 0;
        uint8_t one = 1;
        off = qmi_tlv_append(req, off, sizeof(req), 0x01, routes, sizeof(routes));
        off = qmi_tlv_append(req, off, sizeof(req), 0x10, &one, 1);
        int rc_rt = qmi_transact(fd, &svc_wms, txn++, 0x0032, req, (uint16_t)off,
                                 rsp, sizeof(rsp), &rlen, 2000);

        off = 0;
        off = qmi_tlv_append(req, off, sizeof(req), 0x10, &one, 1);
        int rc_ev = qmi_transact(fd, &svc_wms, txn++, 0x0001, req, (uint16_t)off,
                                 rsp, sizeof(rsp), &rlen, 2000);

        int rc_sc = qmi_transact(fd, &svc_wms, txn++, 0x0034, NULL, 0,
                                 rsp, sizeof(rsp), &rlen, 2000);
        if (rc_sc == 0) {
            uint16_t tl = 0;
            const uint8_t *v = qmi_tlv_find(rsp, rlen, 0x01, &tl);
            if (v && tl >= 2) {
                uint8_t tlen = v[0];
                if (1u + tlen + 1u <= tl) {
                    uint8_t dlen = v[1 + tlen];
                    if (1u + tlen + 1u + dlen <= tl) {
                        pthread_mutex_lock(&g_sms_mu);
                        size_t cpy = (dlen < sizeof(g_smsc_addr) - 1) ? dlen : (sizeof(g_smsc_addr) - 1);
                        memcpy(g_smsc_addr, v + 2 + tlen, cpy);
                        g_smsc_addr[cpy] = '\0';
                        pthread_mutex_unlock(&g_sms_mu);
                    }
                }
            }
        }
        klog("WMS ind client ready svc=%u:%u v%u routes=%d ev=%d smsc='%s'",
             svc_wms.node, svc_wms.port, svc_wms.version, rc_rt, rc_ev, g_smsc_addr);
    }

    bool was_ringing = false;
    uint8_t buf[2048];
    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 5000);
        if (pr <= 0) continue;
        struct sockaddr_qrtr from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n < (ssize_t)sizeof(struct qmi_hdr)) continue;
        if (from.sq_port == QRTR_PORT_CTRL) continue;

        const struct qmi_hdr *h = (const struct qmi_hdr *)(const void *)buf;
        if (h->type != QMI_TYPE_IND) continue;
        uint16_t msg_id = h->msg_id;
        const uint8_t *payload = buf + sizeof(struct qmi_hdr);
        size_t plen = (size_t)n - sizeof(struct qmi_hdr);

        if (svc_voice.found && from.sq_node == svc_voice.node && from.sq_port == svc_voice.port) {
            if (msg_id == 0x002e) {
                uint16_t clen = 0, nlen = 0;
                const uint8_t *ci = qmi_tlv_find(payload, plen, 0x01, &clen);
                const uint8_t *rn = qmi_tlv_find(payload, plen, 0x10, &nlen);
                struct voice_call_info tmp[MAX_VOICE_CALLS];
                int32_t cnt = 0;
                voice_parse_calls(ci, clen, rn, nlen, tmp, &cnt);

                bool ringing = false;
                for (int32_t i = 0; i < cnt; i++) {
                    if (tmp[i].state == 4 || tmp[i].state == 5)
                        ringing = true;
                }
                pthread_mutex_lock(&g_voice_mu);
                memcpy(g_calls, tmp, (size_t)cnt * sizeof(tmp[0]));
                g_num_calls = cnt;
                pthread_mutex_unlock(&g_voice_mu);

                klog("VOICE ind(0x2e): calls=%d c0(id=%d st=%d mt=%d num='%s')",
                     cnt, cnt ? tmp[0].index : 0, cnt ? tmp[0].state : -1,
                     cnt ? (int)tmp[0].is_mt : 0, cnt ? tmp[0].number : "");
                send_voice_call_state_changed();
                if (ringing && !was_ringing)
                    send_voice_call_ring();
                was_ringing = ringing;
            }
        } else if (svc_wms.found && from.sq_node == svc_wms.node && from.sq_port == svc_wms.port) {
            if (msg_id == 0x0001) {
                uint16_t tl = 0;
                const uint8_t *tr = qmi_tlv_find(payload, plen, 0x11, &tl);
                if (tr && tl >= 8) {
                    uint8_t ack_ind = tr[0];
                    uint32_t txn_id = qmi_rd32(tr + 1);
                    uint8_t fmt = tr[5];
                    uint16_t dlen = qmi_rd16(tr + 6);
                    if (8u + dlen <= tl && dlen > 0) {
                        pthread_mutex_lock(&g_sms_mu);
                        g_last_sms_txn_id = txn_id;
                        g_last_sms_need_ack = (ack_ind == 0);
                        pthread_mutex_unlock(&g_sms_mu);
                        klog("WMS ind: newSms(transfer) fmt=%u len=%u txn=%u ack_ind=%u",
                             fmt, dlen, txn_id, ack_ind);
                        send_msg_new_sms(tr + 8, dlen);
                    }
                } else {
                    const uint8_t *mt = qmi_tlv_find(payload, plen, 0x10, &tl);
                    if (mt && tl >= 5) {
                        uint8_t storage = mt[0];
                        uint32_t idx = qmi_rd32(mt + 1);
                        klog("WMS ind: mt_message storage=%u idx=%u", storage, idx);
                        uint8_t rreq[16], rrsp[512];
                        uint8_t mid[5];
                        mid[0] = storage;
                        qmi_wr32(mid + 1, idx);
                        size_t off = qmi_tlv_append(rreq, 0, sizeof(rreq), 0x01, mid, sizeof(mid));
                        size_t rrlen = 0;
                        if (qmi_transact(fd, &svc_wms, txn++, 0x0022, rreq, (uint16_t)off,
                                         rrsp, sizeof(rrsp), &rrlen, 2000) == 0) {
                            uint16_t rl = 0;
                            const uint8_t *rv = qmi_tlv_find(rrsp, rrlen, 0x01, &rl);
                            if (rv && rl >= 4) {
                                uint16_t dlen = qmi_rd16(rv + 2);
                                if (4u + dlen <= rl && dlen > 0) {
                                    klog("WMS raw_read idx=%u len=%u", idx, dlen);
                                    send_msg_new_sms(rv + 4, dlen);
                                }
                            }
                            /* Delete from modem storage so it does not fill up */
                            off = qmi_tlv_append(rreq, 0, sizeof(rreq), 0x01, &storage, 1);
                            uint8_t ibuf[4];
                            qmi_wr32(ibuf, idx);
                            off = qmi_tlv_append(rreq, off, sizeof(rreq), 0x10, ibuf, 4);
                            (void)qmi_transact(fd, &svc_wms, txn++, 0x0024, rreq, (uint16_t)off,
                                               rrsp, sizeof(rrsp), &rrlen, 2000);
                        }
                    }
                }
            }
        }
    }
    return NULL;
}

static void *qmi_init_thread(void *arg) {
    (void)arg;
    bool qmi_ready = false;
    for (int i = 0; i < 60; i++) {
        pthread_mutex_lock(&g_qmi_mu);
        bool ok = qmi_ensure_svc_locked(QMI_SVC_DMS, &g_svc_dms) &&
                  qmi_ensure_svc_locked(QMI_SVC_UIM, &g_svc_uim) &&
                  qmi_ensure_svc_locked(QMI_SVC_NAS, &g_svc_nas);
        if (ok) qmi_refresh_identity_locked();
        pthread_mutex_unlock(&g_qmi_mu);
        if (ok) {
            qmi_ready = true;
            g_qmi_ready = true;
            klog("QMI ready: bb=%s imei=%s iccid=%s imsi=%s",
                 g_baseband, g_imei, g_iccid, g_imsi);
            send_radio_state_ind(g_radio_state);
            AIBinder *sind = get_binder(&g_sim_ind);
            if (sind) {
                g_ndk.decStrong(sind);
                g_sim_notified = true;
                send_unsol_empty_ind(&g_sim_ind, SIM_IND_SIM_STATUS_CHANGED, "simStatusChanged");
            }
            send_unsol_empty_ind(&g_net_ind, NET_IND_NETWORK_STATE_CHANGED, "networkStateChanged");
            break;
        }
        usleep(500000);
    }
    if (!qmi_ready) {
        klog("QMI init timed out waiting for DMS/UIM/NAS");
        return NULL;
    }

    int32_t last_reg = -1;
    int32_t last_rat = -1;
    char last_plmn[16] = "";
    uint32_t last_cid = 0;
    int32_t last_rsrp = INT32_MAX_VAL;

    for (;;) {
        sleep(3);
        if (!g_sim_notified) {
            AIBinder *sind = get_binder(&g_sim_ind);
            if (sind) {
                g_ndk.decStrong(sind);
                g_sim_notified = true;
                send_unsol_empty_ind(&g_sim_ind, SIM_IND_SIM_STATUS_CHANGED, "simStatusChanged");
            }
        }
        AIBinder *nind = get_binder(&g_net_ind);
        if (!nind) continue;
        g_ndk.decStrong(nind);
        if (g_radio_state != RADIO_STATE_ON) continue;

        struct nas_snapshot ns;
        qmi_query_nas(&ns);
        bool force = g_nas_force_notify;
        if (force || ns.reg_state != last_reg || ns.rat != last_rat ||
            strcmp(ns.plmn, last_plmn) != 0 || ns.cell_id != last_cid) {
            g_nas_force_notify = false;
            klog("NAS state: reg=%d rat=%d plmn=%s op='%s' ci=%u tac=%u rsrp=%d rsrq=%d snr=%d",
                 ns.reg_state, ns.rat, ns.plmn, ns.op_name,
                 ns.cell_id, (unsigned)ns.tac, ns.lte_rsrp, ns.lte_rsrq, ns.lte_snr);
            send_unsol_empty_ind(&g_net_ind, NET_IND_NETWORK_STATE_CHANGED, "networkStateChanged");
            send_signal_strength_ind(&ns);
            last_reg = ns.reg_state;
            last_rat = ns.rat;
            snprintf(last_plmn, sizeof(last_plmn), "%s", ns.plmn);
            last_cid = ns.cell_id;
            last_rsrp = ns.lte_rsrp;
        } else if (ns.reg_state == 1 && ns.lte_rsrp != INT32_MAX_VAL &&
                   (last_rsrp == INT32_MAX_VAL || abs(ns.lte_rsrp - last_rsrp) >= 3)) {
            send_signal_strength_ind(&ns);
            last_rsrp = ns.lte_rsrp;
        }
    }
    return NULL;
}

int main(void) {
    g_kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    load_liblog();
    klog("starting Android 17 AIDL Radio HAL (V1)");

    if (!load_binder_ndk())
        return 1;

    g_cfg_rsp_cls = define_cb_class("android.hardware.radio.config.IRadioConfigResponse");
    g_cfg_ind_cls = define_cb_class("android.hardware.radio.config.IRadioConfigIndication");
    g_mdm_rsp_cls = define_cb_class("android.hardware.radio.modem.IRadioModemResponse");
    g_mdm_ind_cls = define_cb_class("android.hardware.radio.modem.IRadioModemIndication");
    g_sim_rsp_cls = define_cb_class("android.hardware.radio.sim.IRadioSimResponse");
    g_sim_ind_cls = define_cb_class("android.hardware.radio.sim.IRadioSimIndication");
    g_net_rsp_cls = define_cb_class("android.hardware.radio.network.IRadioNetworkResponse");
    g_net_ind_cls = define_cb_class("android.hardware.radio.network.IRadioNetworkIndication");
    g_dat_rsp_cls = define_cb_class("android.hardware.radio.data.IRadioDataResponse");
    g_dat_ind_cls = define_cb_class("android.hardware.radio.data.IRadioDataIndication");
    g_voc_rsp_cls = define_cb_class("android.hardware.radio.voice.IRadioVoiceResponse");
    g_voc_ind_cls = define_cb_class("android.hardware.radio.voice.IRadioVoiceIndication");
    g_msg_rsp_cls = define_cb_class("android.hardware.radio.messaging.IRadioMessagingResponse");
    g_msg_ind_cls = define_cb_class("android.hardware.radio.messaging.IRadioMessagingIndication");

    g_ndk.setThreadPoolMaxThreadCount(4);
    g_ndk.startThreadPool();

    pthread_t th, ind_th;
    if (pthread_create(&th, NULL, qmi_init_thread, NULL) == 0)
        pthread_detach(th);
    if (pthread_create(&ind_th, NULL, qmi_ind_thread, NULL) == 0)
        pthread_detach(ind_th);

    register_svc(ICFG_DESC, ICFG_INST, cfg_on_transact);
    register_svc(IMDM_DESC, IMDM_INST, mdm_on_transact);
    register_svc(ISIM_DESC, ISIM_INST, sim_on_transact);
    register_svc(INET_DESC, INET_INST, net_on_transact);
    register_svc(IDAT_DESC, IDAT_INST, dat_on_transact);
    register_svc(IVOC_DESC, IVOC_INST, voc_on_transact);
    register_svc(IMSG_DESC, IMSG_INST, msg_on_transact);

    g_ndk.joinThreadPool();
    return 0;
}
