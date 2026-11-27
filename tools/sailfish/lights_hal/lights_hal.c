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
    TX_SET_LIGHT_STATE = 1,
    TX_GET_LIGHTS = 2,
    TX_GET_INTERFACE_HASH = 16777214,
    TX_GET_INTERFACE_VERSION = 16777215,
};

/* android.hardware.light.LightType */
enum {
    LIGHT_TYPE_BATTERY = 3,
    LIGHT_TYPE_NOTIFICATIONS = 4,
    LIGHT_TYPE_ATTENTION = 5,
};

enum {
    STATUS_OK_VAL = 0,
    STATUS_UNKNOWN_TX_VAL = -EBADMSG,
    EX_UNSUPPORTED_OP = -7,
};

static const char ILIGHTS_DESC[] = "android.hardware.light.ILights";
static const char ILIGHTS_INSTANCE[] = "android.hardware.light.ILights/default";
static const char ILIGHTS_HASH[] = "33fec8401b6e66bddaeff251e1a2a0f4fa0d3bee";

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
    int32_t (*writeByte)(AParcel *, int8_t);
    int32_t (*writeString)(AParcel *, const char *, int32_t);
    int32_t (*readInt32)(const AParcel *, int32_t *);
    int32_t (*readByte)(const AParcel *, int8_t *);
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

static void write_sysfs(const char *path, const char *val) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)write(fd, val, strlen(val));
        close(fd);
    }
}

/* ---------------- Mainline multicolor RGB LED backend ---------------- */
static struct {
    pthread_mutex_t lock;
    char led_dir[128];
    int idx_r, idx_g, idx_b;
    uint32_t colors[3]; /* 0=battery, 1=notifications, 2=attention */
} g_led = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .idx_r = 2,
    .idx_g = 1,
    .idx_b = 0,
};

static void discover_rgb_led(void) {
    DIR *d = opendir("/sys/class/leds");
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char idx_path[192];
        snprintf(idx_path, sizeof(idx_path), "/sys/class/leds/%s/multi_index", de->d_name);
        int fd = open(idx_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;

        char buf[128] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;

        snprintf(g_led.led_dir, sizeof(g_led.led_dir), "/sys/class/leds/%s", de->d_name);
        /* Parse space-separated channel names (e.g. "blue green red") */
        int pos = 0;
        char *saveptr = NULL;
        for (char *tok = strtok_r(buf, " \t\r\n", &saveptr);
             tok && pos < 3;
             tok = strtok_r(NULL, " \t\r\n", &saveptr), pos++) {
            if (strcmp(tok, "red") == 0) g_led.idx_r = pos;
            else if (strcmp(tok, "green") == 0) g_led.idx_g = pos;
            else if (strcmp(tok, "blue") == 0) g_led.idx_b = pos;
        }
        break;
    }
    closedir(d);
}

static void apply_rgb_locked(void) {
    if (!g_led.led_dir[0]) discover_rgb_led();
    if (!g_led.led_dir[0]) return;

    /* Priority: attention > notifications > battery */
    uint32_t argb = g_led.colors[2];
    if ((argb & 0x00ffffffU) == 0) argb = g_led.colors[1];
    if ((argb & 0x00ffffffU) == 0) argb = g_led.colors[0];

    int r = (int)((argb >> 16) & 0xffU);
    int g = (int)((argb >> 8) & 0xffU);
    int b = (int)(argb & 0xffU);

    int vals[3] = { 0, 0, 0 };
    if (g_led.idx_r >= 0 && g_led.idx_r < 3) vals[g_led.idx_r] = r;
    if (g_led.idx_g >= 0 && g_led.idx_g < 3) vals[g_led.idx_g] = g;
    if (g_led.idx_b >= 0 && g_led.idx_b < 3) vals[g_led.idx_b] = b;

    char path[192], buf[48];
    snprintf(path, sizeof(path), "%s/multi_intensity", g_led.led_dir);
    snprintf(buf, sizeof(buf), "%d %d %d\n", vals[0], vals[1], vals[2]);
    write_sysfs(path, buf);

    snprintf(path, sizeof(path), "%s/brightness", g_led.led_dir);
    write_sysfs(path, (r || g || b) ? "255\n" : "0\n");
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
        { "AParcel_writeByte", (void **)&g_ndk.writeByte },
        { "AParcel_writeString", (void **)&g_ndk.writeString },
        { "AParcel_readInt32", (void **)&g_ndk.readInt32 },
        { "AParcel_readByte", (void **)&g_ndk.readByte },
        { "AParcel_getDataPosition", (void **)&g_ndk.getDataPosition },
        { "AParcel_setDataPosition", (void **)&g_ndk.setDataPosition },
    };

    for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        *syms[i].slot = dlsym(lib, syms[i].name);
        if (!*syms[i].slot) return false;
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

static int32_t write_hw_light(AParcel *out, int32_t id, int32_t ordinal, int8_t type) {
    int32_t pos = 0, rc;
    if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) return rc; /* non-null */
    if ((rc = begin_parcelable(out, &pos)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.writeInt32(out, id)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.writeInt32(out, ordinal)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.writeByte(out, type)) != STATUS_OK_VAL) return rc;
    return end_parcelable(out, pos);
}

static void *ilights_on_create(void *args) { return args; }
static void ilights_on_destroy(void *user_data) { (void)user_data; }

static int32_t ilights_on_transact(AIBinder *binder, uint32_t code,
                                   const AParcel *in, AParcel *out) {
    (void)binder;
    int32_t rc;

    switch (code) {
    case TX_SET_LIGHT_STATE: {
        int32_t id = -1, non_null = 0, psize = 0, color = 0;
        if (g_ndk.readInt32(in, &id) != STATUS_OK_VAL)
            return write_status(out, g_ndk.AStatus_fromExceptionCode(EX_UNSUPPORTED_OP));
        if (g_ndk.readInt32(in, &non_null) == STATUS_OK_VAL && non_null != 0) {
            if (g_ndk.readInt32(in, &psize) == STATUS_OK_VAL && psize >= 4) {
                (void)g_ndk.readInt32(in, &color);
            }
        }
        if (id < 0 || id > 2)
            return write_status(out, g_ndk.AStatus_fromExceptionCode(EX_UNSUPPORTED_OP));

        pthread_mutex_lock(&g_led.lock);
        g_led.colors[id] = (uint32_t)color;
        apply_rgb_locked();
        pthread_mutex_unlock(&g_led.lock);
        return write_status(out, g_ndk.AStatus_newOk());
    }

    case TX_GET_LIGHTS:
        if ((rc = write_status(out, g_ndk.AStatus_newOk())) != STATUS_OK_VAL) return rc;
        if ((rc = g_ndk.writeInt32(out, 3)) != STATUS_OK_VAL) return rc;
        if ((rc = write_hw_light(out, 0, 0, LIGHT_TYPE_BATTERY)) != STATUS_OK_VAL) return rc;
        if ((rc = write_hw_light(out, 1, 0, LIGHT_TYPE_NOTIFICATIONS)) != STATUS_OK_VAL) return rc;
        return write_hw_light(out, 2, 0, LIGHT_TYPE_ATTENTION);

    case TX_GET_INTERFACE_HASH:
        if ((rc = write_status(out, g_ndk.AStatus_newOk())) != STATUS_OK_VAL) return rc;
        return g_ndk.writeString(out, ILIGHTS_HASH, (int32_t)(sizeof(ILIGHTS_HASH) - 1));

    case TX_GET_INTERFACE_VERSION:
        if ((rc = write_status(out, g_ndk.AStatus_newOk())) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 2);

    default:
        return STATUS_UNKNOWN_TX_VAL;
    }
}

int main(void) {
    void *lib = dlopen("/system/lib64/libbinder_ndk.so", RTLD_NOW);
    if (!lib || !resolve_ndk(lib)) {
        kmsg_log("<3>sailfish_lights_hal: failed to load libbinder_ndk.so\n");
        for (;;) pause();
    }

    discover_rgb_led();

    AIBinder_Class *clazz = g_ndk.Class_define(ILIGHTS_DESC, ilights_on_create,
                                               ilights_on_destroy, ilights_on_transact);
    AIBinder *binder = clazz ? g_ndk.AIBinder_new(clazz, NULL) : NULL;
    if (!binder) {
        kmsg_log("<3>sailfish_lights_hal: AIBinder_new failed\n");
        for (;;) pause();
    }

    g_ndk.markVintfStability(binder);
    while (g_ndk.addService(binder, ILIGHTS_INSTANCE) != STATUS_OK_VAL) {
        usleep(250000);
    }

    char msg[192];
    snprintf(msg, sizeof(msg),
             "<6>sailfish_lights_hal: registered ILights/default V2 (led=%s r=%d g=%d b=%d)\n",
             g_led.led_dir[0] ? g_led.led_dir : "none",
             g_led.idx_r, g_led.idx_g, g_led.idx_b);
    kmsg_log(msg);

    g_ndk.setThreadPoolMaxThreadCount(4);
    g_ndk.startThreadPool();
    g_ndk.joinThreadPool();

    for (;;) pause();
    return 0;
}
