#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
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
    TX_GET_CAPABILITIES = 1,
    TX_OFF = 2,
    TX_ON = 3,
    TX_PERFORM = 4,
    TX_GET_SUPPORTED_EFFECTS = 5,
    TX_SET_AMPLITUDE = 6,
    TX_SET_EXTERNAL_CONTROL = 7,
    TX_GET_COMPOSITION_DELAY_MAX = 8,
    TX_GET_COMPOSITION_SIZE_MAX = 9,
    TX_GET_SUPPORTED_PRIMITIVES = 10,
    TX_GET_PRIMITIVE_DURATION = 11,
    TX_COMPOSE = 12,
    TX_GET_SUPPORTED_ALWAYS_ON_EFFECTS = 13,
    TX_ALWAYS_ON_ENABLE = 14,
    TX_ALWAYS_ON_DISABLE = 15,
    TX_GET_RESONANT_FREQUENCY = 16,
    TX_GET_Q_FACTOR = 17,
    TX_GET_FREQUENCY_RESOLUTION = 18,
    TX_GET_FREQUENCY_MINIMUM = 19,
    TX_GET_BANDWIDTH_AMPLITUDE_MAP = 20,
    TX_GET_PWLE_PRIMITIVE_DURATION_MAX = 21,
    TX_GET_PWLE_COMPOSITION_SIZE_MAX = 22,
    TX_GET_SUPPORTED_BRAKING = 23,
    TX_COMPOSE_PWLE = 24,
    TX_GET_INTERFACE_HASH = 16777214,
    TX_GET_INTERFACE_VERSION = 16777215,
};

enum {
    STATUS_OK_VAL = 0,
    STATUS_UNKNOWN_TX_VAL = -EBADMSG,
    EX_ILLEGAL_ARGUMENT = -3,
    EX_UNSUPPORTED_OP = -7,
    CAP_AMPLITUDE_CONTROL = (1 << 2),
};

static const char IVIB_DESC[] = "android.hardware.vibrator.IVibrator";
static const char IVIB_INSTANCE[] = "android.hardware.vibrator.IVibrator/default";
static const char IVIB_HASH[] = "ea8742d6993e1a82917da38b9938e537aa7fcb54";

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
    int32_t (*writeFloat)(AParcel *, float);
    int32_t (*writeString)(AParcel *, const char *, int32_t);
    int32_t (*readInt32)(const AParcel *, int32_t *);
    int32_t (*readByte)(const AParcel *, int8_t *);
    int32_t (*readFloat)(const AParcel *, float *);
} g_ndk;

static void kmsg_log(const char *msg) {
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)write(fd, msg, strlen(msg));
        close(fd);
    }
}

/* ---------------- Linux input FF_RUMBLE backend ---------------- */
static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int fd;
    int effect_id;
    float amplitude;
    bool playing;
    int64_t stop_at_ns;
    char dev_path[64];
} g_vib = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .fd = -1,
    .effect_id = -1,
    .amplitude = 0.75f,
};

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void find_haptics_dev_locked(void) {
    if (g_vib.fd >= 0) return;

    DIR *d = opendir("/dev/input");
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;

        char name[128] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
            (strstr(name, "haptics") || strstr(name, "vibrator") || strstr(name, "vibra"))) {
            g_vib.fd = fd;
            g_vib.effect_id = -1;
            snprintf(g_vib.dev_path, sizeof(g_vib.dev_path), "%s", path);
            break;
        }
        close(fd);
    }
    closedir(d);
}

static void hw_set_vib_locked(bool on, uint16_t duration_ms, float amp) {
    find_haptics_dev_locked();
    if (g_vib.fd < 0) return;

    if (!on) {
        if (g_vib.effect_id >= 0) {
            struct input_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = EV_FF;
            ev.code = (uint16_t)g_vib.effect_id;
            ev.value = 0;
            (void)write(g_vib.fd, &ev, sizeof(ev));
        }
        g_vib.playing = false;
        return;
    }

    if (amp < 0.05f) amp = 0.05f;
    if (amp > 1.0f) amp = 1.0f;

    struct ff_effect eff;
    memset(&eff, 0, sizeof(eff));
    eff.type = FF_RUMBLE;
    eff.id = (int16_t)g_vib.effect_id;
    eff.u.rumble.strong_magnitude = (uint16_t)(amp * 65535.0f);
    eff.u.rumble.weak_magnitude = eff.u.rumble.strong_magnitude;
    eff.replay.length = duration_ms ? duration_ms : 1000;
    eff.replay.delay = 0;

    if (ioctl(g_vib.fd, EVIOCSFF, &eff) == 0) {
        g_vib.effect_id = eff.id;
        struct input_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_FF;
        ev.code = (uint16_t)eff.id;
        ev.value = 1;
        (void)write(g_vib.fd, &ev, sizeof(ev));
        g_vib.playing = true;
    }
}

static void *vib_worker_fn(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_vib.lock);
    for (;;) {
        int64_t now = mono_ns();
        if (g_vib.playing && g_vib.stop_at_ns > 0) {
            if (now >= g_vib.stop_at_ns) {
                g_vib.stop_at_ns = 0;
                hw_set_vib_locked(false, 0, 0.0f);
                continue;
            }
            struct timespec ts;
            ts.tv_sec = (time_t)(g_vib.stop_at_ns / 1000000000LL);
            ts.tv_nsec = (long)(g_vib.stop_at_ns % 1000000000LL);
            (void)pthread_cond_timedwait(&g_vib.cond, &g_vib.lock, &ts);
        } else {
            (void)pthread_cond_wait(&g_vib.cond, &g_vib.lock);
        }
    }
    pthread_mutex_unlock(&g_vib.lock);
    return NULL;
}

static void vib_start(int32_t ms, float amp) {
    if (ms <= 0) return;
    if (ms > 30000) ms = 30000;
    uint16_t hw_ms = (ms > 30000) ? 30000 : (uint16_t)ms;

    pthread_mutex_lock(&g_vib.lock);
    g_vib.stop_at_ns = mono_ns() + (int64_t)ms * 1000000LL;
    hw_set_vib_locked(true, hw_ms, amp);
    pthread_cond_signal(&g_vib.cond);
    pthread_mutex_unlock(&g_vib.lock);
}

static void vib_stop(void) {
    pthread_mutex_lock(&g_vib.lock);
    g_vib.stop_at_ns = 0;
    hw_set_vib_locked(false, 0, 0.0f);
    pthread_cond_signal(&g_vib.cond);
    pthread_mutex_unlock(&g_vib.lock);
}

/* ---------------- NDK Binder helpers ---------------- */
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
        { "AParcel_writeFloat", (void **)&g_ndk.writeFloat },
        { "AParcel_writeString", (void **)&g_ndk.writeString },
        { "AParcel_readInt32", (void **)&g_ndk.readInt32 },
        { "AParcel_readByte", (void **)&g_ndk.readByte },
        { "AParcel_readFloat", (void **)&g_ndk.readFloat },
    };

    for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        *syms[i].slot = dlsym(lib, syms[i].name);
        if (!*syms[i].slot) return false;
    }
    return true;
}

static int32_t write_status(AParcel *out, AStatus *status) {
    if (!status) return -ENOMEM;
    int32_t rc = g_ndk.writeStatusHeader(out, status);
    g_ndk.AStatus_delete(status);
    return rc;
}

static int32_t write_int32_array(AParcel *out, const int32_t *vals, int32_t count) {
    int32_t rc = g_ndk.writeInt32(out, count);
    if (rc != STATUS_OK_VAL) return rc;
    for (int32_t i = 0; i < count; i++) {
        rc = g_ndk.writeInt32(out, vals[i]);
        if (rc != STATUS_OK_VAL) return rc;
    }
    return STATUS_OK_VAL;
}

static void *ivib_on_create(void *args) { return args; }
static void ivib_on_destroy(void *user_data) { (void)user_data; }

static int32_t ivib_on_transact(AIBinder *binder, uint32_t code,
                                const AParcel *in, AParcel *out) {
    (void)binder;
    int32_t rc;

    switch (code) {
    case TX_GET_CAPABILITIES:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, CAP_AMPLITUDE_CONTROL);

    case TX_OFF:
        vib_stop();
        return write_status(out, g_ndk.AStatus_newOk());

    case TX_ON: {
        int32_t timeout_ms = 0;
        (void)g_ndk.readInt32(in, &timeout_ms);
        float amp;
        pthread_mutex_lock(&g_vib.lock);
        amp = g_vib.amplitude;
        pthread_mutex_unlock(&g_vib.lock);
        vib_start(timeout_ms, amp);
        return write_status(out, g_ndk.AStatus_newOk());
    }

    case TX_PERFORM: {
        int32_t effect = 0;
        int8_t strength = 1;
        (void)g_ndk.readInt32(in, &effect);
        (void)g_ndk.readByte(in, &strength);

        int32_t ms = 0;
        switch (effect) {
        case 0: ms = 16; break;  /* CLICK */
        case 1: ms = 24; break;  /* DOUBLE_CLICK */
        case 2: ms = 10; break;  /* TICK */
        case 3: ms = 24; break;  /* THUD */
        case 4: ms = 14; break;  /* POP */
        case 5: ms = 28; break;  /* HEAVY_CLICK */
        case 21: ms = 8; break;  /* TEXTURE_TICK */
        default:
            return write_status(out, g_ndk.AStatus_fromExceptionCode(EX_UNSUPPORTED_OP));
        }

        float amp = 0.70f;
        if (strength == 0) amp = 0.45f;      /* LIGHT */
        else if (strength == 2) amp = 0.95f; /* STRONG */

        vib_start(ms, amp);
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, ms);
    }

    case TX_GET_SUPPORTED_EFFECTS: {
        static const int32_t effects[] = { 0, 1, 2, 3, 4, 5, 21 };
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return write_int32_array(out, effects, (int32_t)(sizeof(effects) / sizeof(effects[0])));
    }

    case TX_SET_AMPLITUDE: {
        float amp = 0.0f;
        if (g_ndk.readFloat(in, &amp) != STATUS_OK_VAL || amp <= 0.0f || amp > 1.0f) {
            return write_status(out, g_ndk.AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
        }
        pthread_mutex_lock(&g_vib.lock);
        g_vib.amplitude = amp;
        if (g_vib.playing) {
            int64_t rem_ns = g_vib.stop_at_ns - mono_ns();
            uint16_t rem_ms = (rem_ns > 1000000LL) ? (uint16_t)(rem_ns / 1000000LL) : 20;
            hw_set_vib_locked(true, rem_ms, amp);
        }
        pthread_mutex_unlock(&g_vib.lock);
        return write_status(out, g_ndk.AStatus_newOk());
    }

    case TX_GET_SUPPORTED_PRIMITIVES:
    case TX_GET_SUPPORTED_ALWAYS_ON_EFFECTS:
    case TX_GET_SUPPORTED_BRAKING:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return write_int32_array(out, NULL, 0);

    case TX_GET_RESONANT_FREQUENCY:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeFloat(out, 200.0f);

    case TX_SET_EXTERNAL_CONTROL:
    case TX_GET_COMPOSITION_DELAY_MAX:
    case TX_GET_COMPOSITION_SIZE_MAX:
    case TX_GET_PRIMITIVE_DURATION:
    case TX_COMPOSE:
    case TX_ALWAYS_ON_ENABLE:
    case TX_ALWAYS_ON_DISABLE:
    case TX_GET_Q_FACTOR:
    case TX_GET_FREQUENCY_RESOLUTION:
    case TX_GET_FREQUENCY_MINIMUM:
    case TX_GET_BANDWIDTH_AMPLITUDE_MAP:
    case TX_GET_PWLE_PRIMITIVE_DURATION_MAX:
    case TX_GET_PWLE_COMPOSITION_SIZE_MAX:
    case TX_COMPOSE_PWLE:
        return write_status(out, g_ndk.AStatus_fromExceptionCode(EX_UNSUPPORTED_OP));

    case TX_GET_INTERFACE_HASH:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeString(out, IVIB_HASH, (int32_t)(sizeof(IVIB_HASH) - 1));

    case TX_GET_INTERFACE_VERSION:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 2);

    default:
        return STATUS_UNKNOWN_TX_VAL;
    }
}

int main(void) {
    void *lib = dlopen("/system/lib64/libbinder_ndk.so", RTLD_NOW);
    if (!lib || !resolve_ndk(lib)) {
        kmsg_log("<3>sailfish_vibrator_hal: failed to load libbinder_ndk.so\n");
        for (;;) pause();
    }

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_vib.cond, &cattr);
    pthread_condattr_destroy(&cattr);

    pthread_mutex_lock(&g_vib.lock);
    find_haptics_dev_locked();
    pthread_mutex_unlock(&g_vib.lock);

    pthread_t worker;
    if (pthread_create(&worker, NULL, vib_worker_fn, NULL) == 0) {
        pthread_detach(worker);
    }

    AIBinder_Class *clazz = g_ndk.Class_define(IVIB_DESC, ivib_on_create,
                                               ivib_on_destroy, ivib_on_transact);
    AIBinder *binder = clazz ? g_ndk.AIBinder_new(clazz, NULL) : NULL;
    if (!binder) {
        kmsg_log("<3>sailfish_vibrator_hal: AIBinder_new failed\n");
        for (;;) pause();
    }

    g_ndk.markVintfStability(binder);
    while (g_ndk.addService(binder, IVIB_INSTANCE) != STATUS_OK_VAL) {
        usleep(250000);
    }

    char msg[192];
    snprintf(msg, sizeof(msg),
             "<6>sailfish_vibrator_hal: registered IVibrator/default V2 (dev=%s)\n",
             g_vib.dev_path[0] ? g_vib.dev_path : "deferred");
    kmsg_log(msg);

    g_ndk.setThreadPoolMaxThreadCount(4);
    g_ndk.startThreadPool();
    g_ndk.joinThreadPool();

    for (;;) pause();
    return 0;
}
