/*
 * Sailfish Native Android 17 AIDL IHostapd HAL (V1)
 * Implements android.hardware.wifi.hostapd.IHostapd/default by driving
 * Android 17's built-in /apex/com.android.wifi/bin/wpa_supplicant_mainline
 * in SoftAP mode (mode=2, CONFIG_AP=y).
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define HOSTAPD_HAL_VERSION 1
#define DESC_IHOSTAPD       "android.hardware.wifi.hostapd.IHostapd"
#define DESC_IHOSTAPD_CB    "android.hardware.wifi.hostapd.IHostapdCallback"

#define TX_GET_VERSION 16777215u
#define TX_GET_HASH    16777214u
#define FLAG_ONEWAY    1u

#define STATUS_OK 0
#define EX_UNSUPPORTED_OPERATION (-7)
#define EX_ILLEGAL_ARGUMENT      (-3)

#define WPA_MAINLINE_BIN "/apex/com.android.wifi/bin/wpa_supplicant_mainline"
#define AP_CONF_DIR      "/data/vendor/wifi/hostapd"
#define AP_CTRL_DIR      "/data/vendor/wifi/hostapd/ctrl"
#define AP_CONF_PATH     "/data/vendor/wifi/hostapd/wpa_ap.conf"

typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef int32_t binder_status_t;
typedef uint32_t transaction_code_t;

typedef void *(*AIBinder_Class_onCreate)(void *args);
typedef void (*AIBinder_Class_onDestroy)(void *userData);
typedef binder_status_t (*AIBinder_Class_onTransact)(AIBinder *, transaction_code_t,
                                                     const AParcel *, AParcel *);
typedef bool (*AParcel_stringAllocator)(void *stringData, int32_t length, char **buffer);
typedef bool (*AParcel_byteArrayAllocator)(void *arrayData, int32_t length, int8_t **outBuffer);

static struct {
    AIBinder_Class *(*Class_define)(const char *, AIBinder_Class_onCreate,
                                    AIBinder_Class_onDestroy, AIBinder_Class_onTransact);
    AIBinder *(*newBinder)(const AIBinder_Class *, void *);
    void (*incStrong)(AIBinder *);
    void (*decStrong)(AIBinder *);
    binder_status_t (*addService)(AIBinder *, const char *);
    void (*markVintfStability)(AIBinder *);
    bool (*associateClass)(AIBinder *, const AIBinder_Class *);
    binder_status_t (*prepareTransaction)(AIBinder *, AParcel **);
    binder_status_t (*transact)(AIBinder *, transaction_code_t, AParcel **, AParcel **, uint32_t);
    void (*parcelDelete)(AParcel *);
    void (*startThreadPool)(void);
    void (*joinThreadPool)(void);

    binder_status_t (*readInt32)(const AParcel *, int32_t *);
    binder_status_t (*readBool)(const AParcel *, bool *);
    binder_status_t (*readString)(const AParcel *, void *, AParcel_stringAllocator);
    binder_status_t (*readByteArray)(const AParcel *, void *, AParcel_byteArrayAllocator);
    binder_status_t (*readStrongBinder)(const AParcel *, AIBinder **);
    int32_t (*getDataPosition)(const AParcel *);
    binder_status_t (*setDataPosition)(const AParcel *, int32_t);

    binder_status_t (*writeInt32)(AParcel *, int32_t);
    binder_status_t (*writeString)(AParcel *, const char *, int32_t);
    binder_status_t (*writeByteArray)(AParcel *, const int8_t *, int32_t);
} g_bn;

static AIBinder_Class *g_cls_hostapd;
static AIBinder_Class *g_cls_cb;

static int (*g_alog_vprint)(int prio, const char *tag, const char *fmt, va_list ap);

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_alog_vprint) {
        g_alog_vprint(4 /* ANDROID_LOG_INFO */, "sailfish_hostapd", fmt, ap);
    } else {
        fprintf(stderr, "[sailfish_hostapd] ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    va_end(ap);
}

static bool load_binder_ndk(void) {
    void *lh = dlopen("liblog.so", RTLD_NOW);
    if (lh)
        *(void **)&g_alog_vprint = dlsym(lh, "__android_log_vprint");

    void *h = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!h) {
        log_msg("dlopen libbinder_ndk.so failed: %s", dlerror());
        return false;
    }
#define LOAD(field, sym) do { \
    *(void **)&g_bn.field = dlsym(h, sym); \
    if (!g_bn.field) { log_msg("missing symbol %s", sym); return false; } \
} while (0)
    LOAD(Class_define, "AIBinder_Class_define");
    LOAD(newBinder, "AIBinder_new");
    LOAD(incStrong, "AIBinder_incStrong");
    LOAD(decStrong, "AIBinder_decStrong");
    LOAD(addService, "AServiceManager_addService");
    *(void **)&g_bn.markVintfStability = dlsym(h, "AIBinder_markVintfStability");
    LOAD(associateClass, "AIBinder_associateClass");
    LOAD(prepareTransaction, "AIBinder_prepareTransaction");
    LOAD(transact, "AIBinder_transact");
    LOAD(parcelDelete, "AParcel_delete");
    LOAD(startThreadPool, "ABinderProcess_startThreadPool");
    LOAD(joinThreadPool, "ABinderProcess_joinThreadPool");
    LOAD(readInt32, "AParcel_readInt32");
    LOAD(readBool, "AParcel_readBool");
    LOAD(readString, "AParcel_readString");
    LOAD(readByteArray, "AParcel_readByteArray");
    LOAD(readStrongBinder, "AParcel_readStrongBinder");
    LOAD(getDataPosition, "AParcel_getDataPosition");
    LOAD(setDataPosition, "AParcel_setDataPosition");
    LOAD(writeInt32, "AParcel_writeInt32");
    LOAD(writeString, "AParcel_writeString");
    LOAD(writeByteArray, "AParcel_writeByteArray");
#undef LOAD
    return true;
}

typedef struct {
    char buf[128];
} str_buf_t;

static bool str_buf_alloc(void *data, int32_t len, char **out) {
    str_buf_t *sb = (str_buf_t *)data;
    sb->buf[0] = '\0';
    if (len <= 0) {
        *out = NULL;
        return true;
    }
    if (len >= (int32_t)sizeof(sb->buf)) {
        *out = NULL;
        return false;
    }
    *out = sb->buf;
    return true;
}

typedef struct {
    int8_t buf[128];
    int32_t len;
} byte_buf_t;

static bool byte_buf_alloc(void *data, int32_t len, int8_t **out) {
    byte_buf_t *bb = (byte_buf_t *)data;
    bb->len = 0;
    if (len <= 0) {
        *out = NULL;
        return true;
    }
    if (len >= (int32_t)sizeof(bb->buf)) {
        *out = NULL;
        return false;
    }
    bb->len = len;
    *out = bb->buf;
    return true;
}

static void skip_parcelable(const AParcel *in) {
    int32_t nn = 0, sz = 0;
    if (g_bn.readInt32(in, &nn) != STATUS_OK || !nn) return;
    int32_t pos = g_bn.getDataPosition(in);
    if (g_bn.readInt32(in, &sz) == STATUS_OK && sz >= 4)
        g_bn.setDataPosition(in, pos + sz);
}

static void write_str(AParcel *p, const char *s) {
    g_bn.writeString(p, s, (int32_t)strlen(s));
}

static int32_t parcel_begin(AParcel *p) {
    int32_t start = g_bn.getDataPosition(p);
    g_bn.writeInt32(p, 0);
    return start;
}

static void parcel_end(AParcel *p, int32_t start) {
    int32_t end = g_bn.getDataPosition(p);
    g_bn.setDataPosition(p, start);
    g_bn.writeInt32(p, end - start);
    g_bn.setDataPosition(p, end);
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static AIBinder *g_cb = NULL;
static pid_t g_ap_pid = -1;
static char g_ap_iface[32] = "";

static void stop_ap_locked(void) {
    if (g_ap_pid > 0) {
        log_msg("Stopping SoftAP wpa_supplicant pid=%d iface=%s", g_ap_pid, g_ap_iface);
        kill(g_ap_pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            int st = 0;
            if (waitpid(g_ap_pid, &st, WNOHANG) > 0) {
                g_ap_pid = -1;
                break;
            }
            usleep(50000);
        }
        if (g_ap_pid > 0) {
            kill(g_ap_pid, SIGKILL);
            waitpid(g_ap_pid, NULL, 0);
            g_ap_pid = -1;
        }
    }
    if (g_ap_iface[0]) {
        char sock[128];
        snprintf(sock, sizeof(sock), "%s/%s", AP_CTRL_DIR, g_ap_iface);
        unlink(sock);
        g_ap_iface[0] = '\0';
    }
}

static bool read_iface_mac(const char *iface, int8_t mac[6]) {
    memset(mac, 0, 6);
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    unsigned int b[6] = { 0 };
    int n = fscanf(f, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    fclose(f);
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) mac[i] = (int8_t)(b[i] & 0xff);
    return true;
}

static void notify_ap_info(const char *iface, int32_t freq_mhz) {
    pthread_mutex_lock(&g_lock);
    AIBinder *cb = g_cb;
    if (cb) g_bn.incStrong(cb);
    pthread_mutex_unlock(&g_lock);
    if (!cb) return;

    int8_t mac[6] = { 0 };
    read_iface_mac(iface, mac);

    g_bn.associateClass(cb, g_cls_cb);
    AParcel *in = NULL, *out = NULL;
    if (g_bn.prepareTransaction(cb, &in) == STATUS_OK) {
        g_bn.writeInt32(in, 1); /* non-null ApInfo */
        int32_t s = parcel_begin(in);
        write_str(in, iface);
        write_str(in, iface);
        g_bn.writeInt32(in, freq_mhz);
        g_bn.writeInt32(in, 1 /* BANDWIDTH_20 */);
        g_bn.writeInt32(in, 1 /* WIFI_STANDARD_11N */);
        g_bn.writeByteArray(in, mac, 6);
        parcel_end(in, s);
        binder_status_t rc = g_bn.transact(cb, 1 /* onApInstanceInfoChanged */,
                                           &in, &out, FLAG_ONEWAY);
        if (out) g_bn.parcelDelete(out);
        log_msg("Notified onApInstanceInfoChanged(iface=%s freq=%d) rc=%d", iface, freq_mhz, rc);
    }
    g_bn.decStrong(cb);
}

static void wpa_ctrl_send(const char *iface, const char *cmd) {
    char dst_path[128], local_path[128];
    snprintf(dst_path, sizeof(dst_path), "%s/%s", AP_CTRL_DIR, iface);
    snprintf(local_path, sizeof(local_path), "%s/cli_%d", AP_CTRL_DIR, getpid());

    /* Wait up to 1.5s for wpa_supplicant ctrl socket to appear */
    for (int i = 0; i < 30; i++) {
        if (access(dst_path, F_OK) == 0) break;
        usleep(50000);
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return;

    struct sockaddr_un local, dst;
    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    strncpy(local.sun_path, local_path, sizeof(local.sun_path) - 1);
    unlink(local_path);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(fd);
        return;
    }
    chmod(local_path, 0666);

    memset(&dst, 0, sizeof(dst));
    dst.sun_family = AF_UNIX;
    strncpy(dst.sun_path, dst_path, sizeof(dst.sun_path) - 1);

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ssize_t sent = sendto(fd, cmd, strlen(cmd), 0, (struct sockaddr *)&dst, sizeof(dst));
    char resp[128] = "";
    ssize_t got = (sent > 0) ? recv(fd, resp, sizeof(resp) - 1, 0) : -1;
    if (got > 0) {
        while (got > 0 && (resp[got - 1] == '\n' || resp[got - 1] == '\r')) got--;
        resp[got] = '\0';
    }
    log_msg("wpa_ctrl(%s, '%s') -> '%s' (got=%zd)", iface, cmd, resp, got);
    close(fd);
    unlink(local_path);
}

static int32_t channel_to_freq(int32_t ch, int32_t band_mask) {
    if (ch >= 1 && ch <= 13) return 2407 + ch * 5;
    if (ch == 14) return 2484;
    if (ch >= 36 && ch <= 165) return 5000 + ch * 5;
    /* Default to 2.4 GHz ch 6 (2437 MHz) unless 5 GHz only */
    if ((band_mask & 1) == 0 && (band_mask & 2) != 0)
        return 5180; /* ch 36 */
    return 2437;
}

static void *on_create_noop(void *args) { return args; }
static void on_destroy_noop(void *userData) { (void)userData; }
static binder_status_t on_transact_noop(AIBinder *b, transaction_code_t code,
                                        const AParcel *in, AParcel *out) {
    (void)b; (void)code; (void)in; (void)out;
    return STATUS_OK;
}

static binder_status_t on_transact_hostapd(AIBinder *b, transaction_code_t code,
                                           const AParcel *in, AParcel *out) {
    (void)b;
    if (code == TX_GET_VERSION) {
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, HOSTAPD_HAL_VERSION);
        return STATUS_OK;
    }
    if (code == TX_GET_HASH) {
        g_bn.writeInt32(out, STATUS_OK);
        write_str(out, "notfrozen");
        return STATUS_OK;
    }

    switch (code) {
    case 1: { /* addAccessPoint(IfaceParams, NetworkParams) */
        str_buf_t iface = { .buf = "wlan0" };
        int32_t band_mask = 1;
        int32_t channel = 0;

        /* IfaceParams */
        int32_t nn = 0, psz = 0;
        if (g_bn.readInt32(in, &nn) == STATUS_OK && nn) {
            int32_t pstart = g_bn.getDataPosition(in);
            g_bn.readInt32(in, &psz);
            g_bn.readString(in, &iface, str_buf_alloc);
            skip_parcelable(in); /* hwModeParams */
            /* channelParams[] */
            int32_t nch = 0;
            if (g_bn.readInt32(in, &nch) == STATUS_OK && nch > 0) {
                int32_t cnn = 0, csz = 0;
                if (g_bn.readInt32(in, &cnn) == STATUS_OK && cnn) {
                    int32_t cstart = g_bn.getDataPosition(in);
                    g_bn.readInt32(in, &csz);
                    g_bn.readInt32(in, &band_mask);
                    /* acsChannelFreqRangesMhz[] */
                    int32_t nr = 0;
                    if (g_bn.readInt32(in, &nr) == STATUS_OK && nr > 0) {
                        for (int32_t i = 0; i < nr; i++) skip_parcelable(in);
                    }
                    bool acs = false, excl_dfs = false;
                    g_bn.readBool(in, &acs);
                    g_bn.readBool(in, &excl_dfs);
                    g_bn.readInt32(in, &channel);
                    g_bn.setDataPosition(in, cstart + csz);
                }
            }
            if (psz >= 4) g_bn.setDataPosition(in, pstart + psz);
        }

        /* NetworkParams */
        byte_buf_t ssid = { .len = 0 };
        bool is_hidden = false;
        int32_t enc_type = 1; /* NONE */
        str_buf_t pass = { .buf = "" };

        int32_t nnn = 0, nsz = 0;
        if (g_bn.readInt32(in, &nnn) == STATUS_OK && nnn) {
            int32_t nstart = g_bn.getDataPosition(in);
            g_bn.readInt32(in, &nsz);
            g_bn.readByteArray(in, &ssid, byte_buf_alloc);
            g_bn.readBool(in, &is_hidden);
            g_bn.readInt32(in, &enc_type);
            g_bn.readString(in, &pass, str_buf_alloc);
            if (nsz >= 4) g_bn.setDataPosition(in, nstart + nsz);
        }

        char ssid_str[65] = "AndroidAP";
        if (ssid.len > 0) {
            int32_t l = ssid.len < 64 ? ssid.len : 64;
            memcpy(ssid_str, ssid.buf, (size_t)l);
            ssid_str[l] = '\0';
        }
        int32_t freq = channel_to_freq(channel, band_mask);
        log_msg("addAccessPoint: iface=%s ssid='%s' freq=%d enc=%d hidden=%d",
                iface.buf, ssid_str, freq, enc_type, is_hidden ? 1 : 0);

        mkdir("/data/vendor", 0771);
        mkdir("/data/vendor/wifi", 0771);
        mkdir(AP_CONF_DIR, 0755);
        chmod(AP_CONF_DIR, 0755);
        mkdir(AP_CTRL_DIR, 0770);
        chown(AP_CTRL_DIR, 1010, 1010); /* wifi:wifi */
        chmod(AP_CTRL_DIR, 0770);

        pthread_mutex_lock(&g_lock);
        stop_ap_locked();

        FILE *f = fopen(AP_CONF_PATH, "w");
        if (!f) {
            pthread_mutex_unlock(&g_lock);
            log_msg("Failed to write %s: %s", AP_CONF_PATH, strerror(errno));
            g_bn.writeInt32(out, EX_ILLEGAL_ARGUMENT);
            write_str(out, "cannot write conf");
            return STATUS_OK;
        }
        fprintf(f, "ctrl_interface=%s\n", AP_CTRL_DIR);
        fprintf(f, "ap_scan=1\n");
        fprintf(f, "p2p_disabled=1\n");
        fprintf(f, "country=US\n");
        fprintf(f, "network={\n");
        fprintf(f, "    ssid=\"%s\"\n", ssid_str);
        fprintf(f, "    mode=2\n");
        fprintf(f, "    frequency=%d\n", freq);
        if (is_hidden)
            fprintf(f, "    ignore_broadcast_ssid=1\n");
        if (enc_type == 1 || pass.buf[0] == '\0') {
            fprintf(f, "    key_mgmt=NONE\n");
        } else {
            fprintf(f, "    key_mgmt=WPA-PSK\n");
            fprintf(f, "    proto=RSN\n");
            fprintf(f, "    pairwise=CCMP\n");
            fprintf(f, "    psk=\"%s\"\n", pass.buf);
        }
        fprintf(f, "}\n");
        fclose(f);
        chmod(AP_CONF_PATH, 0644);

        pid_t pid = fork();
        if (pid == 0) {
            execl(WPA_MAINLINE_BIN, "wpa_supplicant_mainline",
                  "-i", iface.buf, "-D", "nl80211", "-c", AP_CONF_PATH, NULL);
            _exit(127);
        }
        if (pid < 0) {
            pthread_mutex_unlock(&g_lock);
            log_msg("fork failed: %s", strerror(errno));
            g_bn.writeInt32(out, EX_ILLEGAL_ARGUMENT);
            write_str(out, "fork failed");
            return STATUS_OK;
        }
        g_ap_pid = pid;
        strncpy(g_ap_iface, iface.buf, sizeof(g_ap_iface) - 1);
        g_ap_iface[sizeof(g_ap_iface) - 1] = '\0';
        pthread_mutex_unlock(&g_lock);

        log_msg("Started SoftAP wpa_supplicant_mainline pid=%d on %s (%d MHz)",
                pid, iface.buf, freq);
        wpa_ctrl_send(iface.buf, "SELECT_NETWORK 0");
        usleep(200000);
        notify_ap_info(iface.buf, freq);

        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    }

    case 2: /* forceClientDisconnect */
    case 5: /* setDebugParams */
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;

    case 3: { /* registerCallback(IHostapdCallback) */
        AIBinder *cb = NULL;
        g_bn.readStrongBinder(in, &cb);
        pthread_mutex_lock(&g_lock);
        if (g_cb) g_bn.decStrong(g_cb);
        g_cb = cb;
        pthread_mutex_unlock(&g_lock);
        log_msg("Registered IHostapdCallback (%p)", (void *)cb);
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    }

    case 4: { /* removeAccessPoint(iface) */
        pthread_mutex_lock(&g_lock);
        stop_ap_locked();
        pthread_mutex_unlock(&g_lock);
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    }

    case 6: { /* terminate (oneway) */
        pthread_mutex_lock(&g_lock);
        stop_ap_locked();
        pthread_mutex_unlock(&g_lock);
        return STATUS_OK;
    }

    default:
        log_msg("IHostapd unhandled tx=%u", code);
        g_bn.writeInt32(out, EX_UNSUPPORTED_OPERATION);
        write_str(out, "unsupported");
        return STATUS_OK;
    }
}

int main(void) {
    signal(SIGCHLD, SIG_IGN);
    log_msg("Starting Sailfish Native Android 17 AIDL IHostapd HAL");
    if (!load_binder_ndk())
        return 1;

    g_cls_hostapd = g_bn.Class_define(DESC_IHOSTAPD, on_create_noop, on_destroy_noop,
                                      on_transact_hostapd);
    g_cls_cb      = g_bn.Class_define(DESC_IHOSTAPD_CB, on_create_noop, on_destroy_noop,
                                      on_transact_noop);

    AIBinder *b = g_bn.newBinder(g_cls_hostapd, NULL);
    if (g_bn.markVintfStability)
        g_bn.markVintfStability(b);

    g_bn.startThreadPool();
    binder_status_t rc = g_bn.addService(b, DESC_IHOSTAPD "/default");
    log_msg("Registered IHostapd/default rc=%d", rc);
    g_bn.joinThreadPool();
    return 0;
}
