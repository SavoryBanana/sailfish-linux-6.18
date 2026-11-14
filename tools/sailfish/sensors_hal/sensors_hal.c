/*
 * Minimal AIDL android.hardware.sensors.ISensors/default (V3) stub for the
 * Pixel 1 (sailfish) bring-up.
 *
 * The 6.18 kernel has no nanohub driver, so the vendor HIDL
 * android.hardware.sensors@1.0-service is replaced with pause_stub. Without
 * any sensors HAL, SensorDevice in system_server blocks forever waiting for
 * the HIDL service, "sensorservice" never gets published and
 * SystemSensorManager.nativeCreate() trips the Watchdog.
 *
 * SensorDevice tries the AIDL HAL first (AServiceManager_isDeclared +
 * AServiceManager_waitForService). This stub registers ISensors/default and
 * reports an empty sensor list, so SensorService initialises with 0 sensors.
 */
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

/* Transaction codes from BpSensors in android.hardware.sensors-V3-ndk.so */
enum {
    TX_ACTIVATE = 1,
    TX_BATCH = 2,
    TX_CONFIG_DIRECT_REPORT = 3,
    TX_FLUSH = 4,
    TX_GET_SENSORS_LIST = 5,
    TX_INITIALIZE = 6,
    TX_INJECT_SENSOR_DATA = 7,
    TX_REGISTER_DIRECT_CHANNEL = 8,
    TX_SET_OPERATION_MODE = 9,
    TX_UNREGISTER_DIRECT_CHANNEL = 10,
    TX_GET_INTERFACE_HASH = 16777214,
    TX_GET_INTERFACE_VERSION = 16777215,
};

enum {
    STATUS_OK_VAL = 0,
    STATUS_UNKNOWN_TX_VAL = -EBADMSG,
    EX_UNSUPPORTED_OP_VAL = -7,
};

static const char ISENSORS_DESC[] = "android.hardware.sensors.ISensors";
static const char ISENSORS_INSTANCE[] = "android.hardware.sensors.ISensors/default";
static const char ISENSORS_HASH[] = "b051523be857524c99057b69f253a7d0bd295400";

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
    int32_t (*writeInt32)(AParcel *, int32_t);
    int32_t (*writeString)(AParcel *, const char *, int32_t);
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
        { "AParcel_writeInt32", (void **)&g_ndk.writeInt32 },
        { "AParcel_writeString", (void **)&g_ndk.writeString },
    };

    for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        *syms[i].slot = dlsym(lib, syms[i].name);
        if (!*syms[i].slot) {
            return false;
        }
    }
    return true;
}

static int32_t write_status(AParcel *out, AStatus *status) {
    if (!status) return -ENOMEM;
    int32_t rc = g_ndk.writeStatusHeader(out, status);
    g_ndk.AStatus_delete(status);
    return rc;
}

static void *isensors_on_create(void *args) {
    return args;
}

static void isensors_on_destroy(void *user_data) {
    (void)user_data;
}

static int32_t isensors_on_transact(AIBinder *binder, uint32_t code,
                                    const AParcel *in, AParcel *out) {
    (void)binder;
    (void)in;
    int32_t rc;

    switch (code) {
    /* void methods (none of ISensors is oneway): reply with an OK status */
    case TX_ACTIVATE:
    case TX_BATCH:
    case TX_FLUSH:
    case TX_INITIALIZE:          /* FMQ descriptors + callback are ignored */
    case TX_INJECT_SENSOR_DATA:
    case TX_SET_OPERATION_MODE:
    case TX_UNREGISTER_DIRECT_CHANNEL:
        return write_status(out, g_ndk.AStatus_newOk());

    case TX_GET_SENSORS_LIST:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 0); /* SensorInfo[] of length 0 */

    case TX_CONFIG_DIRECT_REPORT:
    case TX_REGISTER_DIRECT_CHANNEL:
        return write_status(out, g_ndk.AStatus_fromExceptionCode(EX_UNSUPPORTED_OP_VAL));

    case TX_GET_INTERFACE_HASH:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeString(out, ISENSORS_HASH, (int32_t)(sizeof(ISENSORS_HASH) - 1));

    case TX_GET_INTERFACE_VERSION:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 3);

    default:
        return STATUS_UNKNOWN_TX_VAL;
    }
}

int main(void) {
    void *lib = dlopen("/system/lib64/libbinder_ndk.so", RTLD_NOW);
    if (!lib || !resolve_ndk(lib)) {
        kmsg_log("<3>sailfish_sensors_hal: failed to load libbinder_ndk.so symbols\n");
        for (;;) pause();
    }

    AIBinder_Class *clazz = g_ndk.Class_define(ISENSORS_DESC, isensors_on_create,
                                               isensors_on_destroy, isensors_on_transact);
    AIBinder *binder = clazz ? g_ndk.AIBinder_new(clazz, NULL) : NULL;
    if (!binder) {
        kmsg_log("<3>sailfish_sensors_hal: AIBinder_new failed\n");
        for (;;) pause();
    }

    g_ndk.markVintfStability(binder);

    while (g_ndk.addService(binder, ISENSORS_INSTANCE) != STATUS_OK_VAL) {
        usleep(250000);
    }
    kmsg_log("<6>sailfish_sensors_hal: registered android.hardware.sensors.ISensors/default (V3 stub, 0 sensors)\n");

    g_ndk.setThreadPoolMaxThreadCount(4);
    g_ndk.startThreadPool();
    g_ndk.joinThreadPool();

    for (;;) pause();
    return 0;
}
