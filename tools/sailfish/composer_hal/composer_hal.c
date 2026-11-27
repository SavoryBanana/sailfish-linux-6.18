#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "../gralloc_hal/sailfish_gralloc.h"

/* ---------------- NDK Binder minimal ABI ---------------- */
typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;

typedef void *(*AIBinder_Class_onCreate)(void *args);
typedef void (*AIBinder_Class_onDestroy)(void *userData);
typedef int32_t (*AIBinder_Class_onTransact)(AIBinder *binder, uint32_t code,
                                             const AParcel *in, AParcel *out);
typedef bool (*AParcel_int32ArrayAllocator)(void *arrayData, int32_t length, int32_t **outBuffer);

enum {
    STATUS_OK_VAL         = 0,
    STATUS_UNKNOWN_TX_VAL = -EBADMSG,
    FLAG_ONEWAY           = 1,
    HWC3_EX_BAD_CONFIG    = 1,
    HWC3_EX_BAD_DISPLAY   = 2,
    HWC3_EX_BAD_LAYER     = 3,
    HWC3_EX_BAD_PARAMETER = 4,
    HWC3_EX_NO_RESOURCES  = 6,
    HWC3_EX_UNSUPPORTED   = 8,
};

/* IComposer transactions */
enum {
    TX_COMP_CREATE_CLIENT        = 1,
    TX_COMP_GET_CAPABILITIES     = 2,
    TX_GET_INTERFACE_HASH        = 16777214,
    TX_GET_INTERFACE_VERSION     = 16777215,
};

/* IComposerClient transactions (verified against composer3-V6-ndk.so) */
enum {
    TX_CLI_CREATE_LAYER                       = 1,
    TX_CLI_CREATE_VIRTUAL_DISPLAY             = 2,
    TX_CLI_DESTROY_LAYER                      = 3,
    TX_CLI_DESTROY_VIRTUAL_DISPLAY            = 4,
    TX_CLI_EXECUTE_COMMANDS                   = 5,
    TX_CLI_GET_ACTIVE_CONFIG                  = 6,
    TX_CLI_GET_COLOR_MODES                    = 7,
    TX_CLI_GET_DATASPACE_SATURATION_MATRIX    = 8,
    TX_CLI_GET_DISPLAY_ATTRIBUTE              = 9,
    TX_CLI_GET_DISPLAY_CAPABILITIES           = 10,
    TX_CLI_GET_DISPLAY_CONFIGS                = 11,
    TX_CLI_GET_DISPLAY_CONNECTION_TYPE        = 12,
    TX_CLI_GET_DISPLAY_IDENTIFICATION_DATA    = 13,
    TX_CLI_GET_DISPLAY_NAME                   = 14,
    TX_CLI_GET_DISPLAY_VSYNC_PERIOD           = 15,
    TX_CLI_GET_DISPLAYED_CONTENT_SAMPLE       = 16,
    TX_CLI_GET_DISPLAYED_CONTENT_SAMPLING_ATTR= 17,
    TX_CLI_GET_DISPLAY_PHYSICAL_ORIENTATION   = 18,
    TX_CLI_GET_HDR_CAPABILITIES               = 19,
    TX_CLI_GET_MAX_VIRTUAL_DISPLAY_COUNT      = 20,
    TX_CLI_GET_PER_FRAME_METADATA_KEYS        = 21,
    TX_CLI_GET_READBACK_BUFFER_ATTRIBUTES     = 22,
    TX_CLI_GET_READBACK_BUFFER_FENCE          = 23,
    TX_CLI_GET_RENDER_INTENTS                 = 24,
    TX_CLI_GET_SUPPORTED_CONTENT_TYPES        = 25,
    TX_CLI_GET_DISPLAY_DECORATION_SUPPORT     = 26,
    TX_CLI_REGISTER_CALLBACK                  = 27,
    TX_CLI_SET_ACTIVE_CONFIG                  = 28,
    TX_CLI_SET_ACTIVE_CONFIG_WITH_CONSTRAINTS = 29,
    TX_CLI_SET_BOOT_DISPLAY_CONFIG            = 30,
    TX_CLI_CLEAR_BOOT_DISPLAY_CONFIG          = 31,
    TX_CLI_GET_PREFERRED_BOOT_DISPLAY_CONFIG  = 32,
    TX_CLI_SET_AUTO_LOW_LATENCY_MODE          = 33,
    TX_CLI_SET_CLIENT_TARGET_SLOT_COUNT       = 34,
    TX_CLI_SET_COLOR_MODE                     = 35,
    TX_CLI_SET_CONTENT_TYPE                   = 36,
    TX_CLI_SET_DISPLAYED_CONTENT_SAMPLING_EN  = 37,
    TX_CLI_SET_POWER_MODE                     = 38,
    TX_CLI_SET_READBACK_BUFFER                = 39,
    TX_CLI_SET_VSYNC_ENABLED                  = 40,
    TX_CLI_SET_IDLE_TIMER_ENABLED             = 41,
    TX_CLI_GET_OVERLAY_SUPPORT                = 42,
    TX_CLI_GET_HDR_CONVERSION_CAPABILITIES    = 43,
    TX_CLI_SET_HDR_CONVERSION_STRATEGY        = 44,
    TX_CLI_SET_REFRESH_RATE_DEBUG_ENABLED     = 45,
    TX_CLI_GET_DISPLAY_CONFIGURATIONS         = 46,
    TX_CLI_NOTIFY_EXPECTED_PRESENT            = 47,
    TX_CLI_GET_MAX_LAYER_PICTURE_PROFILES     = 48,
};

/* IComposerCallback transactions */
enum {
    TX_CB_ON_HOTPLUG = 1,
    TX_CB_ON_VSYNC   = 4,
};

/* CommandResultPayload union tags */
enum {
    CRP_TAG_ERROR                     = 0,
    CRP_TAG_CHANGED_COMPOSITION_TYPES = 1,
    CRP_TAG_DISPLAY_REQUESTS          = 2,
    CRP_TAG_PRESENT_FENCE             = 3,
    CRP_TAG_RELEASE_FENCES            = 4,
    CRP_TAG_PRESENT_OR_VALIDATE       = 5,
};

/* DisplayAttribute enum */
enum {
    DISP_ATTR_WIDTH        = 1,
    DISP_ATTR_HEIGHT       = 2,
    DISP_ATTR_VSYNC_PERIOD = 3,
    DISP_ATTR_DPI_X        = 4,
    DISP_ATTR_DPI_Y        = 5,
    DISP_ATTR_CONFIG_GROUP = 7,
};

/* PowerMode enum */
enum {
    POWER_MODE_OFF          = 0,
    POWER_MODE_DOZE         = 1,
    POWER_MODE_ON           = 2,
    POWER_MODE_DOZE_SUSPEND = 3,
    POWER_MODE_ON_SUSPEND   = 4,
};

/* Composition enum */
enum {
    COMP_INVALID     = 0,
    COMP_CLIENT      = 1,
    COMP_DEVICE      = 2,
    COMP_SOLID_COLOR = 3,
    COMP_CURSOR      = 4,
    COMP_SIDEBAND    = 5,
};

/* BlendMode enum (HWC3) */
enum {
    BLEND_INVALID       = 0,
    BLEND_NONE          = 1,
    BLEND_PREMULTIPLIED = 2,
    BLEND_COVERAGE      = 3,
};

static const char ICOMP_DESC[]     = "android.hardware.graphics.composer3.IComposer";
static const char ICOMP_INSTANCE[] = "android.hardware.graphics.composer3.IComposer/default";
static const char ICLI_DESC[]      = "android.hardware.graphics.composer3.IComposerClient";
static const char ICB_DESC[]       = "android.hardware.graphics.composer3.IComposerCallback";
static const char ICOMP_HASH[]     = "07c2ff93a65553c0f274ab0f688fd55f42402fac";
static const int64_t PRIMARY_DISPLAY_ID = 1;

static struct {
    AIBinder_Class *(*Class_define)(const char *, AIBinder_Class_onCreate,
                                    AIBinder_Class_onDestroy,
                                    AIBinder_Class_onTransact);
    AIBinder *(*AIBinder_new)(const AIBinder_Class *, void *);
    bool (*associateClass)(AIBinder *, const AIBinder_Class *);
    void (*markVintfStability)(AIBinder *);
    void (*incStrong)(AIBinder *);
    void (*decStrong)(AIBinder *);
    int32_t (*prepareTransaction)(AIBinder *, AParcel **);
    int32_t (*transact)(AIBinder *, uint32_t, AParcel **, AParcel **, uint32_t);
    int32_t (*addService)(AIBinder *, const char *);
    bool (*setThreadPoolMaxThreadCount)(uint32_t);
    void (*startThreadPool)(void);
    void (*joinThreadPool)(void);
    AStatus *(*AStatus_newOk)(void);
    AStatus *(*AStatus_fromServiceSpecificError)(int32_t);
    void (*AStatus_delete)(AStatus *);
    void (*AParcel_delete)(AParcel *);
    int32_t (*writeStatusHeader)(AParcel *, const AStatus *);
    int32_t (*readBool)(const AParcel *, bool *);
    int32_t (*readInt32)(const AParcel *, int32_t *);
    int32_t (*readInt64)(const AParcel *, int64_t *);
    int32_t (*readFloat)(const AParcel *, float *);
    int32_t (*readInt32Array)(const AParcel *, void *, AParcel_int32ArrayAllocator);
    int32_t (*readStrongBinder)(const AParcel *, AIBinder **);
    int32_t (*readParcelFileDescriptor)(const AParcel *, int *);
    int32_t (*writeBool)(AParcel *, bool);
    int32_t (*writeByte)(AParcel *, int8_t);
    int32_t (*writeInt32)(AParcel *, int32_t);
    int32_t (*writeInt64)(AParcel *, int64_t);
    int32_t (*writeFloat)(AParcel *, float);
    int32_t (*writeInt32Array)(AParcel *, const int32_t *, int32_t);
    int32_t (*writeFloatArray)(AParcel *, const float *, int32_t);
    int32_t (*writeString)(AParcel *, const char *, int32_t);
    int32_t (*writeStrongBinder)(AParcel *, AIBinder *);
    int32_t (*writeParcelFileDescriptor)(AParcel *, int);
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

static bool resolve_ndk(void *lib) {
    const struct {
        const char *name;
        void **slot;
    } syms[] = {
        { "AIBinder_Class_define", (void **)&g_ndk.Class_define },
        { "AIBinder_new", (void **)&g_ndk.AIBinder_new },
        { "AIBinder_associateClass", (void **)&g_ndk.associateClass },
        { "AIBinder_markVintfStability", (void **)&g_ndk.markVintfStability },
        { "AIBinder_incStrong", (void **)&g_ndk.incStrong },
        { "AIBinder_decStrong", (void **)&g_ndk.decStrong },
        { "AIBinder_prepareTransaction", (void **)&g_ndk.prepareTransaction },
        { "AIBinder_transact", (void **)&g_ndk.transact },
        { "AServiceManager_addService", (void **)&g_ndk.addService },
        { "ABinderProcess_setThreadPoolMaxThreadCount", (void **)&g_ndk.setThreadPoolMaxThreadCount },
        { "ABinderProcess_startThreadPool", (void **)&g_ndk.startThreadPool },
        { "ABinderProcess_joinThreadPool", (void **)&g_ndk.joinThreadPool },
        { "AStatus_newOk", (void **)&g_ndk.AStatus_newOk },
        { "AStatus_fromServiceSpecificError", (void **)&g_ndk.AStatus_fromServiceSpecificError },
        { "AStatus_delete", (void **)&g_ndk.AStatus_delete },
        { "AParcel_delete", (void **)&g_ndk.AParcel_delete },
        { "AParcel_writeStatusHeader", (void **)&g_ndk.writeStatusHeader },
        { "AParcel_readBool", (void **)&g_ndk.readBool },
        { "AParcel_readInt32", (void **)&g_ndk.readInt32 },
        { "AParcel_readInt64", (void **)&g_ndk.readInt64 },
        { "AParcel_readFloat", (void **)&g_ndk.readFloat },
        { "AParcel_readInt32Array", (void **)&g_ndk.readInt32Array },
        { "AParcel_readStrongBinder", (void **)&g_ndk.readStrongBinder },
        { "AParcel_readParcelFileDescriptor", (void **)&g_ndk.readParcelFileDescriptor },
        { "AParcel_writeBool", (void **)&g_ndk.writeBool },
        { "AParcel_writeByte", (void **)&g_ndk.writeByte },
        { "AParcel_writeInt32", (void **)&g_ndk.writeInt32 },
        { "AParcel_writeInt64", (void **)&g_ndk.writeInt64 },
        { "AParcel_writeFloat", (void **)&g_ndk.writeFloat },
        { "AParcel_writeInt32Array", (void **)&g_ndk.writeInt32Array },
        { "AParcel_writeFloatArray", (void **)&g_ndk.writeFloatArray },
        { "AParcel_writeString", (void **)&g_ndk.writeString },
        { "AParcel_writeStrongBinder", (void **)&g_ndk.writeStrongBinder },
        { "AParcel_writeParcelFileDescriptor", (void **)&g_ndk.writeParcelFileDescriptor },
        { "AParcel_getDataPosition", (void **)&g_ndk.getDataPosition },
        { "AParcel_setDataPosition", (void **)&g_ndk.setDataPosition },
    };
    for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        *syms[i].slot = dlsym(lib, syms[i].name);
        if (!*syms[i].slot)
            return false;
    }
    return true;
}

static int32_t begin_parcelable(AParcel *out, int32_t *start_pos) {
    *start_pos = g_ndk.getDataPosition(out);
    return g_ndk.writeInt32(out, 0);
}

static int32_t end_parcelable(AParcel *out, int32_t start_pos) {
    int32_t end_pos = g_ndk.getDataPosition(out);
    int32_t rc = g_ndk.setDataPosition(out, start_pos);
    if (rc != STATUS_OK_VAL) return rc;
    rc = g_ndk.writeInt32(out, end_pos - start_pos);
    if (rc != STATUS_OK_VAL) return rc;
    return g_ndk.setDataPosition(out, end_pos);
}

static int32_t write_status_ok(AParcel *out) {
    AStatus *st = g_ndk.AStatus_newOk();
    if (!st) return -ENOMEM;
    int32_t rc = g_ndk.writeStatusHeader(out, st);
    g_ndk.AStatus_delete(st);
    return rc;
}

static int32_t write_status_err(AParcel *out, int32_t err) {
    AStatus *st = g_ndk.AStatus_fromServiceSpecificError(err);
    if (!st) return -ENOMEM;
    int32_t rc = g_ndk.writeStatusHeader(out, st);
    g_ndk.AStatus_delete(st);
    return rc;
}

/* ---------------- Mainline DRM/KMS Atomic Engine ---------------- */
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CLIENT_CAP_ATOMIC           3
#define DRM_MODE_OBJECT_CRTC            0xccccccccU
#define DRM_MODE_OBJECT_CONNECTOR       0xc0c0c0c0U
#define DRM_MODE_OBJECT_PLANE           0xeeeeeeeeU
#define DRM_PLANE_TYPE_OVERLAY          0U
#define DRM_PLANE_TYPE_PRIMARY          1U
#define DRM_PLANE_TYPE_CURSOR           2U
#define DRM_MODE_ATOMIC_TEST_ONLY       0x0100U
#define DRM_MODE_ATOMIC_NONBLOCK        0x0200U
#define DRM_MODE_ATOMIC_ALLOW_MODESET   0x0400U
#define DRM_MODE_BLEND_PREMULTI         0U
#define DRM_MODE_BLEND_COVERAGE         1U
#define DRM_MODE_BLEND_PIXEL_NONE       2U
#define DRM_SYNCOBJ_CREATE_SIGNALED     (1U << 0)
#define DRM_SYNCOBJ_EXPORT_SYNC_FILE    (1U << 0)

struct drm_set_client_cap { uint64_t capability, value; };
struct drm_gem_close { uint32_t handle, pad; };
struct drm_prime_handle { uint32_t handle, flags; int32_t fd; };
struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};
struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};
struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo mode;
};
struct drm_mode_get_encoder {
    uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};
struct drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel, pad;
};
struct drm_mode_get_plane_res { uint64_t plane_id_ptr; uint32_t count_planes; };
struct drm_mode_get_plane {
    uint32_t plane_id, crtc_id, fb_id, possible_crtcs, gamma_size, count_format_types;
    uint64_t format_type_ptr;
};
struct drm_mode_obj_get_properties {
    uint64_t props_ptr, prop_values_ptr;
    uint32_t count_props, obj_id, obj_type;
};
struct drm_mode_get_property {
    uint64_t values_ptr, enum_blob_ptr;
    uint32_t prop_id, flags;
    char name[32];
    uint32_t count_values, count_enum_blobs;
};
struct drm_mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifier[4];
};
struct drm_mode_create_blob { uint64_t data; uint32_t length, blob_id; };
struct drm_mode_atomic {
    uint32_t flags, count_objs;
    uint64_t objs_ptr, count_props_ptr, props_ptr, prop_values_ptr, reserved, user_data;
};
struct drm_syncobj_create { uint32_t handle, flags; };
struct drm_syncobj_destroy { uint32_t handle, pad; };
struct drm_syncobj_handle { uint32_t handle, flags; int32_t fd; uint32_t pad; uint64_t point; };
struct drm_mode_closefb { uint32_t fb_id, pad; };

#define DRM_IOCTL_GEM_CLOSE              _IOW('d', 0x09, struct drm_gem_close)
#define DRM_IOCTL_SET_CLIENT_CAP         _IOW('d', 0x0d, struct drm_set_client_cap)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE     _IOWR('d', 0x2e, struct drm_prime_handle)
#define DRM_IOCTL_MODE_GETRESOURCES      _IOWR('d', 0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC           _IOWR('d', 0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER        _IOWR('d', 0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR      _IOWR('d', 0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_GETPROPERTY       _IOWR('d', 0xAA, struct drm_mode_get_property)
#define DRM_IOCTL_MODE_GETPLANERESOURCES _IOWR('d', 0xB5, struct drm_mode_get_plane_res)
#define DRM_IOCTL_MODE_GETPLANE          _IOWR('d', 0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_ADDFB2            _IOWR('d', 0xB8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES _IOWR('d', 0xB9, struct drm_mode_obj_get_properties)
#define DRM_IOCTL_MODE_ATOMIC            _IOWR('d', 0xBC, struct drm_mode_atomic)
#define DRM_IOCTL_MODE_CREATEPROPBLOB    _IOWR('d', 0xBD, struct drm_mode_create_blob)
#define DRM_IOCTL_SYNCOBJ_CREATE         _IOWR('d', 0xBF, struct drm_syncobj_create)
#define DRM_IOCTL_SYNCOBJ_DESTROY        _IOWR('d', 0xC0, struct drm_syncobj_destroy)
#define DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD   _IOWR('d', 0xC1, struct drm_syncobj_handle)
#define DRM_IOCTL_MODE_CLOSEFB           _IOWR('d', 0xD0, struct drm_mode_closefb)

struct drm_wait_vblank_request {
    uint32_t type;
    uint32_t sequence;
    unsigned long signal;
};

struct drm_wait_vblank_reply {
    uint32_t type;
    uint32_t sequence;
    long tval_sec;
    long tval_usec;
};

union drm_wait_vblank {
    struct drm_wait_vblank_request request;
    struct drm_wait_vblank_reply reply;
};

#define DRM_VBLANK_RELATIVE        0x00000001U
#define DRM_VBLANK_HIGH_CRTC_SHIFT 1U
#define DRM_VBLANK_SECONDARY       0x20000000U
#define DRM_IOCTL_WAIT_VBLANK      _IOWR('d', 0x3a, union drm_wait_vblank)

#define MAX_CLIENT_SLOTS 32
#define MAX_LAYER_SLOTS  32
#define MAX_LAYERS       128
#define MAX_HW_PLANES    6

typedef struct {
    uint32_t fb_id;
    uint32_t gem_handle;
    uint32_t w;
    uint32_t h;
    uint32_t fourcc;
} client_slot_t;

typedef struct {
    uint32_t id;
    uint32_t type;
    uint32_t p_fb, p_crtc;
    uint32_t p_sx, p_sy, p_sw, p_sh;
    uint32_t p_cx, p_cy, p_cw, p_ch;
    uint32_t p_infence, p_zpos, p_alpha, p_blend;
} drm_plane_info_t;

typedef struct {
    bool used;
    int64_t id;
    int32_t req_comp;
    int32_t comp_type;
    int32_t blend_mode;
    float plane_alpha;
    int32_t z;
    int32_t transform;
    int32_t dataspace;
    int32_t df_left, df_top, df_right, df_bottom;
    float sc_left, sc_top, sc_right, sc_bottom;
    bool has_color_transform;
    bool has_brightness;
    bool has_sideband;
    bool visible_empty;
    client_slot_t slots[MAX_LAYER_SLOTS];
    int active_slot;
    int acquire_fence;
} layer_state_t;

static struct {
    pthread_mutex_t lock;
    int card_fd;
    uint32_t conn_id, prop_conn_crtc_id;
    uint32_t crtc_id, crtc_index, prop_crtc_active, prop_crtc_mode_id, prop_crtc_out_fence_ptr;
    uint32_t mode_blob_id;
    drm_plane_info_t planes[MAX_HW_PLANES];
    uint32_t n_planes;
    uint32_t disp_w, disp_h;
    int32_t vsync_period_ns;
    int32_t dpi_x, dpi_y;
    bool crtc_active;
    bool need_modeset;
    bool disp_has_color_transform;
    int prev_out_fence;
    client_slot_t slots[MAX_CLIENT_SLOTS];
    int active_slot;
    layer_state_t layers[MAX_LAYERS];
    int64_t next_layer_id;
    bool validated_all_device;
    int validated_device_idx[MAX_HW_PLANES];
    int32_t n_validated_device;
    int64_t prev_device_layers[MAX_HW_PLANES];
    int32_t n_prev_device_layers;
    AIBinder *callback;
    bool vsync_enabled;
    pthread_t vsync_thread;
    bool vsync_thread_started;
} g_disp = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .card_fd = -1,
    .disp_w = 1080,
    .disp_h = 1920,
    .vsync_period_ns = 16666667,
    .dpi_x = 441000,
    .dpi_y = 441000,
    .crtc_active = true,
    .need_modeset = true,
    .prev_out_fence = -1,
    .active_slot = -1,
    .next_layer_id = 1,
};

static uint32_t drm_find_prop(int fd, uint32_t obj_id, uint32_t obj_type,
                              const char *name, uint64_t *out_val) {
    struct drm_mode_obj_get_properties gp;
    uint32_t ids[32];
    uint64_t vals[32];
    memset(&gp, 0, sizeof(gp));
    gp.obj_id = obj_id;
    gp.obj_type = obj_type;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0 || gp.count_props == 0)
        return 0;
    if (gp.count_props > 32) gp.count_props = 32;
    gp.props_ptr = (uint64_t)(uintptr_t)ids;
    gp.prop_values_ptr = (uint64_t)(uintptr_t)vals;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0)
        return 0;
    for (uint32_t i = 0; i < gp.count_props; i++) {
        struct drm_mode_get_property prop;
        memset(&prop, 0, sizeof(prop));
        prop.prop_id = ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) == 0 &&
            strcmp(prop.name, name) == 0) {
            if (out_val) *out_val = vals[i];
            return ids[i];
        }
    }
    return 0;
}

static bool drm_init_locked(void) {
    if (g_disp.card_fd >= 0)
        return true;

    static const char *const cards[] = { "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2" };
    int fd = -1;
    uint32_t crtcs[16], conns[16], encs[16], plane_ids[32];
    struct drm_mode_card_res res;

    for (size_t i = 0; i < sizeof(cards) / sizeof(cards[0]); i++) {
        int cand = open(cards[i], O_RDWR | O_CLOEXEC);
        if (cand < 0) continue;
        struct drm_set_client_cap cap = { DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
        if (ioctl(cand, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) { close(cand); continue; }
        cap.capability = DRM_CLIENT_CAP_ATOMIC;
        cap.value = 1;
        if (ioctl(cand, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) { close(cand); continue; }
        memset(&res, 0, sizeof(res));
        if (ioctl(cand, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 &&
            res.count_crtcs > 0 && res.count_connectors > 0) {
            fd = cand;
            break;
        }
        close(cand);
    }
    if (fd < 0) {
        kmsg_log("<3>sailfish_composer_hal: failed to open DRM card\n");
        return false;
    }

    if (res.count_crtcs > 16) res.count_crtcs = 16;
    if (res.count_connectors > 16) res.count_connectors = 16;
    if (res.count_encoders > 16) res.count_encoders = 16;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encs;
    res.count_fbs = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        close(fd);
        return false;
    }

    struct drm_mode_modeinfo modes[16], chosen_mode;
    uint32_t conn_encs[16], conn_props[32];
    uint64_t conn_prop_vals[32];
    uint32_t chosen_conn = 0, chosen_enc = 0;
    bool have_mode = false;
    uint32_t mm_w = 0, mm_h = 0;

    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector conn;
        memset(&conn, 0, sizeof(conn));
        conn.connector_id = conns[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        if (conn.count_modes > 16) conn.count_modes = 16;
        if (conn.count_encoders > 16) conn.count_encoders = 16;
        if (conn.count_props > 32) conn.count_props = 32;
        conn.modes_ptr = (uint64_t)(uintptr_t)modes;
        conn.encoders_ptr = (uint64_t)(uintptr_t)conn_encs;
        conn.props_ptr = (uint64_t)(uintptr_t)conn_props;
        conn.prop_values_ptr = (uint64_t)(uintptr_t)conn_prop_vals;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        if ((conn.connection == 1 || chosen_conn == 0) && conn.count_modes > 0) {
            chosen_conn = conn.connector_id;
            chosen_enc = conn.encoder_id ? conn.encoder_id : (conn.count_encoders ? conn_encs[0] : 0);
            chosen_mode = modes[0];
            mm_w = conn.mm_width;
            mm_h = conn.mm_height;
            have_mode = true;
            if (conn.connection == 1) break;
        }
    }

    uint32_t crtc_idx = 0, chosen_crtc = crtcs[0];
    if (chosen_enc) {
        struct drm_mode_get_encoder enc;
        memset(&enc, 0, sizeof(enc));
        enc.encoder_id = chosen_enc;
        if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0) {
            for (uint32_t i = 0; i < res.count_crtcs; i++) {
                if ((enc.crtc_id && crtcs[i] == enc.crtc_id) ||
                    (!enc.crtc_id && (enc.possible_crtcs & (1U << i)))) {
                    chosen_crtc = crtcs[i];
                    crtc_idx = i;
                    break;
                }
            }
        }
    }

    struct drm_mode_crtc gcrtc;
    memset(&gcrtc, 0, sizeof(gcrtc));
    gcrtc.crtc_id = chosen_crtc;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &gcrtc) == 0 && gcrtc.mode_valid) {
        chosen_mode = gcrtc.mode;
        have_mode = true;
    }

    if (!chosen_conn || !chosen_crtc || !have_mode) {
        close(fd);
        return false;
    }

    struct drm_mode_create_blob blob;
    memset(&blob, 0, sizeof(blob));
    blob.data = (uint64_t)(uintptr_t)&chosen_mode;
    blob.length = sizeof(chosen_mode);
    if (ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) < 0 || blob.blob_id == 0) {
        close(fd);
        return false;
    }

    g_disp.conn_id = chosen_conn;
    g_disp.prop_conn_crtc_id = drm_find_prop(fd, chosen_conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
    g_disp.crtc_id = chosen_crtc;
    g_disp.crtc_index = crtc_idx;
    g_disp.prop_crtc_active = drm_find_prop(fd, chosen_crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
    g_disp.prop_crtc_mode_id = drm_find_prop(fd, chosen_crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
    g_disp.prop_crtc_out_fence_ptr = drm_find_prop(fd, chosen_crtc, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR", NULL);
    g_disp.mode_blob_id = blob.blob_id;
    g_disp.disp_w = chosen_mode.hdisplay ? chosen_mode.hdisplay : 1080;
    g_disp.disp_h = chosen_mode.vdisplay ? chosen_mode.vdisplay : 1920;
    uint32_t vref = chosen_mode.vrefresh ? chosen_mode.vrefresh : 60;
    g_disp.vsync_period_ns = (int32_t)(1000000000ULL / vref);
    if (mm_w > 0 && mm_h > 0) {
        g_disp.dpi_x = (int32_t)((uint64_t)g_disp.disp_w * 25400ULL / mm_w);
        g_disp.dpi_y = (int32_t)((uint64_t)g_disp.disp_h * 25400ULL / mm_h);
    }

    g_disp.n_planes = 0;
    struct drm_mode_get_plane_res pres;
    memset(&pres, 0, sizeof(pres));
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) == 0 && pres.count_planes > 0) {
        if (pres.count_planes > 32) pres.count_planes = 32;
        pres.plane_id_ptr = (uint64_t)(uintptr_t)plane_ids;
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) == 0) {
            /* Pass 1: PRIMARY plane at index 0; Pass 2: OVERLAY planes at 1..MAX_HW_PLANES-1 */
            for (int pass = 0; pass < 2; pass++) {
                uint64_t want_type = (pass == 0) ? DRM_PLANE_TYPE_PRIMARY : DRM_PLANE_TYPE_OVERLAY;
                for (uint32_t i = 0; i < pres.count_planes && g_disp.n_planes < MAX_HW_PLANES; i++) {
                    struct drm_mode_get_plane gp;
                    memset(&gp, 0, sizeof(gp));
                    gp.plane_id = plane_ids[i];
                    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp) < 0) continue;
                    if (!(gp.possible_crtcs & (1U << crtc_idx))) continue;
                    uint64_t ptype = 0;
                    drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "type", &ptype);
                    if (ptype != want_type) continue;

                    drm_plane_info_t *pl = &g_disp.planes[g_disp.n_planes];
                    memset(pl, 0, sizeof(*pl));
                    pl->id = gp.plane_id;
                    pl->type = (uint32_t)ptype;
                    pl->p_fb = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
                    pl->p_crtc = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
                    pl->p_sx = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", NULL);
                    pl->p_sy = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", NULL);
                    pl->p_sw = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", NULL);
                    pl->p_sh = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", NULL);
                    pl->p_cx = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", NULL);
                    pl->p_cy = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", NULL);
                    pl->p_cw = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", NULL);
                    pl->p_ch = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", NULL);
                    pl->p_infence = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD", NULL);
                    pl->p_zpos = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "zpos", NULL);
                    pl->p_alpha = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "alpha", NULL);
                    pl->p_blend = drm_find_prop(fd, gp.plane_id, DRM_MODE_OBJECT_PLANE, "pixel blend mode", NULL);
                    if (pl->p_fb && pl->p_crtc) {
                        g_disp.n_planes++;
                        if (pass == 0) break; /* one primary plane */
                    }
                }
            }
        }
    }

    if (g_disp.n_planes == 0 || !g_disp.prop_crtc_active) {
        close(fd);
        return false;
    }

    g_disp.card_fd = fd;
    char msg[160];
    snprintf(msg, sizeof(msg),
             "<6>sailfish_composer_hal: DRM ready crtc=%u planes=%u (primary=%u) %ux%u@%uHz\n",
             g_disp.crtc_id, g_disp.n_planes, g_disp.planes[0].id,
             g_disp.disp_w, g_disp.disp_h, vref);
    kmsg_log(msg);
    return true;
}

static int drm_create_signaled_fence_locked(void) {
    if (g_disp.card_fd < 0) return -1;
    struct drm_syncobj_create sc = { 0, DRM_SYNCOBJ_CREATE_SIGNALED };
    if (ioctl(g_disp.card_fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc) < 0) return -1;
    struct drm_syncobj_handle sh = { sc.handle, DRM_SYNCOBJ_EXPORT_SYNC_FILE, -1, 0, 0 };
    int ret_fd = -1;
    if (ioctl(g_disp.card_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &sh) == 0)
        ret_fd = sh.fd;
    struct drm_syncobj_destroy sd = { sc.handle, 0 };
    ioctl(g_disp.card_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sd);
    return ret_fd;
}

static void release_slot_buf_locked(client_slot_t *s) {
    if (!s) return;
    if (s->fb_id && g_disp.card_fd >= 0) {
        struct drm_mode_closefb cfb = { s->fb_id, 0 };
        ioctl(g_disp.card_fd, DRM_IOCTL_MODE_CLOSEFB, &cfb);
        s->fb_id = 0;
    }
    if (s->gem_handle && g_disp.card_fd >= 0) {
        struct drm_gem_close gc = { s->gem_handle, 0 };
        ioctl(g_disp.card_fd, DRM_IOCTL_GEM_CLOSE, &gc);
        s->gem_handle = 0;
    }
    s->w = 0;
    s->h = 0;
    s->fourcc = 0;
}

static bool import_slot_buf_locked(client_slot_t *s, int dmabuf_fd,
                                   const sailfish_gralloc_ints_t *info,
                                   bool allow_xb24_fallback) {
    if (!s || dmabuf_fd < 0 || !drm_init_locked())
        return false;
    release_slot_buf_locked(s);
    if (!allow_xb24_fallback && !info)
        return false;

    struct drm_prime_handle ph;
    memset(&ph, 0, sizeof(ph));
    ph.fd = dmabuf_fd;
    if (ioctl(g_disp.card_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) < 0 || ph.handle == 0)
        return false;

    uint32_t w = (info && info->width) ? info->width : g_disp.disp_w;
    uint32_t h = (info && info->height) ? info->height : g_disp.disp_h;
    uint32_t pitch = (info && info->stride_bytes) ? info->stride_bytes : (((w + 63U) & ~63U) * 4U);
    uint32_t fourcc = (info && info->drm_fourcc) ? info->drm_fourcc : FOURCC_CODE('A', 'B', '2', '4');

    /* Only import MDP5-safe single-plane RGB formats into DRM FBs */
    if (fourcc != FOURCC_CODE('A', 'B', '2', '4') &&
        fourcc != FOURCC_CODE('X', 'B', '2', '4') &&
        fourcc != FOURCC_CODE('A', 'R', '2', '4') &&
        fourcc != FOURCC_CODE('R', 'G', '1', '6')) {
        if (!allow_xb24_fallback) {
            struct drm_gem_close gc = { ph.handle, 0 };
            ioctl(g_disp.card_fd, DRM_IOCTL_GEM_CLOSE, &gc);
            return false;
        }
        fourcc = FOURCC_CODE('A', 'B', '2', '4');
    }

    struct drm_mode_fb_cmd2 fb;
    memset(&fb, 0, sizeof(fb));
    fb.width = w;
    fb.height = h;
    fb.pixel_format = fourcc;
    fb.handles[0] = ph.handle;
    fb.pitches[0] = pitch;
    if (ioctl(g_disp.card_fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
        if (!allow_xb24_fallback) {
            struct drm_gem_close gc = { ph.handle, 0 };
            ioctl(g_disp.card_fd, DRM_IOCTL_GEM_CLOSE, &gc);
            return false;
        }
        fb.pixel_format = FOURCC_CODE('X', 'B', '2', '4');
        if (ioctl(g_disp.card_fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
            struct drm_gem_close gc = { ph.handle, 0 };
            ioctl(g_disp.card_fd, DRM_IOCTL_GEM_CLOSE, &gc);
            return false;
        }
        fourcc = fb.pixel_format;
    }

    s->fb_id = fb.fb_id;
    s->gem_handle = ph.handle;
    s->w = w;
    s->h = h;
    s->fourcc = fourcc;
    return true;
}

static bool import_client_target_locked(int slot, int dmabuf_fd,
                                        const sailfish_gralloc_ints_t *info) {
    if (slot < 0 || slot >= MAX_CLIENT_SLOTS) return false;
    return import_slot_buf_locked(&g_disp.slots[slot], dmabuf_fd, info, true);
}

static void layer_destroy_resources_locked(layer_state_t *L) {
    if (!L) return;
    for (int i = 0; i < MAX_LAYER_SLOTS; i++)
        release_slot_buf_locked(&L->slots[i]);
    if (L->acquire_fence >= 0) {
        close(L->acquire_fence);
        L->acquire_fence = -1;
    }
    L->active_slot = -1;
}

typedef struct {
    uint32_t objs[16];
    uint32_t cnts[16];
    uint32_t props[128];
    uint64_t vals[128];
    uint32_t n_objs;
    uint32_t n_props;
} atomic_req_t;

static void areq_begin_obj(atomic_req_t *r, uint32_t obj_id) {
    if (r->n_objs >= 16) return;
    r->objs[r->n_objs] = obj_id;
    r->cnts[r->n_objs] = 0;
    r->n_objs++;
}

static void areq_add_prop(atomic_req_t *r, uint32_t prop_id, uint64_t val) {
    if (!prop_id || r->n_objs == 0 || r->n_props >= 128) return;
    r->props[r->n_props] = prop_id;
    r->vals[r->n_props] = val;
    r->n_props++;
    r->cnts[r->n_objs - 1]++;
}

static void set_backlight_sysfs(int bl_power, int brightness) {
    int fd;
    char buf[16];
    if (bl_power >= 0) {
        fd = open("/sys/class/backlight/994000.dsi.0/bl_power", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            int n = snprintf(buf, sizeof(buf), "%d\n", bl_power);
            (void)write(fd, buf, (size_t)n);
            close(fd);
        }
    }
    if (brightness >= 0) {
        fd = open("/sys/class/backlight/994000.dsi.0/brightness", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            int n = snprintf(buf, sizeof(buf), "%d\n", brightness);
            (void)write(fd, buf, (size_t)n);
            close(fd);
        }
    }
}

static int g_last_brightness = 180;
static int g_commit_ok_logs = 0;
static int g_hw_overlay_logs = 0;
static int g_commit_err_logs = 0;

static uint64_t hwc_blend_to_drm(int32_t blend_mode) {
    switch (blend_mode) {
    case BLEND_NONE:     return DRM_MODE_BLEND_PIXEL_NONE;
    case BLEND_COVERAGE: return DRM_MODE_BLEND_COVERAGE;
    default:             return DRM_MODE_BLEND_PREMULTI;
    }
}

static uint64_t float_alpha_to_drm(float a) {
    if (a <= 0.0f) return 0;
    if (a >= 1.0f) return 0xffffU;
    return (uint64_t)(a * 65535.0f + 0.5f);
}

static void areq_add_plane_layer(atomic_req_t *req, const drm_plane_info_t *pl,
                                 const layer_state_t *L, uint64_t zpos,
                                 bool include_fence) {
    const client_slot_t *s = &L->slots[L->active_slot];
    uint64_t sx = (uint64_t)(L->sc_left * 65536.0f + 0.5f);
    uint64_t sy = (uint64_t)(L->sc_top * 65536.0f + 0.5f);
    uint64_t sw = (uint64_t)((L->sc_right - L->sc_left) * 65536.0f + 0.5f);
    uint64_t sh = (uint64_t)((L->sc_bottom - L->sc_top) * 65536.0f + 0.5f);

    areq_begin_obj(req, pl->id);
    areq_add_prop(req, pl->p_fb, s->fb_id);
    areq_add_prop(req, pl->p_crtc, g_disp.crtc_id);
    areq_add_prop(req, pl->p_sx, sx);
    areq_add_prop(req, pl->p_sy, sy);
    areq_add_prop(req, pl->p_sw, sw);
    areq_add_prop(req, pl->p_sh, sh);
    areq_add_prop(req, pl->p_cx, (uint64_t)(int64_t)L->df_left);
    areq_add_prop(req, pl->p_cy, (uint64_t)(int64_t)L->df_top);
    areq_add_prop(req, pl->p_cw, (uint64_t)(uint32_t)(L->df_right - L->df_left));
    areq_add_prop(req, pl->p_ch, (uint64_t)(uint32_t)(L->df_bottom - L->df_top));
    areq_add_prop(req, pl->p_zpos, zpos);
    areq_add_prop(req, pl->p_alpha, float_alpha_to_drm(L->plane_alpha));
    areq_add_prop(req, pl->p_blend, hwc_blend_to_drm(L->blend_mode));
    if (include_fence && pl->p_infence && L->acquire_fence >= 0)
        areq_add_prop(req, pl->p_infence, (uint64_t)(int64_t)L->acquire_fence);
}

static void areq_disable_plane(atomic_req_t *req, const drm_plane_info_t *pl) {
    areq_begin_obj(req, pl->id);
    areq_add_prop(req, pl->p_fb, 0);
    areq_add_prop(req, pl->p_crtc, 0);
}

static bool drm_test_device_planes_locked(const int *layer_idxs, int n_layers) {
    if (!drm_init_locked() || !g_disp.crtc_active || n_layers <= 0 ||
        (uint32_t)n_layers > g_disp.n_planes)
        return false;

    atomic_req_t req;
    memset(&req, 0, sizeof(req));

    areq_begin_obj(&req, g_disp.conn_id);
    areq_add_prop(&req, g_disp.prop_conn_crtc_id, g_disp.crtc_id);

    areq_begin_obj(&req, g_disp.crtc_id);
    areq_add_prop(&req, g_disp.prop_crtc_active, 1);
    areq_add_prop(&req, g_disp.prop_crtc_mode_id, g_disp.mode_blob_id);

    for (int i = 0; i < n_layers; i++) {
        areq_add_plane_layer(&req, &g_disp.planes[i], &g_disp.layers[layer_idxs[i]],
                             (uint64_t)(i + 1), false);
    }
    for (uint32_t p = (uint32_t)n_layers; p < g_disp.n_planes; p++) {
        areq_disable_plane(&req, &g_disp.planes[p]);
    }

    struct drm_mode_atomic ma;
    memset(&ma, 0, sizeof(ma));
    ma.flags = DRM_MODE_ATOMIC_TEST_ONLY |
               (g_disp.need_modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0);
    ma.count_objs = req.n_objs;
    ma.objs_ptr = (uint64_t)(uintptr_t)req.objs;
    ma.count_props_ptr = (uint64_t)(uintptr_t)req.cnts;
    ma.props_ptr = (uint64_t)(uintptr_t)req.props;
    ma.prop_values_ptr = (uint64_t)(uintptr_t)req.vals;
    return ioctl(g_disp.card_fd, DRM_IOCTL_MODE_ATOMIC, &ma) == 0;
}

static int drm_commit_present_locked(int slot, int acquire_fence, bool use_device_planes) {
    if (!drm_init_locked()) {
        if (acquire_fence >= 0) close(acquire_fence);
        return -1;
    }
    if (!g_disp.crtc_active) {
        if (acquire_fence >= 0) close(acquire_fence);
        return drm_create_signaled_fence_locked();
    }

    if (use_device_planes) {
        if (g_disp.n_validated_device <= 0 ||
            (uint32_t)g_disp.n_validated_device > g_disp.n_planes) {
            use_device_planes = false;
        } else {
            for (int i = 0; i < g_disp.n_validated_device; i++) {
                layer_state_t *L = &g_disp.layers[g_disp.validated_device_idx[i]];
                if (!L->used || L->active_slot < 0 || L->active_slot >= MAX_LAYER_SLOTS ||
                    !L->slots[L->active_slot].fb_id) {
                    use_device_planes = false;
                    break;
                }
            }
        }
    }

    if (!use_device_planes &&
        (slot < 0 || slot >= MAX_CLIENT_SLOTS || !g_disp.slots[slot].fb_id)) {
        if (acquire_fence >= 0) close(acquire_fence);
        return drm_create_signaled_fence_locked();
    }

    atomic_req_t req;
    memset(&req, 0, sizeof(req));
    int32_t out_fence = -1;

    areq_begin_obj(&req, g_disp.conn_id);
    areq_add_prop(&req, g_disp.prop_conn_crtc_id, g_disp.crtc_id);

    areq_begin_obj(&req, g_disp.crtc_id);
    areq_add_prop(&req, g_disp.prop_crtc_active, 1);
    areq_add_prop(&req, g_disp.prop_crtc_mode_id, g_disp.mode_blob_id);
    if (g_disp.prop_crtc_out_fence_ptr)
        areq_add_prop(&req, g_disp.prop_crtc_out_fence_ptr, (uint64_t)(uintptr_t)&out_fence);

    uint32_t primary_fb = 0, primary_w = g_disp.disp_w, primary_h = g_disp.disp_h;
    if (use_device_planes) {
        for (int i = 0; i < g_disp.n_validated_device; i++) {
            layer_state_t *L = &g_disp.layers[g_disp.validated_device_idx[i]];
            const drm_plane_info_t *pl = &g_disp.planes[i];
            if (!pl->p_infence && L->acquire_fence >= 0) {
                struct pollfd pfd = { .fd = L->acquire_fence, .events = POLLIN };
                (void)poll(&pfd, 1, 500);
                close(L->acquire_fence);
                L->acquire_fence = -1;
            }
            areq_add_plane_layer(&req, pl, L, (uint64_t)(i + 1), true);
            if (i == 0) {
                primary_fb = L->slots[L->active_slot].fb_id;
                primary_w = L->slots[L->active_slot].w;
                primary_h = L->slots[L->active_slot].h;
            }
        }
        for (uint32_t p = (uint32_t)g_disp.n_validated_device; p < g_disp.n_planes; p++) {
            areq_disable_plane(&req, &g_disp.planes[p]);
        }
    } else {
        const drm_plane_info_t *pl = &g_disp.planes[0];
        client_slot_t *s = &g_disp.slots[slot];
        primary_fb = s->fb_id;
        primary_w = s->w;
        primary_h = s->h;
        if (!pl->p_infence && acquire_fence >= 0) {
            struct pollfd pfd = { .fd = acquire_fence, .events = POLLIN };
            (void)poll(&pfd, 1, 500);
            close(acquire_fence);
            acquire_fence = -1;
        }
        areq_begin_obj(&req, pl->id);
        areq_add_prop(&req, pl->p_fb, s->fb_id);
        areq_add_prop(&req, pl->p_crtc, g_disp.crtc_id);
        areq_add_prop(&req, pl->p_sx, 0);
        areq_add_prop(&req, pl->p_sy, 0);
        areq_add_prop(&req, pl->p_sw, (uint64_t)s->w << 16);
        areq_add_prop(&req, pl->p_sh, (uint64_t)s->h << 16);
        areq_add_prop(&req, pl->p_cx, 0);
        areq_add_prop(&req, pl->p_cy, 0);
        areq_add_prop(&req, pl->p_cw, g_disp.disp_w);
        areq_add_prop(&req, pl->p_ch, g_disp.disp_h);
        areq_add_prop(&req, pl->p_zpos, 1);
        areq_add_prop(&req, pl->p_alpha, 0xffffU);
        areq_add_prop(&req, pl->p_blend, DRM_MODE_BLEND_PIXEL_NONE);
        if (pl->p_infence && acquire_fence >= 0)
            areq_add_prop(&req, pl->p_infence, (uint64_t)(int64_t)acquire_fence);
        for (uint32_t p = 1; p < g_disp.n_planes; p++) {
            areq_disable_plane(&req, &g_disp.planes[p]);
        }
    }

    struct drm_mode_atomic ma;
    memset(&ma, 0, sizeof(ma));
    ma.flags = g_disp.need_modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : DRM_MODE_ATOMIC_NONBLOCK;
    ma.count_objs = req.n_objs;
    ma.objs_ptr = (uint64_t)(uintptr_t)req.objs;
    ma.count_props_ptr = (uint64_t)(uintptr_t)req.cnts;
    ma.props_ptr = (uint64_t)(uintptr_t)req.props;
    ma.prop_values_ptr = (uint64_t)(uintptr_t)req.vals;

    int ret = ioctl(g_disp.card_fd, DRM_IOCTL_MODE_ATOMIC, &ma);
    if (ret < 0 && errno == EBUSY) {
        if (g_disp.prev_out_fence >= 0) {
            struct pollfd pfd = { .fd = g_disp.prev_out_fence, .events = POLLIN };
            (void)poll(&pfd, 1, 35);
            close(g_disp.prev_out_fence);
            g_disp.prev_out_fence = -1;
            ret = ioctl(g_disp.card_fd, DRM_IOCTL_MODE_ATOMIC, &ma);
        }
        if (ret < 0 && errno == EBUSY) {
            ma.flags = 0;
            ret = ioctl(g_disp.card_fd, DRM_IOCTL_MODE_ATOMIC, &ma);
        }
    }
    if (ret < 0 && !g_disp.need_modeset) {
        ma.flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
        ret = ioctl(g_disp.card_fd, DRM_IOCTL_MODE_ATOMIC, &ma);
    }
    int saved_err = errno;
    if (acquire_fence >= 0)
        close(acquire_fence);
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (g_disp.layers[i].used && g_disp.layers[i].acquire_fence >= 0) {
            close(g_disp.layers[i].acquire_fence);
            g_disp.layers[i].acquire_fence = -1;
        }
    }

    if (ret == 0) {
        if (g_disp.prev_out_fence >= 0) {
            close(g_disp.prev_out_fence);
            g_disp.prev_out_fence = -1;
        }
        if (g_disp.need_modeset) {
            set_backlight_sysfs(0, g_last_brightness);
            g_disp.need_modeset = false;
        }
        if (use_device_planes && g_hw_overlay_logs < 5) {
            g_hw_overlay_logs++;
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "<6>sailfish_composer_hal: MDP5 HW overlay commit #%d OK (%d planes, fb0=%u %ux%u fence=%d)\n",
                     g_hw_overlay_logs, g_disp.n_validated_device, primary_fb, primary_w, primary_h, out_fence);
            kmsg_log(msg);
        } else if (!use_device_planes && g_commit_ok_logs < 3) {
            g_commit_ok_logs++;
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "<6>sailfish_composer_hal: atomic commit #%d OK fb=%u %ux%u fence=%d\n",
                     g_commit_ok_logs, primary_fb, primary_w, primary_h, out_fence);
            kmsg_log(msg);
        }
        if (out_fence >= 0) {
            g_disp.prev_out_fence = fcntl(out_fence, F_DUPFD_CLOEXEC, 0);
            return out_fence;
        }
    } else if (g_commit_err_logs < 16) {
        g_commit_err_logs++;
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "<3>sailfish_composer_hal: atomic commit FAILED errno=%d dev=%d fb=%u\n",
                 saved_err, (int)use_device_planes, primary_fb);
        kmsg_log(msg);
    }
    return drm_create_signaled_fence_locked();
}

static void drm_set_power_locked(bool on) {
    if (!drm_init_locked()) return;
    if (on == g_disp.crtc_active) {
        if (on) set_backlight_sysfs(0, g_last_brightness);
        return;
    }
    g_disp.crtc_active = on;
    if (on) {
        g_disp.need_modeset = true;
        set_backlight_sysfs(0, g_last_brightness);
        return;
    }

    set_backlight_sysfs(4, 0);
    atomic_req_t req;
    memset(&req, 0, sizeof(req));
    areq_begin_obj(&req, g_disp.conn_id);
    areq_add_prop(&req, g_disp.prop_conn_crtc_id, 0);
    areq_begin_obj(&req, g_disp.crtc_id);
    areq_add_prop(&req, g_disp.prop_crtc_active, 0);
    areq_add_prop(&req, g_disp.prop_crtc_mode_id, 0);
    for (uint32_t p = 0; p < g_disp.n_planes; p++) {
        areq_disable_plane(&req, &g_disp.planes[p]);
    }

    struct drm_mode_atomic ma;
    memset(&ma, 0, sizeof(ma));
    ma.flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    ma.count_objs = req.n_objs;
    ma.objs_ptr = (uint64_t)(uintptr_t)req.objs;
    ma.count_props_ptr = (uint64_t)(uintptr_t)req.cnts;
    ma.props_ptr = (uint64_t)(uintptr_t)req.props;
    ma.prop_values_ptr = (uint64_t)(uintptr_t)req.vals;
    (void)ioctl(g_disp.card_fd, DRM_IOCTL_MODE_ATOMIC, &ma);
    g_disp.need_modeset = true;
}

static AIBinder_Class *g_cb_cls = NULL;

/* ---------------- Callback Helpers (onHotplug / onVsync) ---------------- */
static void send_on_hotplug(AIBinder *cb, int64_t display, bool connected) {
    if (!cb) return;
    AParcel *in = NULL, *out = NULL;
    int32_t rc = g_ndk.prepareTransaction(cb, &in);
    if (rc != STATUS_OK_VAL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "<3>sailfish_composer_hal: onHotplug prepareTransaction rc=%d\n", rc);
        kmsg_log(msg);
        return;
    }
    (void)g_ndk.writeInt64(in, display);
    (void)g_ndk.writeBool(in, connected);
    rc = g_ndk.transact(cb, TX_CB_ON_HOTPLUG, &in, &out, 0);
    if (out) g_ndk.AParcel_delete(out);
    char msg[128];
    snprintf(msg, sizeof(msg), "<6>sailfish_composer_hal: sent onHotplug(disp=%lld, conn=%d) rc=%d\n",
             (long long)display, (int)connected, rc);
    kmsg_log(msg);
}

static void send_on_vsync(AIBinder *cb, int64_t display, int64_t ts_ns, int32_t period_ns) {
    if (!cb) return;
    AParcel *in = NULL, *out = NULL;
    if (g_ndk.prepareTransaction(cb, &in) != STATUS_OK_VAL) return;
    (void)g_ndk.writeInt64(in, display);
    (void)g_ndk.writeInt64(in, ts_ns);
    (void)g_ndk.writeInt32(in, period_ns);
    (void)g_ndk.transact(cb, TX_CB_ON_VSYNC, &in, &out, FLAG_ONEWAY);
    if (out) g_ndk.AParcel_delete(out);
}

static void *vsync_thread_fn(void *arg) {
    (void)arg;
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 2;
    (void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    struct timespec next_ts;
    bool hw_vblank_logged = false;
    clock_gettime(CLOCK_MONOTONIC, &next_ts);
    for (;;) {
        pthread_mutex_lock(&g_disp.lock);
        bool en = g_disp.vsync_enabled && g_disp.crtc_active && g_disp.callback != NULL;
        int32_t period = g_disp.vsync_period_ns;
        int card_fd = g_disp.card_fd;
        uint32_t crtc_idx = g_disp.crtc_index;
        AIBinder *cb = g_disp.callback;
        if (cb) g_ndk.incStrong(cb);
        pthread_mutex_unlock(&g_disp.lock);

        if (!en) {
            if (cb) g_ndk.decStrong(cb);
            struct timespec sl = { 0, 16000000 };
            nanosleep(&sl, NULL);
            clock_gettime(CLOCK_MONOTONIC, &next_ts);
            continue;
        }

        int64_t now_ns = 0;
        bool got_hw = false;
        if (card_fd >= 0) {
            union drm_wait_vblank vbl;
            memset(&vbl, 0, sizeof(vbl));
            vbl.request.type = DRM_VBLANK_RELATIVE;
            if (crtc_idx == 1)
                vbl.request.type |= DRM_VBLANK_SECONDARY;
            else if (crtc_idx > 1)
                vbl.request.type |= (crtc_idx << DRM_VBLANK_HIGH_CRTC_SHIFT);
            vbl.request.sequence = 1;
            if (ioctl(card_fd, DRM_IOCTL_WAIT_VBLANK, &vbl) == 0) {
                now_ns = (int64_t)vbl.reply.tval_sec * 1000000000LL +
                         (int64_t)vbl.reply.tval_usec * 1000LL;
                got_hw = true;
                if (!hw_vblank_logged) {
                    hw_vblank_logged = true;
                    kmsg_log("<6>sailfish_composer_hal: hardware DRM VBLANK VSYNC active\n");
                }
            }
        }

        if (!got_hw) {
            next_ts.tv_nsec += period;
            while (next_ts.tv_nsec >= 1000000000L) {
                next_ts.tv_nsec -= 1000000000L;
                next_ts.tv_sec += 1;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_ts, NULL);
            now_ns = (int64_t)next_ts.tv_sec * 1000000000LL + next_ts.tv_nsec;
        } else {
            clock_gettime(CLOCK_MONOTONIC, &next_ts);
        }

        send_on_vsync(cb, PRIMARY_DISPLAY_ID, now_ns, period);
        g_ndk.decStrong(cb);
    }
    return NULL;
}

/* ---------------- Parcel Parsing Helpers for executeCommands ---------------- */
static int32_t skip_nullable_parcelable(const AParcel *in) {
    int32_t non_null = 0;
    int32_t rc = g_ndk.readInt32(in, &non_null);
    if (rc != STATUS_OK_VAL || non_null == 0) return rc;
    int32_t pos = g_ndk.getDataPosition(in);
    int32_t psize = 0;
    if ((rc = g_ndk.readInt32(in, &psize)) != STATUS_OK_VAL) return rc;
    if (psize < 4) return -EINVAL;
    return g_ndk.setDataPosition(in, pos + psize);
}

typedef struct {
    int32_t *buf;
    int32_t cap;
    int32_t len;
} int32_arr_t;

static bool int32_arr_alloc(void *arrayData, int32_t length, int32_t **outBuffer) {
    int32_arr_t *a = (int32_arr_t *)arrayData;
    if (length < 0) {
        a->len = 0;
        *outBuffer = NULL;
        return true;
    }
    if (length > a->cap) return false;
    a->len = length;
    *outBuffer = a->buf;
    return true;
}

static int32_t parse_native_handle(const AParcel *in, int *out_fd,
                                   sailfish_gralloc_ints_t *out_info, bool *has_info) {
    *out_fd = -1;
    *has_info = false;
    int32_t pos = g_ndk.getDataPosition(in);
    int32_t psize = 0;
    int32_t rc = g_ndk.readInt32(in, &psize);
    if (rc != STATUS_OK_VAL || psize < 4) return -EINVAL;

    int32_t nfds = 0;
    if ((rc = g_ndk.readInt32(in, &nfds)) != STATUS_OK_VAL) return rc;
    for (int32_t i = 0; i < nfds; i++) {
        int fd = -1;
        if ((rc = g_ndk.readParcelFileDescriptor(in, &fd)) != STATUS_OK_VAL) return rc;
        if (i == 0 && *out_fd < 0)
            *out_fd = fd;
        else if (fd >= 0)
            close(fd);
    }

    int32_t raw_ints[64];
    int32_arr_t arr = { .buf = raw_ints, .cap = 64, .len = 0 };
    if (g_ndk.readInt32Array(in, &arr, int32_arr_alloc) == STATUS_OK_VAL &&
        arr.len >= SAILFISH_GRALLOC_NUM_INTS) {
        memcpy(out_info, raw_ints, sizeof(*out_info));
        if (out_info->magic == SAILFISH_GRALLOC_MAGIC)
            *has_info = true;
    }

    return g_ndk.setDataPosition(in, pos + psize);
}

static int32_t parse_buffer(const AParcel *in, int32_t *out_slot, int *out_dmabuf_fd,
                            sailfish_gralloc_ints_t *out_info, bool *has_info,
                            int *out_fence_fd) {
    *out_slot = -1;
    *out_dmabuf_fd = -1;
    *has_info = false;
    *out_fence_fd = -1;

    int32_t pos = g_ndk.getDataPosition(in);
    int32_t psize = 0;
    int32_t rc = g_ndk.readInt32(in, &psize);
    if (rc != STATUS_OK_VAL || psize < 4) return -EINVAL;

    if ((rc = g_ndk.readInt32(in, out_slot)) != STATUS_OK_VAL) return rc;

    int32_t handle_nn = 0;
    if ((rc = g_ndk.readInt32(in, &handle_nn)) != STATUS_OK_VAL) return rc;
    if (handle_nn != 0) {
        if ((rc = parse_native_handle(in, out_dmabuf_fd, out_info, has_info)) != STATUS_OK_VAL)
            return rc;
    }

    int ffd = -1;
    if ((rc = g_ndk.readParcelFileDescriptor(in, &ffd)) != STATUS_OK_VAL) return rc;
    *out_fence_fd = ffd;

    return g_ndk.setDataPosition(in, pos + psize);
}

static int32_t parse_client_target(const AParcel *in, bool *present, int32_t *out_slot,
                                   int *out_dmabuf_fd, sailfish_gralloc_ints_t *out_info,
                                   bool *has_info, int *out_fence_fd) {
    *present = false;
    *out_slot = -1;
    *out_dmabuf_fd = -1;
    *has_info = false;
    *out_fence_fd = -1;

    int32_t nn = 0;
    int32_t rc = g_ndk.readInt32(in, &nn);
    if (rc != STATUS_OK_VAL || nn == 0) return rc;
    *present = true;

    int32_t pos = g_ndk.getDataPosition(in);
    int32_t psize = 0;
    if ((rc = g_ndk.readInt32(in, &psize)) != STATUS_OK_VAL || psize < 4) return -EINVAL;

    int32_t buf_nn = 0;
    if ((rc = g_ndk.readInt32(in, &buf_nn)) != STATUS_OK_VAL) return rc;
    if (buf_nn != 0) {
        if ((rc = parse_buffer(in, out_slot, out_dmabuf_fd, out_info, has_info, out_fence_fd)) != STATUS_OK_VAL)
            return rc;
    }

    return g_ndk.setDataPosition(in, pos + psize);
}

static int32_t skip_nullable_parcelable_array(const AParcel *in, int32_t *out_len) {
    int32_t len = -1;
    int32_t rc = g_ndk.readInt32(in, &len);
    if (out_len) *out_len = len;
    if (rc != STATUS_OK_VAL || len <= 0) return rc;
    for (int32_t i = 0; i < len; i++) {
        if ((rc = skip_nullable_parcelable(in)) != STATUS_OK_VAL) return rc;
    }
    return STATUS_OK_VAL;
}

static int32_t read_nullable_float_matrix_identity(const AParcel *in, bool *present, bool *is_identity) {
    *present = false;
    *is_identity = true;
    int32_t len = -1;
    int32_t rc = g_ndk.readInt32(in, &len);
    if (rc != STATUS_OK_VAL || len < 0) return rc;
    *present = true;
    if (len != 16) {
        *is_identity = (len == 0);
        if (len > 0) {
            int32_t pos = g_ndk.getDataPosition(in);
            return g_ndk.setDataPosition(in, pos + len * 4);
        }
        return STATUS_OK_VAL;
    }
    bool ident = true;
    for (int i = 0; i < 16; i++) {
        float v = 0.0f;
        if ((rc = g_ndk.readFloat(in, &v)) != STATUS_OK_VAL) return rc;
        float exp = ((i % 5) == 0) ? 1.0f : 0.0f;
        float diff = v - exp;
        if (diff < -1e-4f || diff > 1e-4f) ident = false;
    }
    *is_identity = ident;
    return STATUS_OK_VAL;
}

static int32_t read_nullable_int32_parcelable(const AParcel *in, bool *present, int32_t *out_val) {
    *present = false;
    int32_t nn = 0;
    int32_t rc = g_ndk.readInt32(in, &nn);
    if (rc != STATUS_OK_VAL || nn == 0) return rc;
    int32_t pos = g_ndk.getDataPosition(in);
    int32_t psize = 0;
    if ((rc = g_ndk.readInt32(in, &psize)) != STATUS_OK_VAL || psize < 4) return -EINVAL;
    if ((rc = g_ndk.readInt32(in, out_val)) != STATUS_OK_VAL) return rc;
    *present = true;
    return g_ndk.setDataPosition(in, pos + psize);
}

static int32_t read_nullable_float_parcelable(const AParcel *in, bool *present, float *out_val) {
    *present = false;
    int32_t nn = 0;
    int32_t rc = g_ndk.readInt32(in, &nn);
    if (rc != STATUS_OK_VAL || nn == 0) return rc;
    int32_t pos = g_ndk.getDataPosition(in);
    int32_t psize = 0;
    if ((rc = g_ndk.readInt32(in, &psize)) != STATUS_OK_VAL || psize < 4) return -EINVAL;
    if ((rc = g_ndk.readFloat(in, out_val)) != STATUS_OK_VAL) return rc;
    *present = true;
    return g_ndk.setDataPosition(in, pos + psize);
}

static int32_t read_nullable_rect(const AParcel *in, bool *present,
                                  int32_t *l, int32_t *t, int32_t *r, int32_t *b) {
    *present = false;
    int32_t nn = 0;
    int32_t rc = g_ndk.readInt32(in, &nn);
    if (rc != STATUS_OK_VAL || nn == 0) return rc;
    int32_t pos = g_ndk.getDataPosition(in);
    int32_t psize = 0;
    if ((rc = g_ndk.readInt32(in, &psize)) != STATUS_OK_VAL || psize < 4) return -EINVAL;
    if ((rc = g_ndk.readInt32(in, l)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.readInt32(in, t)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.readInt32(in, r)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.readInt32(in, b)) != STATUS_OK_VAL) return rc;
    *present = true;
    return g_ndk.setDataPosition(in, pos + psize);
}

static int32_t read_nullable_frect(const AParcel *in, bool *present,
                                   float *l, float *t, float *r, float *b) {
    *present = false;
    int32_t nn = 0;
    int32_t rc = g_ndk.readInt32(in, &nn);
    if (rc != STATUS_OK_VAL || nn == 0) return rc;
    int32_t pos = g_ndk.getDataPosition(in);
    int32_t psize = 0;
    if ((rc = g_ndk.readInt32(in, &psize)) != STATUS_OK_VAL || psize < 4) return -EINVAL;
    if ((rc = g_ndk.readFloat(in, l)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.readFloat(in, t)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.readFloat(in, r)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.readFloat(in, b)) != STATUS_OK_VAL) return rc;
    *present = true;
    return g_ndk.setDataPosition(in, pos + psize);
}

static int32_t parse_layer_command_locked(const AParcel *in) {
    int32_t lpos = g_ndk.getDataPosition(in);
    int32_t lsize = 0;
    int32_t rc = g_ndk.readInt32(in, &lsize);
    if (rc != STATUS_OK_VAL || lsize < 4) return -EINVAL;
    int32_t lend = lpos + lsize;

    int64_t lid = 0;
    if ((rc = g_ndk.readInt64(in, &lid)) != STATUS_OK_VAL) {
        (void)g_ndk.setDataPosition(in, lend);
        return rc;
    }

    layer_state_t *L = NULL;
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (g_disp.layers[i].used && g_disp.layers[i].id == lid) {
            L = &g_disp.layers[i];
            break;
        }
    }

    /* Field 2: @nullable Point cursorPosition */
    if (g_ndk.getDataPosition(in) < lend)
        if (skip_nullable_parcelable(in) != STATUS_OK_VAL) goto done;

    /* Field 3: @nullable Buffer buffer */
    if (g_ndk.getDataPosition(in) < lend) {
        int32_t bnn = 0;
        if (g_ndk.readInt32(in, &bnn) != STATUS_OK_VAL) goto done;
        if (bnn != 0) {
            int32_t bslot = -1;
            int bfd = -1, bfence = -1;
            sailfish_gralloc_ints_t binfo;
            bool has_binfo = false;
            if (parse_buffer(in, &bslot, &bfd, &binfo, &has_binfo, &bfence) == STATUS_OK_VAL) {
                if (L && bslot >= 0 && bslot < MAX_LAYER_SLOTS) {
                    if (bfd >= 0) {
                        (void)import_slot_buf_locked(&L->slots[bslot], bfd,
                                                     has_binfo ? &binfo : NULL, false);
                    }
                    L->active_slot = bslot;
                    if (L->acquire_fence >= 0) close(L->acquire_fence);
                    L->acquire_fence = bfence;
                    bfence = -1;
                }
            }
            if (bfd >= 0) close(bfd);
            if (bfence >= 0) close(bfence);
        }
    }

    /* Field 4: @nullable Rect[] damage */
    if (g_ndk.getDataPosition(in) < lend)
        if (skip_nullable_parcelable_array(in, NULL) != STATUS_OK_VAL) goto done;

    /* Field 5: @nullable ParcelableBlendMode blendMode */
    if (g_ndk.getDataPosition(in) < lend) {
        bool p = false; int32_t v = 0;
        if (read_nullable_int32_parcelable(in, &p, &v) != STATUS_OK_VAL) goto done;
        if (p && L) L->blend_mode = v;
    }

    /* Field 6: @nullable Color color */
    if (g_ndk.getDataPosition(in) < lend)
        if (skip_nullable_parcelable(in) != STATUS_OK_VAL) goto done;

    /* Field 7: @nullable ParcelableComposition composition */
    if (g_ndk.getDataPosition(in) < lend) {
        bool p = false; int32_t v = 0;
        if (read_nullable_int32_parcelable(in, &p, &v) != STATUS_OK_VAL) goto done;
        if (p && L) L->req_comp = v;
    }

    /* Field 8: @nullable ParcelableDataspace dataspace */
    if (g_ndk.getDataPosition(in) < lend) {
        bool p = false; int32_t v = 0;
        if (read_nullable_int32_parcelable(in, &p, &v) != STATUS_OK_VAL) goto done;
        if (p && L) L->dataspace = v;
    }

    /* Field 9: @nullable Rect displayFrame */
    if (g_ndk.getDataPosition(in) < lend) {
        bool p = false; int32_t l = 0, t = 0, r = 0, b = 0;
        if (read_nullable_rect(in, &p, &l, &t, &r, &b) != STATUS_OK_VAL) goto done;
        if (p && L) { L->df_left = l; L->df_top = t; L->df_right = r; L->df_bottom = b; }
    }

    /* Field 10: @nullable PlaneAlpha planeAlpha */
    if (g_ndk.getDataPosition(in) < lend) {
        bool p = false; float v = 1.0f;
        if (read_nullable_float_parcelable(in, &p, &v) != STATUS_OK_VAL) goto done;
        if (p && L) L->plane_alpha = v;
    }

    /* Field 11: @nullable NativeHandle sidebandStream */
    if (g_ndk.getDataPosition(in) < lend) {
        int32_t snn = 0;
        if (g_ndk.readInt32(in, &snn) != STATUS_OK_VAL) goto done;
        if (snn != 0) {
            int sfd = -1;
            sailfish_gralloc_ints_t sinfo;
            bool has_sinfo = false;
            (void)parse_native_handle(in, &sfd, &sinfo, &has_sinfo);
            if (sfd >= 0) close(sfd);
            if (L) L->has_sideband = true;
        }
    }

    /* Field 12: @nullable FRect sourceCrop */
    if (g_ndk.getDataPosition(in) < lend) {
        bool p = false; float l = 0, t = 0, r = 0, b = 0;
        if (read_nullable_frect(in, &p, &l, &t, &r, &b) != STATUS_OK_VAL) goto done;
        if (p && L) { L->sc_left = l; L->sc_top = t; L->sc_right = r; L->sc_bottom = b; }
    }

    /* Field 13: @nullable ParcelableTransform transform */
    if (g_ndk.getDataPosition(in) < lend) {
        bool p = false; int32_t v = 0;
        if (read_nullable_int32_parcelable(in, &p, &v) != STATUS_OK_VAL) goto done;
        if (p && L) L->transform = v;
    }

    /* Field 14: @nullable Rect[] visibleRegion */
    if (g_ndk.getDataPosition(in) < lend) {
        int32_t vlen = -1;
        if (skip_nullable_parcelable_array(in, &vlen) != STATUS_OK_VAL) goto done;
        if (vlen >= 0 && L) L->visible_empty = (vlen == 0);
    }

    /* Field 15: @nullable ZOrder z */
    if (g_ndk.getDataPosition(in) < lend) {
        bool p = false; int32_t v = 0;
        if (read_nullable_int32_parcelable(in, &p, &v) != STATUS_OK_VAL) goto done;
        if (p && L) L->z = v;
    }

    /* Field 16: @nullable float[] colorTransform */
    if (g_ndk.getDataPosition(in) < lend) {
        bool p = false, ident = true;
        if (read_nullable_float_matrix_identity(in, &p, &ident) != STATUS_OK_VAL) goto done;
        if (p && L) L->has_color_transform = !ident;
    }

    /* Field 17: @nullable LayerBrightness brightness */
    if (g_ndk.getDataPosition(in) < lend) {
        bool p = false; float br = 1.0f;
        if (read_nullable_float_parcelable(in, &p, &br) != STATUS_OK_VAL) goto done;
        if (p && L) L->has_brightness = (br < 0.999f || br > 1.001f);
    }

done:
    return g_ndk.setDataPosition(in, lend);
}

static bool layer_is_hw_eligible(const layer_state_t *L) {
    if (!L || !L->used) return false;
    if (L->req_comp != COMP_DEVICE) return false;
    if (L->active_slot < 0 || L->active_slot >= MAX_LAYER_SLOTS) return false;
    const client_slot_t *s = &L->slots[L->active_slot];
    if (!s->fb_id || s->w == 0 || s->h == 0) return false;
    if (L->transform != 0) return false;
    if (L->has_color_transform || L->has_brightness || L->has_sideband || L->visible_empty)
        return false;
    if (L->blend_mode != BLEND_NONE && L->blend_mode != BLEND_PREMULTIPLIED &&
        L->blend_mode != BLEND_COVERAGE)
        return false;
    if (L->plane_alpha < 0.999f || L->plane_alpha > 1.0f) return false;
    /* Reject HDR transfer functions (ST2084=7<<22, HLG=8<<22) */
    uint32_t xfer = ((uint32_t)L->dataspace >> 22) & 0x1fU;
    if (xfer == 7U || xfer == 8U) return false;

    int32_t dw = L->df_right - L->df_left;
    int32_t dh = L->df_bottom - L->df_top;
    if (dw < 16 || dh < 16) return false;
    if (L->df_left < 0 || L->df_top < 0 ||
        (uint32_t)L->df_right > g_disp.disp_w || (uint32_t)L->df_bottom > g_disp.disp_h)
        return false;

    float sw = L->sc_right - L->sc_left;
    float sh = L->sc_bottom - L->sc_top;
    if (sw < 16.0f || sh < 16.0f) return false;
    if (L->sc_left < 0.0f || L->sc_top < 0.0f ||
        L->sc_right > (float)s->w + 0.01f || L->sc_bottom > (float)s->h + 0.01f)
        return false;

    /* Restrict MDP5 HW overlays to 1:1 unscaled integer crops so window-shrink/zoom
     * animations (e.g. swipe-home) use GPU composition and never trigger MDP5 QSEED2
     * mid-animation pipe-switch glitches. */
    float dx = sw - (float)dw;
    float dy = sh - (float)dh;
    if (dx < -0.01f || dx > 0.01f || dy < -0.01f || dy > 0.01f)
        return false;
    float cl_err = L->sc_left - (float)(int32_t)(L->sc_left + 0.5f);
    float ct_err = L->sc_top - (float)(int32_t)(L->sc_top + 0.5f);
    if (cl_err < -0.01f || cl_err > 0.01f || ct_err < -0.01f || ct_err > 0.01f)
        return false;

    return true;
}

/* ---------------- IComposerClient Transaction Handlers ---------------- */
static int32_t handle_execute_commands(const AParcel *in, AParcel *out) {
    int32_t n_cmds = 0;
    int32_t rc = g_ndk.readInt32(in, &n_cmds);
    if (rc != STATUS_OK_VAL || n_cmds < 0)
        return write_status_err(out, HWC3_EX_BAD_PARAMETER);

    /* Collect payloads to emit (up to 4 per display command) */
    typedef struct {
        int32_t tag;
        int64_t display;
        int8_t pov_result;
        int fence_fd;
        int64_t changed_layers[MAX_LAYERS];
        int32_t changed_types[MAX_LAYERS];
        int32_t n_changed;
        int64_t rel_layers[MAX_HW_PLANES];
        int rel_fences[MAX_HW_PLANES];
        int32_t n_rel;
    } payload_t;

    payload_t payloads[16];
    int32_t n_payloads = 0;

    for (int32_t c = 0; c < n_cmds; c++) {
        int32_t nn = 0;
        if ((rc = g_ndk.readInt32(in, &nn)) != STATUS_OK_VAL) break;
        if (nn == 0) continue;

        int32_t cmd_pos = g_ndk.getDataPosition(in);
        int32_t cmd_size = 0;
        if ((rc = g_ndk.readInt32(in, &cmd_size)) != STATUS_OK_VAL || cmd_size < 4) break;

        int64_t display = 0;
        if ((rc = g_ndk.readInt64(in, &display)) != STATUS_OK_VAL) break;

        /* LayerCommand[] layers */
        int32_t n_layers = 0;
        if ((rc = g_ndk.readInt32(in, &n_layers)) != STATUS_OK_VAL) break;
        pthread_mutex_lock(&g_disp.lock);
        for (int32_t l = 0; l < n_layers; l++) {
            int32_t lnn = 0;
            if ((rc = g_ndk.readInt32(in, &lnn)) != STATUS_OK_VAL) break;
            if (lnn == 0) continue;
            if ((rc = parse_layer_command_locked(in)) != STATUS_OK_VAL) break;
        }
        pthread_mutex_unlock(&g_disp.lock);
        if (rc != STATUS_OK_VAL) break;

        /* @nullable float[] colorTransform (display-wide) */
        bool ct_mat_present = false, ct_mat_ident = true;
        if ((rc = read_nullable_float_matrix_identity(in, &ct_mat_present, &ct_mat_ident)) != STATUS_OK_VAL)
            break;
        if (ct_mat_present) {
            pthread_mutex_lock(&g_disp.lock);
            g_disp.disp_has_color_transform = !ct_mat_ident;
            pthread_mutex_unlock(&g_disp.lock);
        }

        /* DisplayBrightness brightness (nullable parcelable: float brightness) */
        {
            int32_t bnn = 0;
            if ((rc = g_ndk.readInt32(in, &bnn)) != STATUS_OK_VAL) break;
            if (bnn != 0) {
                int32_t bpos = g_ndk.getDataPosition(in);
                int32_t bsize = 0;
                if ((rc = g_ndk.readInt32(in, &bsize)) != STATUS_OK_VAL || bsize < 4) break;
                float br = -1.0f;
                if (g_ndk.readFloat(in, &br) == STATUS_OK_VAL) {
                    if (br >= 0.0f) {
                        int level = (int)(br * 255.0f + 0.5f);
                        if (level < 8) level = 8;
                        if (level > 255) level = 255;
                        g_last_brightness = level;
                        set_backlight_sysfs(0, level);
                    } else {
                        set_backlight_sysfs(4, 0);
                    }
                }
                (void)g_ndk.setDataPosition(in, bpos + bsize);
            }
        }

        bool has_ct = false;
        int32_t ct_slot = -1;
        int ct_fd = -1, ct_fence = -1;
        sailfish_gralloc_ints_t ct_info;
        bool has_ct_info = false;
        if ((rc = parse_client_target(in, &has_ct, &ct_slot, &ct_fd, &ct_info,
                                      &has_ct_info, &ct_fence)) != STATUS_OK_VAL) break;

        if ((rc = skip_nullable_parcelable(in)) != STATUS_OK_VAL) break; /* virtualDisplayOutputBuffer */
        if ((rc = skip_nullable_parcelable(in)) != STATUS_OK_VAL) break; /* expectedPresentTime */

        bool validate_disp = false, accept_changes = false, present_disp = false, pov = false;
        (void)g_ndk.readBool(in, &validate_disp);
        (void)g_ndk.readBool(in, &accept_changes);
        (void)g_ndk.readBool(in, &present_disp);
        (void)g_ndk.readBool(in, &pov);

        (void)g_ndk.setDataPosition(in, cmd_pos + cmd_size);

        pthread_mutex_lock(&g_disp.lock);
        if (has_ct && ct_slot >= 0 && ct_slot < MAX_CLIENT_SLOTS) {
            if (ct_fd >= 0) {
                import_client_target_locked(ct_slot, ct_fd, has_ct_info ? &ct_info : NULL);
                close(ct_fd);
                ct_fd = -1;
            }
            g_disp.active_slot = ct_slot;
        }
        if (ct_fd >= 0) close(ct_fd);

        if (validate_disp || pov) {
            /* Collect active layers sorted ascending by z */
            int active_idx[MAX_LAYERS];
            int n_active = 0;
            for (int i = 0; i < MAX_LAYERS; i++) {
                if (g_disp.layers[i].used)
                    active_idx[n_active++] = i;
            }
            for (int i = 1; i < n_active; i++) {
                int key = active_idx[i];
                int j = i - 1;
                while (j >= 0 && g_disp.layers[active_idx[j]].z > g_disp.layers[key].z) {
                    active_idx[j + 1] = active_idx[j];
                    j--;
                }
                active_idx[j + 1] = key;
            }

            /* Try full MDP5 hardware overlay composition when all layers are eligible */
            bool all_hw = (!g_disp.disp_has_color_transform &&
                           n_active > 0 && (uint32_t)n_active <= g_disp.n_planes);
            for (int i = 0; all_hw && i < n_active; i++) {
                if (!layer_is_hw_eligible(&g_disp.layers[active_idx[i]]))
                    all_hw = false;
            }
            if (all_hw && !drm_test_device_planes_locked(active_idx, n_active)) {
                all_hw = false;
            }

            g_disp.validated_all_device = all_hw;
            g_disp.n_validated_device = all_hw ? n_active : 0;
            if (all_hw) {
                for (int i = 0; i < n_active; i++)
                    g_disp.validated_device_idx[i] = active_idx[i];
            }

            int32_t target_comp = all_hw ? COMP_DEVICE : COMP_CLIENT;
            payload_t ch;
            memset(&ch, 0, sizeof(ch));
            ch.tag = CRP_TAG_CHANGED_COMPOSITION_TYPES;
            ch.display = display;
            ch.fence_fd = -1;
            for (int i = 0; i < n_active; i++) {
                layer_state_t *L = &g_disp.layers[active_idx[i]];
                if (L->req_comp != target_comp) {
                    ch.changed_layers[ch.n_changed] = L->id;
                    ch.changed_types[ch.n_changed] = target_comp;
                    ch.n_changed++;
                }
                L->comp_type = target_comp;
            }
            if (ch.n_changed > 0 && n_payloads < 16)
                payloads[n_payloads++] = ch;

            if (pov && n_payloads < 16) {
                /* Present immediately if no composition type changes AND either:
                 *  - all layers are MDP5 DEVICE overlays (no clientTarget needed), OR
                 *  - CLIENT composition with a fresh clientTarget already supplied. */
                bool can_present_now = (ch.n_changed == 0) &&
                    (all_hw || (has_ct && g_disp.active_slot >= 0));
                payload_t pr;
                memset(&pr, 0, sizeof(pr));
                pr.tag = CRP_TAG_PRESENT_OR_VALIDATE;
                pr.display = display;
                pr.pov_result = can_present_now ? 1 : 0;
                pr.fence_fd = -1;
                payloads[n_payloads++] = pr;
                if (can_present_now)
                    present_disp = true;
            }
        }

        if (present_disp && n_payloads < 16) {
            bool use_dev = g_disp.validated_all_device;
            int p_fence = drm_commit_present_locked(g_disp.active_slot, ct_fence, use_dev);
            ct_fence = -1;

            payload_t pf;
            memset(&pf, 0, sizeof(pf));
            pf.tag = CRP_TAG_PRESENT_FENCE;
            pf.display = display;
            pf.fence_fd = p_fence;
            payloads[n_payloads++] = pf;

            /* Emit RELEASE_FENCES for the previous frame's DEVICE layers */
            if (g_disp.n_prev_device_layers > 0 && n_payloads < 16) {
                payload_t rf;
                memset(&rf, 0, sizeof(rf));
                rf.tag = CRP_TAG_RELEASE_FENCES;
                rf.display = display;
                rf.fence_fd = -1;
                for (int32_t i = 0; i < g_disp.n_prev_device_layers; i++) {
                    rf.rel_layers[rf.n_rel] = g_disp.prev_device_layers[i];
                    rf.rel_fences[rf.n_rel] = (p_fence >= 0)
                        ? fcntl(p_fence, F_DUPFD_CLOEXEC, 0)
                        : drm_create_signaled_fence_locked();
                    rf.n_rel++;
                }
                payloads[n_payloads++] = rf;
            }

            g_disp.n_prev_device_layers = 0;
            if (use_dev) {
                for (int i = 0; i < g_disp.n_validated_device; i++) {
                    g_disp.prev_device_layers[g_disp.n_prev_device_layers++] =
                        g_disp.layers[g_disp.validated_device_idx[i]].id;
                }
            }
        }
        pthread_mutex_unlock(&g_disp.lock);

        if (ct_fence >= 0) close(ct_fence);
    }

    if ((rc = write_status_ok(out)) != STATUS_OK_VAL) goto out_cleanup;
    if ((rc = g_ndk.writeInt32(out, n_payloads)) != STATUS_OK_VAL) goto out_cleanup;

    for (int32_t i = 0; i < n_payloads; i++) {
        payload_t *p = &payloads[i];
        /* Non-null CommandResultPayload union */
        if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) goto out_cleanup;
        if ((rc = g_ndk.writeInt32(out, p->tag)) != STATUS_OK_VAL) goto out_cleanup;
        if (p->tag == CRP_TAG_CHANGED_COMPOSITION_TYPES) {
            if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) goto out_cleanup;
            int32_t pos = 0;
            if ((rc = begin_parcelable(out, &pos)) != STATUS_OK_VAL) goto out_cleanup;
            if ((rc = g_ndk.writeInt64(out, p->display)) != STATUS_OK_VAL) goto out_cleanup;
            if ((rc = g_ndk.writeInt32(out, p->n_changed)) != STATUS_OK_VAL) goto out_cleanup;
            for (int32_t j = 0; j < p->n_changed; j++) {
                if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) goto out_cleanup;
                int32_t lpos = 0;
                if ((rc = begin_parcelable(out, &lpos)) != STATUS_OK_VAL) goto out_cleanup;
                if ((rc = g_ndk.writeInt64(out, p->changed_layers[j])) != STATUS_OK_VAL) goto out_cleanup;
                if ((rc = g_ndk.writeInt32(out, p->changed_types[j])) != STATUS_OK_VAL) goto out_cleanup;
                if ((rc = end_parcelable(out, lpos)) != STATUS_OK_VAL) goto out_cleanup;
            }
            if ((rc = end_parcelable(out, pos)) != STATUS_OK_VAL) goto out_cleanup;
        } else if (p->tag == CRP_TAG_PRESENT_FENCE) {
            if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) goto out_cleanup;
            int32_t pos = 0;
            if ((rc = begin_parcelable(out, &pos)) != STATUS_OK_VAL) goto out_cleanup;
            if ((rc = g_ndk.writeInt64(out, p->display)) != STATUS_OK_VAL) goto out_cleanup;
            if ((rc = g_ndk.writeParcelFileDescriptor(out, p->fence_fd)) != STATUS_OK_VAL) goto out_cleanup;
            if ((rc = end_parcelable(out, pos)) != STATUS_OK_VAL) goto out_cleanup;
        } else if (p->tag == CRP_TAG_RELEASE_FENCES) {
            if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) goto out_cleanup;
            int32_t pos = 0;
            if ((rc = begin_parcelable(out, &pos)) != STATUS_OK_VAL) goto out_cleanup;
            if ((rc = g_ndk.writeInt64(out, p->display)) != STATUS_OK_VAL) goto out_cleanup;
            if ((rc = g_ndk.writeInt32(out, p->n_rel)) != STATUS_OK_VAL) goto out_cleanup;
            for (int32_t j = 0; j < p->n_rel; j++) {
                if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) goto out_cleanup;
                int32_t lpos = 0;
                if ((rc = begin_parcelable(out, &lpos)) != STATUS_OK_VAL) goto out_cleanup;
                if ((rc = g_ndk.writeInt64(out, p->rel_layers[j])) != STATUS_OK_VAL) goto out_cleanup;
                if ((rc = g_ndk.writeParcelFileDescriptor(out, p->rel_fences[j])) != STATUS_OK_VAL) goto out_cleanup;
                if ((rc = end_parcelable(out, lpos)) != STATUS_OK_VAL) goto out_cleanup;
            }
            if ((rc = end_parcelable(out, pos)) != STATUS_OK_VAL) goto out_cleanup;
        } else if (p->tag == CRP_TAG_PRESENT_OR_VALIDATE) {
            if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) goto out_cleanup;
            int32_t pos = 0;
            if ((rc = begin_parcelable(out, &pos)) != STATUS_OK_VAL) goto out_cleanup;
            if ((rc = g_ndk.writeInt64(out, p->display)) != STATUS_OK_VAL) goto out_cleanup;
            if ((rc = g_ndk.writeByte(out, p->pov_result)) != STATUS_OK_VAL) goto out_cleanup;
            if ((rc = end_parcelable(out, pos)) != STATUS_OK_VAL) goto out_cleanup;
        }
    }

out_cleanup:
    for (int32_t i = 0; i < n_payloads; i++) {
        if (payloads[i].fence_fd >= 0) close(payloads[i].fence_fd);
        for (int32_t j = 0; j < payloads[i].n_rel; j++) {
            if (payloads[i].rel_fences[j] >= 0) close(payloads[i].rel_fences[j]);
        }
    }
    return rc;
}

static void *cli_on_create(void *args) { return args; }
static void cli_on_destroy(void *userData) { (void)userData; }

static int32_t cli_on_transact(AIBinder *binder, uint32_t code,
                               const AParcel *in, AParcel *out) {
    (void)binder;
    int32_t rc;
    int64_t display = 0;

    switch (code) {
    case TX_CLI_CREATE_LAYER: {
        int32_t slot_cnt = 0;
        (void)g_ndk.readInt64(in, &display);
        (void)g_ndk.readInt32(in, &slot_cnt);
        int64_t lid = -1;
        pthread_mutex_lock(&g_disp.lock);
        for (int i = 0; i < MAX_LAYERS; i++) {
            if (!g_disp.layers[i].used) {
                layer_state_t *L = &g_disp.layers[i];
                memset(L, 0, sizeof(*L));
                L->used = true;
                L->id = g_disp.next_layer_id++;
                L->req_comp = COMP_DEVICE;
                L->comp_type = 0;
                L->blend_mode = BLEND_PREMULTIPLIED;
                L->plane_alpha = 1.0f;
                L->active_slot = -1;
                L->acquire_fence = -1;
                lid = L->id;
                break;
            }
        }
        pthread_mutex_unlock(&g_disp.lock);
        if (lid < 0) return write_status_err(out, HWC3_EX_NO_RESOURCES);
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt64(out, lid);
    }
    case TX_CLI_DESTROY_LAYER: {
        int64_t lid = 0;
        (void)g_ndk.readInt64(in, &display);
        (void)g_ndk.readInt64(in, &lid);
        pthread_mutex_lock(&g_disp.lock);
        for (int i = 0; i < MAX_LAYERS; i++) {
            if (g_disp.layers[i].used && g_disp.layers[i].id == lid) {
                layer_destroy_resources_locked(&g_disp.layers[i]);
                g_disp.layers[i].used = false;
                break;
            }
        }
        pthread_mutex_unlock(&g_disp.lock);
        return write_status_ok(out);
    }
    case TX_CLI_EXECUTE_COMMANDS:
        return handle_execute_commands(in, out);
    case TX_CLI_GET_ACTIVE_CONFIG:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 0);
    case TX_CLI_GET_COLOR_MODES: {
        int32_t modes[1] = { 0 }; /* ColorMode::NATIVE */
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32Array(out, modes, 1);
    }
    case TX_CLI_GET_DATASPACE_SATURATION_MATRIX: {
        float id[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeFloatArray(out, id, 16);
    }
    case TX_CLI_GET_DISPLAY_ATTRIBUTE: {
        int32_t cfg = 0, attr = 0;
        (void)g_ndk.readInt64(in, &display);
        (void)g_ndk.readInt32(in, &cfg);
        (void)g_ndk.readInt32(in, &attr);
        int32_t val = 0;
        switch (attr) {
        case DISP_ATTR_WIDTH:        val = (int32_t)g_disp.disp_w; break;
        case DISP_ATTR_HEIGHT:       val = (int32_t)g_disp.disp_h; break;
        case DISP_ATTR_VSYNC_PERIOD: val = g_disp.vsync_period_ns; break;
        case DISP_ATTR_DPI_X:        val = g_disp.dpi_x; break;
        case DISP_ATTR_DPI_Y:        val = g_disp.dpi_y; break;
        case DISP_ATTR_CONFIG_GROUP: val = 0; break;
        default: return write_status_err(out, HWC3_EX_BAD_PARAMETER);
        }
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, val);
    }
    case TX_CLI_GET_DISPLAY_CAPABILITIES: {
        int32_t caps[1] = { 3 }; /* DisplayCapability::BRIGHTNESS = 3 */
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32Array(out, caps, 1);
    }
    case TX_CLI_GET_DISPLAY_CONFIGS: {
        int32_t cfgs[1] = { 0 };
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32Array(out, cfgs, 1);
    }
    case TX_CLI_GET_DISPLAY_CONNECTION_TYPE:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 0); /* INTERNAL */
    case TX_CLI_GET_DISPLAY_NAME: {
        static const char name[] = "Sailfish Primary";
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeString(out, name, (int32_t)(sizeof(name) - 1));
    }
    case TX_CLI_GET_DISPLAY_VSYNC_PERIOD:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, g_disp.vsync_period_ns);
    case TX_CLI_GET_DISPLAY_PHYSICAL_ORIENTATION:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 0); /* NONE */
    case TX_CLI_GET_HDR_CAPABILITIES: {
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) return rc;
        int32_t pos = 0;
        if ((rc = begin_parcelable(out, &pos)) != STATUS_OK_VAL) return rc;
        if ((rc = g_ndk.writeInt32Array(out, NULL, 0)) != STATUS_OK_VAL) return rc;
        if ((rc = g_ndk.writeFloat(out, 0.0f)) != STATUS_OK_VAL) return rc;
        if ((rc = g_ndk.writeFloat(out, 0.0f)) != STATUS_OK_VAL) return rc;
        if ((rc = g_ndk.writeFloat(out, 0.0f)) != STATUS_OK_VAL) return rc;
        return end_parcelable(out, pos);
    }
    case TX_CLI_GET_MAX_VIRTUAL_DISPLAY_COUNT:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 0);
    case TX_CLI_GET_PER_FRAME_METADATA_KEYS:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32Array(out, NULL, 0);
    case TX_CLI_GET_RENDER_INTENTS: {
        int32_t intents[1] = { 0 }; /* COLORIMETRIC */
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32Array(out, intents, 1);
    }
    case TX_CLI_GET_SUPPORTED_CONTENT_TYPES:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32Array(out, NULL, 0);
    case TX_CLI_GET_DISPLAY_DECORATION_SUPPORT:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 0); /* null optional */
    case TX_CLI_REGISTER_CALLBACK: {
        AIBinder *cb = NULL;
        if ((rc = g_ndk.readStrongBinder(in, &cb)) != STATUS_OK_VAL)
            return write_status_err(out, HWC3_EX_BAD_PARAMETER);
        if (cb && g_cb_cls)
            (void)g_ndk.associateClass(cb, g_cb_cls);
        pthread_mutex_lock(&g_disp.lock);
        if (g_disp.callback) g_ndk.decStrong(g_disp.callback);
        g_disp.callback = cb;
        if (!g_disp.vsync_thread_started) {
            g_disp.vsync_thread_started = true;
            pthread_create(&g_disp.vsync_thread, NULL, vsync_thread_fn, NULL);
        }
        pthread_mutex_unlock(&g_disp.lock);
        if (cb)
            send_on_hotplug(cb, PRIMARY_DISPLAY_ID, true);
        return write_status_ok(out);
    }
    case TX_CLI_SET_ACTIVE_CONFIG:
    case TX_CLI_SET_CLIENT_TARGET_SLOT_COUNT:
    case TX_CLI_SET_COLOR_MODE:
    case TX_CLI_SET_CONTENT_TYPE:
        return write_status_ok(out);
    case TX_CLI_SET_ACTIVE_CONFIG_WITH_CONSTRAINTS: {
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) return rc;
        int32_t pos = 0;
        if ((rc = begin_parcelable(out, &pos)) != STATUS_OK_VAL) return rc;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        if ((rc = g_ndk.writeInt64(out, now_ns)) != STATUS_OK_VAL) return rc;
        if ((rc = g_ndk.writeBool(out, false)) != STATUS_OK_VAL) return rc;
        if ((rc = g_ndk.writeInt64(out, now_ns)) != STATUS_OK_VAL) return rc;
        return end_parcelable(out, pos);
    }
    case TX_CLI_SET_POWER_MODE: {
        int32_t mode = POWER_MODE_ON;
        (void)g_ndk.readInt64(in, &display);
        (void)g_ndk.readInt32(in, &mode);
        pthread_mutex_lock(&g_disp.lock);
        drm_set_power_locked(mode != POWER_MODE_OFF);
        pthread_mutex_unlock(&g_disp.lock);
        return write_status_ok(out);
    }
    case TX_CLI_SET_VSYNC_ENABLED: {
        bool en = false;
        (void)g_ndk.readInt64(in, &display);
        (void)g_ndk.readBool(in, &en);
        pthread_mutex_lock(&g_disp.lock);
        g_disp.vsync_enabled = en;
        pthread_mutex_unlock(&g_disp.lock);
        return write_status_ok(out);
    }
    case TX_CLI_GET_DISPLAY_IDENTIFICATION_DATA:
    case TX_CLI_GET_DISPLAYED_CONTENT_SAMPLE:
    case TX_CLI_GET_DISPLAYED_CONTENT_SAMPLING_ATTR:
    case TX_CLI_GET_READBACK_BUFFER_ATTRIBUTES:
    case TX_CLI_GET_READBACK_BUFFER_FENCE:
    case TX_CLI_SET_BOOT_DISPLAY_CONFIG:
    case TX_CLI_CLEAR_BOOT_DISPLAY_CONFIG:
    case TX_CLI_GET_PREFERRED_BOOT_DISPLAY_CONFIG:
    case TX_CLI_SET_AUTO_LOW_LATENCY_MODE:
    case TX_CLI_SET_DISPLAYED_CONTENT_SAMPLING_EN:
    case TX_CLI_SET_READBACK_BUFFER:
    case TX_CLI_SET_IDLE_TIMER_ENABLED:
    case TX_CLI_CREATE_VIRTUAL_DISPLAY:
    case TX_CLI_DESTROY_VIRTUAL_DISPLAY:
    case TX_CLI_GET_OVERLAY_SUPPORT:
    case TX_CLI_GET_HDR_CONVERSION_CAPABILITIES:
    case TX_CLI_SET_HDR_CONVERSION_STRATEGY:
    case TX_CLI_GET_DISPLAY_CONFIGURATIONS:
        return write_status_err(out, HWC3_EX_UNSUPPORTED);
    case TX_CLI_SET_REFRESH_RATE_DEBUG_ENABLED:
    case TX_CLI_NOTIFY_EXPECTED_PRESENT:
        return write_status_ok(out);
    case TX_CLI_GET_MAX_LAYER_PICTURE_PROFILES:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 0);
    case TX_GET_INTERFACE_VERSION:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 1);
    case TX_GET_INTERFACE_HASH:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeString(out, ICOMP_HASH, (int32_t)(sizeof(ICOMP_HASH) - 1));
    default:
        return STATUS_UNKNOWN_TX_VAL;
    }
}

static AIBinder_Class *g_cli_cls = NULL;

static void *comp_on_create(void *args) { return args; }
static void comp_on_destroy(void *userData) { (void)userData; }

static int32_t comp_on_transact(AIBinder *binder, uint32_t code,
                                const AParcel *in, AParcel *out) {
    (void)binder;
    (void)in;
    int32_t rc;
    switch (code) {
    case TX_COMP_CREATE_CLIENT: {
        AIBinder *cli = g_ndk.AIBinder_new(g_cli_cls, NULL);
        if (!cli) return write_status_err(out, HWC3_EX_NO_RESOURCES);
        g_ndk.markVintfStability(cli);
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) {
            g_ndk.decStrong(cli);
            return rc;
        }
        rc = g_ndk.writeStrongBinder(out, cli);
        g_ndk.decStrong(cli);
        return rc;
    }
    case TX_COMP_GET_CAPABILITIES:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32Array(out, NULL, 0);
    case TX_GET_INTERFACE_VERSION:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 1);
    case TX_GET_INTERFACE_HASH:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeString(out, ICOMP_HASH, (int32_t)(sizeof(ICOMP_HASH) - 1));
    default:
        return STATUS_UNKNOWN_TX_VAL;
    }
}

int main(void) {
    void *lib = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!lib || !resolve_ndk(lib)) {
        kmsg_log("<3>sailfish_composer_hal: failed to load libbinder_ndk.so\n");
        return 1;
    }

    pthread_mutex_lock(&g_disp.lock);
    (void)drm_init_locked();
    pthread_mutex_unlock(&g_disp.lock);

    g_ndk.setThreadPoolMaxThreadCount(4);
    g_ndk.startThreadPool();

    g_cb_cls = g_ndk.Class_define(ICB_DESC, cli_on_create, cli_on_destroy, cli_on_transact);
    g_cli_cls = g_ndk.Class_define(ICLI_DESC, cli_on_create, cli_on_destroy, cli_on_transact);
    AIBinder_Class *comp_cls = g_ndk.Class_define(ICOMP_DESC, comp_on_create, comp_on_destroy, comp_on_transact);
    if (!g_cb_cls || !g_cli_cls || !comp_cls) {
        kmsg_log("<3>sailfish_composer_hal: AIBinder_Class_define failed\n");
        return 1;
    }

    AIBinder *comp = g_ndk.AIBinder_new(comp_cls, NULL);
    if (!comp) return 1;
    g_ndk.markVintfStability(comp);

    int32_t st = g_ndk.addService(comp, ICOMP_INSTANCE);
    if (st != STATUS_OK_VAL) {
        kmsg_log("<3>sailfish_composer_hal: AServiceManager_addService failed\n");
        return 1;
    }

    kmsg_log("<6>sailfish_composer_hal: registered android.hardware.graphics.composer3.IComposer/default (V1)\n");
    g_ndk.joinThreadPool();
    return 0;
}
