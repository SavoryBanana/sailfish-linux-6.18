#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* android.hardware.power.Boost */
enum {
    BOOST_INTERACTION = 0,
    BOOST_DISPLAY_UPDATE_IMMINENT = 1,
};

/* android.hardware.power.Mode */
enum {
    MODE_LAUNCH = 5,
    MODE_EXPENSIVE_RENDERING = 6,
    MODE_INTERACTIVE = 7,
    MODE_DISPLAY_INACTIVE = 9,
};

enum {
    STATUS_OK_VAL = 0,
    STATUS_UNKNOWN_TX_VAL = -EBADMSG,
    EX_UNSUPPORTED_OP_VAL = -7,
};

static const int64_t SUPPORTED_BOOSTS_MASK =
    (1LL << BOOST_INTERACTION) |
    (1LL << BOOST_DISPLAY_UPDATE_IMMINENT);

static const int64_t SUPPORTED_MODES_MASK =
    (1LL << MODE_LAUNCH) |
    (1LL << MODE_EXPENSIVE_RENDERING) |
    (1LL << MODE_INTERACTIVE) |
    (1LL << MODE_DISPLAY_INACTIVE);

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
    int32_t (*readInt32)(const AParcel *, int32_t *);
    int32_t (*readBool)(const AParcel *, bool *);
    int32_t (*getDataPosition)(const AParcel *);
    int32_t (*setDataPosition)(const AParcel *, int32_t);
} g_ndk;

static void kmsg_log(const char *msg) {
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)write(fd, msg, strlen(msg));
        close(fd);
    }
}

static void write_sysfs_str(const char *path, const char *val) {
    if (!path || !path[0]) return;
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)write(fd, val, strlen(val));
        close(fd);
    }
}

static bool read_sysfs_ll(const char *path, long long *out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    *out = strtoll(buf, NULL, 10);
    return true;
}

/* ---------------- Mainline sysfs power state ---------------- */
enum {
    PROFILE_SLEEP = 0,
    PROFILE_IDLE = 1,
    PROFILE_INTERACTION = 2,
    PROFILE_EXPENSIVE = 3,
    PROFILE_LAUNCH = 4,
};

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    char gpu_min_path[128];
    bool has_uclamp_topapp;
    bool interactive;
    bool display_inactive;
    bool expensive_rendering;
    int64_t boost_until_ns;
    int64_t launch_until_ns;
    int active_profile;
} g_pwr = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .interactive = true,
    .active_profile = -1,
};

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void discover_sysfs_nodes(void) {
    DIR *d = opendir("/sys/class/devfreq");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char max_path[160];
            long long max_hz = 0;
            snprintf(max_path, sizeof(max_path), "/sys/class/devfreq/%s/max_freq", de->d_name);
            if (read_sysfs_ll(max_path, &max_hz) && max_hz >= 300000000LL) {
                snprintf(g_pwr.gpu_min_path, sizeof(g_pwr.gpu_min_path),
                         "/sys/class/devfreq/%s/min_freq", de->d_name);
                /* Speed up GPU devfreq polling from 50ms -> 10ms */
                char poll_path[160];
                snprintf(poll_path, sizeof(poll_path),
                         "/sys/class/devfreq/%s/polling_interval", de->d_name);
                write_sysfs_str(poll_path, "10\n");
                break;
            }
        }
        closedir(d);
    }

    /* Tune mainline schedutil rate_limit_us to 1ms for fast frequency ramp */
    write_sysfs_str("/sys/devices/system/cpu/cpufreq/schedutil/rate_limit_us", "1000\n");
    write_sysfs_str("/sys/devices/system/cpu/cpufreq/policy0/schedutil/rate_limit_us", "1000\n");
    write_sysfs_str("/sys/devices/system/cpu/cpufreq/policy2/schedutil/rate_limit_us", "1000\n");

    /* Enable mainline uclamp latency-sensitive wakeup placement for UI tasks */
    write_sysfs_str("/dev/cpuctl/top-app/cpu.uclamp.latency_sensitive", "1\n");
    write_sysfs_str("/dev/cpuctl/foreground/cpu.uclamp.latency_sensitive", "1\n");

    g_pwr.has_uclamp_topapp = (access("/dev/cpuctl/top-app/cpu.uclamp.min", W_OK) == 0);
}

static void apply_profile_locked(int profile) {
    if (profile == g_pwr.active_profile) return;
    g_pwr.active_profile = profile;

    const char *p0_min = "307200\n";
    const char *p2_min = "307200\n";
    const char *gpu_min = "133000000\n";
    const char *uclamp_min = "0\n";

    switch (profile) {
    case PROFILE_LAUNCH:
        p0_min = "1516800\n";
        p2_min = "1900800\n";
        gpu_min = "401800000\n";
        uclamp_min = "85\n";
        break;
    case PROFILE_EXPENSIVE:
        p0_min = "1228800\n";
        p2_min = "1593600\n";
        gpu_min = "401800000\n";
        uclamp_min = "60\n";
        break;
    case PROFILE_INTERACTION:
        p0_min = "1228800\n";
        p2_min = "1593600\n";
        gpu_min = "401800000\n";
        uclamp_min = "50\n";
        break;
    case PROFILE_IDLE:
        uclamp_min = "10\n";
        break;
    case PROFILE_SLEEP:
    default:
        break;
    }

    write_sysfs_str("/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq", p0_min);
    write_sysfs_str("/sys/devices/system/cpu/cpufreq/policy2/scaling_min_freq", p2_min);
    if (g_pwr.gpu_min_path[0])
        write_sysfs_str(g_pwr.gpu_min_path, gpu_min);
    write_sysfs_str("/sys/bus/platform/devices/b00000.gpu/power/control",
                    (profile == PROFILE_SLEEP) ? "auto\n" : "on\n");
    if (g_pwr.has_uclamp_topapp)
        write_sysfs_str("/dev/cpuctl/top-app/cpu.uclamp.min", uclamp_min);
}

static int compute_profile_locked(int64_t now, int64_t *next_expiry_out) {
    int64_t next_exp = 0;

    if (!g_pwr.interactive || g_pwr.display_inactive) {
        g_pwr.boost_until_ns = 0;
        g_pwr.launch_until_ns = 0;
        *next_expiry_out = 0;
        return PROFILE_SLEEP;
    }

    if (g_pwr.launch_until_ns > now) {
        next_exp = g_pwr.launch_until_ns;
        *next_expiry_out = next_exp;
        return PROFILE_LAUNCH;
    }
    g_pwr.launch_until_ns = 0;

    if (g_pwr.expensive_rendering) {
        if (g_pwr.boost_until_ns > now)
            next_exp = g_pwr.boost_until_ns;
        *next_expiry_out = next_exp;
        return PROFILE_EXPENSIVE;
    }

    if (g_pwr.boost_until_ns > now) {
        *next_expiry_out = g_pwr.boost_until_ns;
        return PROFILE_INTERACTION;
    }
    g_pwr.boost_until_ns = 0;

    *next_expiry_out = 0;
    return PROFILE_IDLE;
}

static void *power_worker_fn(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_pwr.lock);
    for (;;) {
        int64_t now = mono_ns();
        int64_t next_exp = 0;
        int prof = compute_profile_locked(now, &next_exp);
        apply_profile_locked(prof);

        if (next_exp > now) {
            struct timespec ts;
            ts.tv_sec = (time_t)(next_exp / 1000000000LL);
            ts.tv_nsec = (long)(next_exp % 1000000000LL);
            (void)pthread_cond_timedwait(&g_pwr.cond, &g_pwr.lock, &ts);
        } else {
            (void)pthread_cond_wait(&g_pwr.cond, &g_pwr.lock);
        }
    }
    pthread_mutex_unlock(&g_pwr.lock);
    return NULL;
}

static void handle_set_boost(int32_t boost_type, int32_t duration_ms) {
    if (boost_type != BOOST_INTERACTION && boost_type != BOOST_DISPLAY_UPDATE_IMMINENT)
        return;

    int32_t ms = duration_ms;
    if (ms <= 0) ms = 250;
    if (ms < 80) ms = 80;
    if (ms > 1500) ms = 1500;

    int64_t until = mono_ns() + (int64_t)ms * 1000000LL;
    pthread_mutex_lock(&g_pwr.lock);
    if (g_pwr.interactive && !g_pwr.display_inactive) {
        if (until > g_pwr.boost_until_ns)
            g_pwr.boost_until_ns = until;
        pthread_cond_signal(&g_pwr.cond);
    }
    pthread_mutex_unlock(&g_pwr.lock);
}

static void handle_set_mode(int32_t mode_type, bool enabled) {
    int64_t now = mono_ns();
    pthread_mutex_lock(&g_pwr.lock);
    switch (mode_type) {
    case MODE_LAUNCH:
        g_pwr.launch_until_ns = enabled ? (now + 5000000000LL) : 0;
        break;
    case MODE_EXPENSIVE_RENDERING:
        g_pwr.expensive_rendering = enabled;
        break;
    case MODE_INTERACTIVE:
        g_pwr.interactive = enabled;
        if (enabled) {
            int64_t wake_boost = now + 350000000LL;
            if (wake_boost > g_pwr.boost_until_ns)
                g_pwr.boost_until_ns = wake_boost;
        }
        break;
    case MODE_DISPLAY_INACTIVE:
        g_pwr.display_inactive = enabled;
        break;
    default:
        pthread_mutex_unlock(&g_pwr.lock);
        return;
    }
    pthread_cond_signal(&g_pwr.cond);
    pthread_mutex_unlock(&g_pwr.lock);
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
        { "AParcel_writeBool", (void **)&g_ndk.writeBool },
        { "AParcel_writeInt32", (void **)&g_ndk.writeInt32 },
        { "AParcel_writeInt64", (void **)&g_ndk.writeInt64 },
        { "AParcel_writeString", (void **)&g_ndk.writeString },
        { "AParcel_readInt32", (void **)&g_ndk.readInt32 },
        { "AParcel_readBool", (void **)&g_ndk.readBool },
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
    if ((rc = g_ndk.writeInt64(out, SUPPORTED_BOOSTS_MASK)) != STATUS_OK_VAL) return rc; /* boosts */
    if ((rc = g_ndk.writeInt64(out, SUPPORTED_MODES_MASK)) != STATUS_OK_VAL) return rc;  /* modes */
    if ((rc = g_ndk.writeInt64(out, 0)) != STATUS_OK_VAL) return rc; /* sessionHints */
    if ((rc = g_ndk.writeInt64(out, 0)) != STATUS_OK_VAL) return rc; /* sessionModes */
    if ((rc = g_ndk.writeInt64(out, 0)) != STATUS_OK_VAL) return rc; /* sessionTags */

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
    int32_t rc;

    switch (code) {
    case TX_SET_MODE: {
        int32_t mode_type = 0;
        bool enabled = false;
        if (g_ndk.readInt32(in, &mode_type) == STATUS_OK_VAL &&
            g_ndk.readBool(in, &enabled) == STATUS_OK_VAL) {
            handle_set_mode(mode_type, enabled);
        }
        return STATUS_OK_VAL;
    }

    case TX_SET_BOOST: {
        int32_t boost_type = 0;
        int32_t duration_ms = 0;
        if (g_ndk.readInt32(in, &boost_type) == STATUS_OK_VAL &&
            g_ndk.readInt32(in, &duration_ms) == STATUS_OK_VAL) {
            handle_set_boost(boost_type, duration_ms);
        }
        return STATUS_OK_VAL;
    }

    case TX_CLOSE_SESSION_CHANNEL:
    case TX_SEND_COMPOSITION_DATA:
    case TX_SEND_COMPOSITION_UPDATE:
        return STATUS_OK_VAL;

    case TX_IS_MODE_SUPPORTED: {
        int32_t mode_type = -1;
        bool sup = false;
        if (g_ndk.readInt32(in, &mode_type) == STATUS_OK_VAL &&
            mode_type >= 0 && mode_type < 63) {
            sup = ((SUPPORTED_MODES_MASK & (1LL << mode_type)) != 0);
        }
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeBool(out, sup);
    }

    case TX_IS_BOOST_SUPPORTED: {
        int32_t boost_type = -1;
        bool sup = false;
        if (g_ndk.readInt32(in, &boost_type) == STATUS_OK_VAL &&
            boost_type >= 0 && boost_type < 63) {
            sup = ((SUPPORTED_BOOSTS_MASK & (1LL << boost_type)) != 0);
        }
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeBool(out, sup);
    }

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

    discover_sysfs_nodes();

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_pwr.cond, &cattr);
    pthread_condattr_destroy(&cattr);

    pthread_t worker;
    if (pthread_create(&worker, NULL, power_worker_fn, NULL) == 0) {
        pthread_detach(worker);
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

    char msg[256];
    snprintf(msg, sizeof(msg),
             "<6>sailfish_power_hal: registered IPower/default V7 (mainline cpufreq+devfreq gpu=%s uclamp=%d)\n",
             g_pwr.gpu_min_path[0] ? g_pwr.gpu_min_path : "none",
             (int)g_pwr.has_uclamp_topapp);
    kmsg_log(msg);

    g_ndk.setThreadPoolMaxThreadCount(4);
    g_ndk.startThreadPool();
    g_ndk.joinThreadPool();

    for (;;) pause();
    return 0;
}
