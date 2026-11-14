#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;

typedef void *(*AIBinder_Class_onCreate)(void *args);
typedef void (*AIBinder_Class_onDestroy)(void *userData);
typedef int32_t (*AIBinder_Class_onTransact)(AIBinder *binder, uint32_t code,
                                             const AParcel *in, AParcel *out);

enum {
    TX_SET_MODE = 1,
    TX_IS_MODE_SUPPORTED = 2,
    TX_SET_BOOST = 3,
    TX_IS_BOOST_SUPPORTED = 4,
    TX_CREATE_HINT_SESSION = 5,
    TX_GET_HINT_SESSION_PREFERRED_RATE = 6,
    TX_CREATE_HINT_SESSION_WITH_CONFIG = 7,
    TX_GET_SESSION_CHANNEL = 8,
    TX_CLOSE_SESSION_CHANNEL = 9,
    TX_GET_SUPPORT_INFO = 10,
    TX_GET_CPU_HEADROOM = 11,
    TX_GET_GPU_HEADROOM = 12,
    TX_SEND_COMPOSITION_DATA = 13,
    TX_SEND_COMPOSITION_UPDATE = 14,
    TX_GET_INTERFACE_HASH = 16777214,
    TX_GET_INTERFACE_VERSION = 16777215,
};

enum {
    STATUS_OK_VAL = 0,
    STATUS_UNKNOWN_TX_VAL = -EBADMSG,
    EX_UNSUPPORTED_OP_VAL = -7,
};

static const char IPOWER_DESC[] = "android.hardware.power.IPower";
static const char IPOWER_INSTANCE[] = "android.hardware.power.IPower/default";
static const char IPOWER_HASH[] = "4642211f44ad39c8c592b3611e62d29974b784b5";

static struct {
    AIBinder_Class *(*Class_define)(const char *, AIBinder_Class_onCreate,
                                    AIBinder_Class_onDestroy,
                                    AIBinder_Class_onTransact);
    AIBinder *(*AIBinder_new)(const AIBinder_Class *, void *);
    void (*markVintfStability)(AIBinder *);
    int32_t (*addService)(AIBinder *, const char *);
    bool (*setThreadPoolMaxThreadCount)(uint32_t);
    void (*startThreadPool)(void);
    void (*joinThreadPool)(void);
    AStatus *(*AStatus_newOk)(void);
    AStatus *(*AStatus_fromExceptionCode)(int32_t);
    void (*AStatus_delete)(AStatus *);
    int32_t (*writeStatusHeader)(AParcel *, const AStatus *);
    int32_t (*writeBool)(AParcel *, bool);
    int32_t (*writeInt32)(AParcel *, int32_t);
    int32_t (*writeInt64)(AParcel *, int64_t);
    int32_t (*writeString)(AParcel *, const char *, int32_t);
    int32_t (*getDataPosition)(const AParcel *);
    int32_t (*setDataPosition)(const AParcel *, int32_t);
} g_ndk;

static void kmsg_log(const char *msg) {
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t len = 0;
        while (msg[len] != '\0') len++;
        (void)write(fd, msg, len);
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
        { "AServiceManager_addService", (void **)&g_ndk.addService },
        { "ABinderProcess_setThreadPoolMaxThreadCount", (void **)&g_ndk.setThreadPoolMaxThreadCount },
        { "ABinderProcess_startThreadPool", (void **)&g_ndk.startThreadPool },
        { "ABinderProcess_joinThreadPool", (void **)&g_ndk.joinThreadPool },
        { "AStatus_newOk", (void **)&g_ndk.AStatus_newOk },
        { "AStatus_fromExceptionCode", (void **)&g_ndk.AStatus_fromExceptionCode },
        { "AStatus_delete", (void **)&g_ndk.AStatus_delete },
        { "AParcel_writeStatusHeader", (void **)&g_ndk.writeStatusHeader },
        { "AParcel_writeBool", (void **)&g_ndk.writeBool },
        { "AParcel_writeInt32", (void **)&g_ndk.writeInt32 },
        { "AParcel_writeInt64", (void **)&g_ndk.writeInt64 },
        { "AParcel_writeString", (void **)&g_ndk.writeString },
        { "AParcel_getDataPosition", (void **)&g_ndk.getDataPosition },
        { "AParcel_setDataPosition", (void **)&g_ndk.setDataPosition },
    };

    for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        *syms[i].slot = dlsym(lib, syms[i].name);
        if (!*syms[i].slot) {
            return false;
        }
    }
    return true;
}

static int32_t begin_parcelable(AParcel *out, int32_t *start_pos) {
    *start_pos = g_ndk.getDataPosition(out);
    return g_ndk.writeInt32(out, 0);
}

static int32_t end_parcelable(AParcel *out, int32_t start_pos) {
    int32_t end_pos = g_ndk.getDataPosition(out);
    int32_t st = g_ndk.setDataPosition(out, start_pos);
    if (st != STATUS_OK_VAL) return st;
    st = g_ndk.writeInt32(out, end_pos - start_pos);
    if (st != STATUS_OK_VAL) return st;
    return g_ndk.setDataPosition(out, end_pos);
}

static int32_t write_status(AParcel *out, AStatus *status) {
    if (!status) return -ENOMEM;
    int32_t rc = g_ndk.writeStatusHeader(out, status);
    g_ndk.AStatus_delete(status);
    return rc;
}

static int32_t write_support_info(AParcel *out) {
    int32_t s_pos = 0, c_pos = 0, h_pos = 0;
    int32_t rc;

    /* Non-null SupportInfo typed object marker */
    if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) return rc;
    if ((rc = begin_parcelable(out, &s_pos)) != STATUS_OK_VAL) return rc;

    if ((rc = g_ndk.writeBool(out, false)) != STATUS_OK_VAL) return rc; /* usesSessions */
    for (int i = 0; i < 5; i++) {
        /* boosts, modes, sessionHints, sessionModes, sessionTags */
        if ((rc = g_ndk.writeInt64(out, 0)) != STATUS_OK_VAL) return rc;
    }

    /* Non-null CompositionDataSupportInfo typed object */
    if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) return rc;
    if ((rc = begin_parcelable(out, &c_pos)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.writeBool(out, false)) != STATUS_OK_VAL) return rc;  /* isSupported */
    if ((rc = g_ndk.writeBool(out, false)) != STATUS_OK_VAL) return rc;  /* disableGpuFences */
    if ((rc = g_ndk.writeInt32(out, 0)) != STATUS_OK_VAL) return rc;     /* maxBatchSize */
    if ((rc = g_ndk.writeBool(out, false)) != STATUS_OK_VAL) return rc;  /* alwaysBatch */
    if ((rc = end_parcelable(out, c_pos)) != STATUS_OK_VAL) return rc;

    /* Non-null HeadroomSupportInfo typed object */
    if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) return rc;
    if ((rc = begin_parcelable(out, &h_pos)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.writeBool(out, false)) != STATUS_OK_VAL) return rc;  /* isCpuSupported */
    if ((rc = g_ndk.writeBool(out, false)) != STATUS_OK_VAL) return rc;  /* isGpuSupported */
    for (int i = 0; i < 7; i++) {
        /* cpuMinIntervalMillis, gpuMinIntervalMillis, cpuMinCalculationWindowMillis,
         * cpuMaxCalculationWindowMillis, gpuMinCalculationWindowMillis,
         * gpuMaxCalculationWindowMillis, cpuMaxTidCount */
        if ((rc = g_ndk.writeInt32(out, 0)) != STATUS_OK_VAL) return rc;
    }
    if ((rc = end_parcelable(out, h_pos)) != STATUS_OK_VAL) return rc;

    return end_parcelable(out, s_pos);
}

static void *ipower_on_create(void *args) {
    return args;
}

static void ipower_on_destroy(void *user_data) {
    (void)user_data;
}

static int32_t ipower_on_transact(AIBinder *binder, uint32_t code,
                                  const AParcel *in, AParcel *out) {
    (void)binder;
    (void)in;
    int32_t rc;

    switch (code) {
    case TX_SET_MODE:
    case TX_SET_BOOST:
    case TX_CLOSE_SESSION_CHANNEL:
    case TX_SEND_COMPOSITION_DATA:
    case TX_SEND_COMPOSITION_UPDATE:
        return STATUS_OK_VAL;

    case TX_IS_MODE_SUPPORTED:
    case TX_IS_BOOST_SUPPORTED:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeBool(out, false);

    case TX_GET_HINT_SESSION_PREFERRED_RATE:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt64(out, -1);

    case TX_GET_SUPPORT_INFO:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return write_support_info(out);

    case TX_CREATE_HINT_SESSION:
    case TX_CREATE_HINT_SESSION_WITH_CONFIG:
    case TX_GET_SESSION_CHANNEL:
    case TX_GET_CPU_HEADROOM:
    case TX_GET_GPU_HEADROOM:
        return write_status(out, g_ndk.AStatus_fromExceptionCode(EX_UNSUPPORTED_OP_VAL));

    case TX_GET_INTERFACE_HASH:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeString(out, IPOWER_HASH, (int32_t)(sizeof(IPOWER_HASH) - 1));

    case TX_GET_INTERFACE_VERSION:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 7);

    default:
        return STATUS_UNKNOWN_TX_VAL;
    }
}

int main(void) {
    void *lib = dlopen("/system/lib64/libbinder_ndk.so", RTLD_NOW);
    if (!lib || !resolve_ndk(lib)) {
        kmsg_log("<3>sailfish_power_hal: failed to load libbinder_ndk.so symbols\n");
        for (;;) pause();
    }

    AIBinder_Class *clazz = g_ndk.Class_define(IPOWER_DESC, ipower_on_create,
                                               ipower_on_destroy, ipower_on_transact);
    AIBinder *binder = clazz ? g_ndk.AIBinder_new(clazz, NULL) : NULL;
    if (!binder) {
        kmsg_log("<3>sailfish_power_hal: AIBinder_new failed\n");
        for (;;) pause();
    }

    g_ndk.markVintfStability(binder);

    while (g_ndk.addService(binder, IPOWER_INSTANCE) != STATUS_OK_VAL) {
        usleep(250000);
    }
    kmsg_log("<6>sailfish_power_hal: registered android.hardware.power.IPower/default (V7 stub)\n");

    g_ndk.setThreadPoolMaxThreadCount(4);
    g_ndk.startThreadPool();
    g_ndk.joinThreadPool();

    for (;;) pause();
    return 0;
}
