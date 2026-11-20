/*
 * AIDL android.hardware.bluetooth.IBluetoothHci/default (V1) HAL for Pixel 1 (sailfish).
 *
 * Bridges Android 17's Bluetooth stack (com.android.bt) directly to the mainline
 * Linux kernel QCA6174 serdev controller (hci0) via AF_BLUETOOTH / HCI_CHANNEL_USER.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AIBinder_DeathRecipient AIBinder_DeathRecipient;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;

typedef void *(*AIBinder_Class_onCreate)(void *args);
typedef void (*AIBinder_Class_onDestroy)(void *userData);
typedef int32_t (*AIBinder_Class_onTransact)(AIBinder *binder, uint32_t code,
                                             const AParcel *in, AParcel *out);
typedef void (*AIBinder_DeathRecipient_onBinderDied)(void *cookie);
typedef bool (*AParcel_byteArrayAllocator)(void *arrayData, int32_t length,
                                           int8_t **outBuffer);

/* Linux Bluetooth HCI UAPI constants (no external bluez headers required) */
#define AF_BLUETOOTH      31
#define BTPROTO_HCI       1
#define HCI_CHANNEL_USER  1
#define HCIDEVDOWN        _IOW('H', 202, int)

#define H4_CMD_PKT        0x01
#define H4_ACL_PKT        0x02
#define H4_SCO_PKT        0x03
#define H4_EVT_PKT        0x04
#define H4_ISO_PKT        0x05

struct sockaddr_hci {
    sa_family_t hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

/* android.hardware.bluetooth.IBluetoothHci (V1) transaction codes (alphabetical) */
enum {
    TX_CLOSE = 1,
    TX_INITIALIZE = 2,
    TX_SEND_ACL_DATA = 3,
    TX_SEND_HCI_COMMAND = 4,
    TX_SEND_ISO_DATA = 5,
    TX_SEND_SCO_DATA = 6,
    TX_GET_INTERFACE_HASH = 16777214,
    TX_GET_INTERFACE_VERSION = 16777215,
};

/* android.hardware.bluetooth.IBluetoothHciCallbacks (V1) transaction codes (alphabetical) */
enum {
    CB_ACL_DATA_RECEIVED = 1,
    CB_HCI_EVENT_RECEIVED = 2,
    CB_INITIALIZATION_COMPLETE = 3,
    CB_ISO_DATA_RECEIVED = 4,
    CB_SCO_DATA_RECEIVED = 5,
};

/* android.hardware.bluetooth.Status (V1) */
enum {
    BT_STATUS_SUCCESS = 0,
    BT_STATUS_ALREADY_INITIALIZED = 1,
    BT_STATUS_UNABLE_TO_OPEN_INTERFACE = 2,
    BT_STATUS_HARDWARE_INITIALIZATION_ERROR = 3,
    BT_STATUS_UNKNOWN = 4,
};

enum {
    STATUS_OK_VAL = 0,
    STATUS_UNKNOWN_TX_VAL = -EBADMSG,
};

static const char IHCI_DESC[] = "android.hardware.bluetooth.IBluetoothHci";
static const char IHCI_CB_DESC[] = "android.hardware.bluetooth.IBluetoothHciCallbacks";
static const char IHCI_INSTANCE[] = "android.hardware.bluetooth.IBluetoothHci/default";
static const char IHCI_HASH[] = "fcf7df331b538d2e5c1f35c8568f1cf4c806c90c";

static struct {
    AIBinder_Class *(*Class_define)(const char *, AIBinder_Class_onCreate,
                                    AIBinder_Class_onDestroy,
                                    AIBinder_Class_onTransact);
    AIBinder *(*AIBinder_new)(const AIBinder_Class *, void *);
    void (*markVintfStability)(AIBinder *);
    bool (*associateClass)(AIBinder *, const AIBinder_Class *);
    void (*decStrong)(AIBinder *);
    int32_t (*prepareTransaction)(AIBinder *, AParcel **);
    int32_t (*transact)(AIBinder *, uint32_t, AParcel **, AParcel **, uint32_t);
    AIBinder_DeathRecipient *(*DeathRecipient_new)(AIBinder_DeathRecipient_onBinderDied);
    int32_t (*linkToDeath)(AIBinder *, AIBinder_DeathRecipient *, void *);
    int32_t (*unlinkToDeath)(AIBinder *, AIBinder_DeathRecipient *, void *);
    int32_t (*addService)(AIBinder *, const char *);
    bool (*setThreadPoolMaxThreadCount)(uint32_t);
    void (*startThreadPool)(void);
    void (*joinThreadPool)(void);
    AStatus *(*AStatus_newOk)(void);
    void (*AStatus_delete)(AStatus *);
    void (*AParcel_delete)(AParcel *);
    int32_t (*writeStatusHeader)(AParcel *, const AStatus *);
    int32_t (*writeInt32)(AParcel *, int32_t);
    int32_t (*writeString)(AParcel *, const char *, int32_t);
    int32_t (*writeByteArray)(AParcel *, const int8_t *, int32_t);
    int32_t (*readStrongBinder)(const AParcel *, AIBinder **);
    int32_t (*readByteArray)(const AParcel *, void *, AParcel_byteArrayAllocator);
} g_ndk;

static AIBinder_Class *g_cb_class = NULL;
static AIBinder_DeathRecipient *g_death_recipient = NULL;

static pthread_mutex_t g_state_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_tx_mu = PTHREAD_MUTEX_INITIALIZER;
static AIBinder *g_cb = NULL;
static int g_hci_fd = -1;
static int g_wake_pipe[2] = { -1, -1 };
static pthread_t g_reader_tid;
static bool g_reader_running = false;
static uint16_t g_last_vendor_cmd = 0;

static void kmsg_log(const char *msg) {
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)write(fd, msg, strlen(msg));
        close(fd);
    }
}

static bool resolve_ndk(void *lib) {
    const struct {
        const char *name;
        void **slot;
    } syms[] = {
        { "AIBinder_Class_define", (void **)&g_ndk.Class_define },
        { "AIBinder_new", (void **)&g_ndk.AIBinder_new },
        { "AIBinder_markVintfStability", (void **)&g_ndk.markVintfStability },
        { "AIBinder_associateClass", (void **)&g_ndk.associateClass },
        { "AIBinder_decStrong", (void **)&g_ndk.decStrong },
        { "AIBinder_prepareTransaction", (void **)&g_ndk.prepareTransaction },
        { "AIBinder_transact", (void **)&g_ndk.transact },
        { "AIBinder_DeathRecipient_new", (void **)&g_ndk.DeathRecipient_new },
        { "AIBinder_linkToDeath", (void **)&g_ndk.linkToDeath },
        { "AIBinder_unlinkToDeath", (void **)&g_ndk.unlinkToDeath },
        { "AServiceManager_addService", (void **)&g_ndk.addService },
        { "ABinderProcess_setThreadPoolMaxThreadCount", (void **)&g_ndk.setThreadPoolMaxThreadCount },
        { "ABinderProcess_startThreadPool", (void **)&g_ndk.startThreadPool },
        { "ABinderProcess_joinThreadPool", (void **)&g_ndk.joinThreadPool },
        { "AStatus_newOk", (void **)&g_ndk.AStatus_newOk },
        { "AStatus_delete", (void **)&g_ndk.AStatus_delete },
        { "AParcel_delete", (void **)&g_ndk.AParcel_delete },
        { "AParcel_writeStatusHeader", (void **)&g_ndk.writeStatusHeader },
        { "AParcel_writeInt32", (void **)&g_ndk.writeInt32 },
        { "AParcel_writeString", (void **)&g_ndk.writeString },
        { "AParcel_writeByteArray", (void **)&g_ndk.writeByteArray },
        { "AParcel_readStrongBinder", (void **)&g_ndk.readStrongBinder },
        { "AParcel_readByteArray", (void **)&g_ndk.readByteArray },
    };

    for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        *syms[i].slot = dlsym(lib, syms[i].name);
        if (!*syms[i].slot)
            return false;
    }
    return true;
}

static int32_t write_ok_status(AParcel *out) {
    AStatus *st = g_ndk.AStatus_newOk();
    if (!st)
        return -ENOMEM;
    int32_t rc = g_ndk.writeStatusHeader(out, st);
    g_ndk.AStatus_delete(st);
    return rc;
}

static void send_cb_init_complete(AIBinder *cb, int32_t status) {
    if (!cb)
        return;
    AParcel *in = NULL;
    AParcel *out = NULL;
    if (g_ndk.prepareTransaction(cb, &in) != STATUS_OK_VAL || !in)
        return;
    if (g_ndk.writeInt32(in, status) == STATUS_OK_VAL) {
        (void)g_ndk.transact(cb, CB_INITIALIZATION_COMPLETE, &in, &out, 0);
    }
    if (in)
        g_ndk.AParcel_delete(in);
    if (out)
        g_ndk.AParcel_delete(out);
}

static void send_cb_packet(AIBinder *cb, uint32_t tx_code,
                           const uint8_t *payload, size_t len) {
    if (!cb || len == 0 || len > 65536)
        return;
    AParcel *in = NULL;
    AParcel *out = NULL;
    if (g_ndk.prepareTransaction(cb, &in) != STATUS_OK_VAL || !in)
        return;
    if (g_ndk.writeByteArray(in, (const int8_t *)payload, (int32_t)len) == STATUS_OK_VAL) {
        (void)g_ndk.transact(cb, tx_code, &in, &out, 0);
    }
    if (in)
        g_ndk.AParcel_delete(in);
    if (out)
        g_ndk.AParcel_delete(out);
}

/* Program a persistent, unique BD_ADDR derived from wlan0's factory MAC via QCA Rome
 * EDL_NVM_ACCESS_SET_REQ_CMD (opcode 0xFC0B, tag 0x02) so Android doesn't see 00:00:00:00:5a:ad. */
static void program_rome_bdaddr(int hci_fd) {
    int fd = open("/sys/class/net/wlan0/address", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 17)
        return;

    unsigned int b[6] = {0};
    if (sscanf(buf, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return;
    /* Derive BT MAC from Wi-Fi MAC by flipping bit 0 of the last octet */
    b[5] ^= 0x01;

    uint8_t cmd[13] = {
        H4_CMD_PKT,
        0x0b, 0xfc, /* EDL_NVM_ACCESS_OPCODE (0xFC0B) */
        0x09,       /* param len */
        0x01,       /* EDL_NVM_ACCESS_SET_REQ_CMD */
        0x02,       /* TAG ID = BD_ADDR */
        0x06,       /* length = 6 */
        (uint8_t)b[5], (uint8_t)b[4], (uint8_t)b[3],
        (uint8_t)b[2], (uint8_t)b[1], (uint8_t)b[0],
    };
    if (write(hci_fd, cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd))
        return;

    /* Consume the single vendor/command-complete event (up to 500 ms) so it isn't leaked */
    struct pollfd pfd = { .fd = hci_fd, .events = POLLIN };
    if (poll(&pfd, 1, 500) > 0 && (pfd.revents & POLLIN)) {
        uint8_t resp[260];
        (void)read(hci_fd, resp, sizeof(resp));
    }
}

static int open_hci_user_channel(void) {
    /* Kernel requires hci0 to be DOWN before binding HCI_CHANNEL_USER */
    int ctl = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (ctl >= 0) {
        (void)ioctl(ctl, HCIDEVDOWN, 0);
        close(ctl);
    }

    for (int attempt = 0; attempt < 20; attempt++) {
        int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
        if (fd < 0)
            return -errno;

        struct sockaddr_hci addr;
        memset(&addr, 0, sizeof(addr));
        addr.hci_family = AF_BLUETOOTH;
        addr.hci_dev = 0;
        addr.hci_channel = HCI_CHANNEL_USER;

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        int err = errno;
        close(fd);
        if (err == EBUSY || err == EAGAIN || err == ENODEV) {
            usleep(100000);
            continue;
        }
        return -err;
    }
    return -ETIMEDOUT;
}

static void *hci_reader_thread(void *arg) {
    (void)arg;
    uint8_t buf[4096];

    for (;;) {
        struct pollfd pfds[2] = {
            { .fd = g_hci_fd, .events = POLLIN },
            { .fd = g_wake_pipe[0], .events = POLLIN },
        };
        int rc = poll(pfds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pfds[1].revents & POLLIN)
            break;
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if (!(pfds[0].revents & POLLIN))
            continue;

        ssize_t n = read(g_hci_fd, buf, sizeof(buf));
        if (n <= 1) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            break;
        }

        AIBinder *cb = g_cb;
        if (!cb)
            continue;

        uint8_t pkt_type = buf[0];
        uint8_t *payload = buf + 1;
        size_t plen = (size_t)(n - 1);

        if (pkt_type == H4_EVT_PKT) {
            /* QCA6174 Rome strips OGF 0x3f (0xfc00) in Command Complete/Status for
             * unsupported Android vendor commands (e.g. 0xfd57 -> 0x0157). Restore it. */
            uint16_t last_v = __atomic_load_n(&g_last_vendor_cmd, __ATOMIC_ACQUIRE);
            if (last_v != 0) {
                size_t op_off = 0;
                if (payload[0] == 0x0e && plen >= 6) {
                    op_off = 3;
                } else if (payload[0] == 0x0f && plen >= 6) {
                    op_off = 4;
                }
                if (op_off != 0) {
                    uint16_t evt_op = (uint16_t)payload[op_off] |
                                      ((uint16_t)payload[op_off + 1] << 8);
                    if (evt_op == (last_v & 0x03ffU)) {
                        payload[op_off] = (uint8_t)(last_v & 0xffU);
                        payload[op_off + 1] = (uint8_t)(last_v >> 8);
                        __atomic_store_n(&g_last_vendor_cmd, 0, __ATOMIC_RELEASE);
                    } else if (evt_op == last_v) {
                        __atomic_store_n(&g_last_vendor_cmd, 0, __ATOMIC_RELEASE);
                    }
                }
            }
        }

        switch (pkt_type) {
        case H4_EVT_PKT:
            send_cb_packet(cb, CB_HCI_EVENT_RECEIVED, payload, plen);
            break;
        case H4_ACL_PKT:
            send_cb_packet(cb, CB_ACL_DATA_RECEIVED, payload, plen);
            break;
        case H4_SCO_PKT:
            send_cb_packet(cb, CB_SCO_DATA_RECEIVED, payload, plen);
            break;
        case H4_ISO_PKT:
            send_cb_packet(cb, CB_ISO_DATA_RECEIVED, payload, plen);
            break;
        default:
            break;
        }
    }
    return NULL;
}

static void close_hci_locked(void) {
    if (g_reader_running) {
        if (g_wake_pipe[1] >= 0) {
            char b = 1;
            (void)write(g_wake_pipe[1], &b, 1);
        }
        pthread_join(g_reader_tid, NULL);
        g_reader_running = false;
    }
    if (g_wake_pipe[0] >= 0) {
        close(g_wake_pipe[0]);
        g_wake_pipe[0] = -1;
    }
    if (g_wake_pipe[1] >= 0) {
        close(g_wake_pipe[1]);
        g_wake_pipe[1] = -1;
    }
    pthread_mutex_lock(&g_tx_mu);
    if (g_hci_fd >= 0) {
        close(g_hci_fd);
        g_hci_fd = -1;
    }
    pthread_mutex_unlock(&g_tx_mu);

    if (g_cb) {
        if (g_death_recipient)
            (void)g_ndk.unlinkToDeath(g_cb, g_death_recipient, NULL);
        g_ndk.decStrong(g_cb);
        g_cb = NULL;
    }
}

static void on_client_died(void *cookie) {
    (void)cookie;
    kmsg_log("<6>sailfish_bluetooth_hal: client died, closing hci0\n");
    pthread_mutex_lock(&g_state_mu);
    close_hci_locked();
    pthread_mutex_unlock(&g_state_mu);
}

struct h4_alloc_ctx {
    uint8_t pkt_type;
    uint8_t *buf;
    size_t total_len;
};

static bool h4_byte_array_allocator(void *array_data, int32_t length, int8_t **out_buf) {
    struct h4_alloc_ctx *ctx = (struct h4_alloc_ctx *)array_data;
    if (length < 0 || length > 65535) {
        *out_buf = NULL;
        ctx->buf = NULL;
        ctx->total_len = 0;
        return length < 0;
    }
    ctx->buf = (uint8_t *)malloc((size_t)length + 1);
    if (!ctx->buf)
        return false;
    ctx->buf[0] = ctx->pkt_type;
    ctx->total_len = (size_t)length + 1;
    *out_buf = (int8_t *)(ctx->buf + 1);
    return true;
}

static int32_t handle_send_packet(const AParcel *in, AParcel *out, uint8_t pkt_type) {
    struct h4_alloc_ctx ctx = { .pkt_type = pkt_type, .buf = NULL, .total_len = 0 };
    int32_t rc = g_ndk.readByteArray(in, &ctx, h4_byte_array_allocator);
    if (rc != STATUS_OK_VAL) {
        free(ctx.buf);
        return rc;
    }
    if (ctx.buf && ctx.total_len > 1) {
        if (pkt_type == H4_CMD_PKT && ctx.total_len >= 4) {
            uint16_t opcode = (uint16_t)ctx.buf[1] | ((uint16_t)ctx.buf[2] << 8);
            if ((opcode & 0xfc00U) == 0xfc00U) {
                __atomic_store_n(&g_last_vendor_cmd, opcode, __ATOMIC_RELEASE);
            }
        }
        pthread_mutex_lock(&g_tx_mu);
        if (g_hci_fd >= 0) {
            (void)write(g_hci_fd, ctx.buf, ctx.total_len);
        }
        pthread_mutex_unlock(&g_tx_mu);
    }
    free(ctx.buf);
    return write_ok_status(out);
}

static int32_t handle_initialize(const AParcel *in, AParcel *out) {
    AIBinder *cb = NULL;
    int32_t rc = g_ndk.readStrongBinder(in, &cb);
    if (rc != STATUS_OK_VAL || !cb)
        return write_ok_status(out);

    (void)g_ndk.associateClass(cb, g_cb_class);

    pthread_mutex_lock(&g_state_mu);
    if (g_hci_fd >= 0) {
        close_hci_locked();
    }

    int hci_fd = open_hci_user_channel();
    if (hci_fd < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "<3>sailfish_bluetooth_hal: open_hci_user_channel failed (%d)\n", hci_fd);
        kmsg_log(msg);
        pthread_mutex_unlock(&g_state_mu);
        send_cb_init_complete(cb, BT_STATUS_UNABLE_TO_OPEN_INTERFACE);
        g_ndk.decStrong(cb);
        return write_ok_status(out);
    }

    program_rome_bdaddr(hci_fd);

    if (pipe2(g_wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        close(hci_fd);
        pthread_mutex_unlock(&g_state_mu);
        send_cb_init_complete(cb, BT_STATUS_HARDWARE_INITIALIZATION_ERROR);
        g_ndk.decStrong(cb);
        return write_ok_status(out);
    }

    g_hci_fd = hci_fd;
    g_cb = cb;
    if (g_death_recipient)
        (void)g_ndk.linkToDeath(g_cb, g_death_recipient, NULL);

    if (pthread_create(&g_reader_tid, NULL, hci_reader_thread, NULL) != 0) {
        close_hci_locked();
        pthread_mutex_unlock(&g_state_mu);
        send_cb_init_complete(cb, BT_STATUS_HARDWARE_INITIALIZATION_ERROR);
        return write_ok_status(out);
    }
    g_reader_running = true;
    pthread_mutex_unlock(&g_state_mu);

    kmsg_log("<6>sailfish_bluetooth_hal: initialized hci0 (HCI_CHANNEL_USER)\n");
    send_cb_init_complete(cb, BT_STATUS_SUCCESS);
    return write_ok_status(out);
}

static void *dummy_on_create(void *args) { return args; }
static void dummy_on_destroy(void *user_data) { (void)user_data; }
static int32_t dummy_on_transact(AIBinder *b, uint32_t c, const AParcel *i, AParcel *o) {
    (void)b; (void)c; (void)i; (void)o;
    return STATUS_UNKNOWN_TX_VAL;
}

static int32_t ihci_on_transact(AIBinder *binder, uint32_t code,
                                const AParcel *in, AParcel *out) {
    (void)binder;
    int32_t rc;

    switch (code) {
    case TX_CLOSE:
        pthread_mutex_lock(&g_state_mu);
        close_hci_locked();
        pthread_mutex_unlock(&g_state_mu);
        kmsg_log("<6>sailfish_bluetooth_hal: closed hci0\n");
        return write_ok_status(out);

    case TX_INITIALIZE:
        return handle_initialize(in, out);

    case TX_SEND_ACL_DATA:
        return handle_send_packet(in, out, H4_ACL_PKT);

    case TX_SEND_HCI_COMMAND:
        return handle_send_packet(in, out, H4_CMD_PKT);

    case TX_SEND_ISO_DATA:
        return handle_send_packet(in, out, H4_ISO_PKT);

    case TX_SEND_SCO_DATA:
        return handle_send_packet(in, out, H4_SCO_PKT);

    case TX_GET_INTERFACE_HASH:
        rc = write_ok_status(out);
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeString(out, IHCI_HASH, (int32_t)(sizeof(IHCI_HASH) - 1));

    case TX_GET_INTERFACE_VERSION:
        rc = write_ok_status(out);
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 1);

    default:
        return STATUS_UNKNOWN_TX_VAL;
    }
}

int main(void) {
    void *lib = dlopen("/system/lib64/libbinder_ndk.so", RTLD_NOW);
    if (!lib || !resolve_ndk(lib)) {
        kmsg_log("<3>sailfish_bluetooth_hal: failed to load libbinder_ndk.so symbols\n");
        for (;;) pause();
    }

    g_cb_class = g_ndk.Class_define(IHCI_CB_DESC, dummy_on_create,
                                    dummy_on_destroy, dummy_on_transact);
    g_death_recipient = g_ndk.DeathRecipient_new(on_client_died);

    AIBinder_Class *clazz = g_ndk.Class_define(IHCI_DESC, dummy_on_create,
                                               dummy_on_destroy, ihci_on_transact);
    AIBinder *binder = clazz ? g_ndk.AIBinder_new(clazz, NULL) : NULL;
    if (!binder || !g_cb_class) {
        kmsg_log("<3>sailfish_bluetooth_hal: AIBinder_new failed\n");
        for (;;) pause();
    }

    g_ndk.markVintfStability(binder);

    while (g_ndk.addService(binder, IHCI_INSTANCE) != STATUS_OK_VAL) {
        usleep(250000);
    }
    kmsg_log("<6>sailfish_bluetooth_hal: registered android.hardware.bluetooth.IBluetoothHci/default (V1)\n");

    g_ndk.setThreadPoolMaxThreadCount(4);
    g_ndk.startThreadPool();
    g_ndk.joinThreadPool();

    for (;;) pause();
    return 0;
}
