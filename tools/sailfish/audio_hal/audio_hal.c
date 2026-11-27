/*
 * Sailfish Native Android 17 AIDL Audio HAL
 * Implements android.hardware.audio.core.{IConfig,IModule,IStreamOut,IStreamCommon}/default (V5)
 * directly over mainline Linux 6.18 ALSA (/dev/snd/controlC0 + /dev/snd/pcmC0D0p).
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>

#define AUDIO_HAL_VERSION 5
#define AUDIO_HAL_HASH    "bd0612235335e8a7bd07714095b17d06e54fbd0e"

#define DESC_ICONFIG       "android.hardware.audio.core.IConfig"
#define DESC_IMODULE       "android.hardware.audio.core.IModule"
#define DESC_ISTREAM_OUT   "android.hardware.audio.core.IStreamOut"
#define DESC_ISTREAM_COMM  "android.hardware.audio.core.IStreamCommon"
#define DESC_IFACTORY      "android.hardware.audio.effect.IFactory"
#define DESC_BT_FACTORY    "android.hardware.bluetooth.audio.IBluetoothAudioProviderFactory"
#define DESC_BT_PROVIDER   "android.hardware.bluetooth.audio.IBluetoothAudioProvider"
#define DESC_BT_PORT       "android.hardware.bluetooth.audio.IBluetoothAudioPort"
#define BT_HAL_VERSION     2

#define TX_GET_VERSION 16777215u
#define TX_GET_HASH    16777214u

#define STATUS_OK 0
#define EX_UNSUPPORTED_OPERATION (-7)
#define EX_ILLEGAL_ARGUMENT      (-3)

/* Opaque libbinder_ndk types */
typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;
typedef int32_t binder_status_t;
typedef uint32_t transaction_code_t;

typedef void *(*AIBinder_Class_onCreate)(void *args);
typedef void (*AIBinder_Class_onDestroy)(void *userData);
typedef binder_status_t (*AIBinder_Class_onTransact)(AIBinder *binder,
                                                     transaction_code_t code,
                                                     const AParcel *in,
                                                     AParcel *out);
typedef bool (*AParcel_stringAllocator)(void *stringData, int32_t length, char **buffer);
typedef bool (*AParcel_byteArrayAllocator)(void *arrayData, int32_t length, int8_t **outBuffer);
typedef int32_t (*AParcel_writeParcelableElement)(AParcel *parcel,
                                                  const void *arrayData,
                                                  size_t index);

static struct {
    AIBinder_Class *(*Class_define)(const char *, AIBinder_Class_onCreate,
                                    AIBinder_Class_onDestroy, AIBinder_Class_onTransact);
    AIBinder *(*newBinder)(const AIBinder_Class *, void *);
    void *(*getUserData)(AIBinder *);
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
    binder_status_t (*readInt64)(const AParcel *, int64_t *);
    binder_status_t (*readFloat)(const AParcel *, float *);
    binder_status_t (*readBool)(const AParcel *, bool *);
    binder_status_t (*readByte)(const AParcel *, int8_t *);
    binder_status_t (*readString)(const AParcel *, void *, AParcel_stringAllocator);
    binder_status_t (*readByteArray)(const AParcel *, void *, AParcel_byteArrayAllocator);
    binder_status_t (*readStrongBinder)(const AParcel *, AIBinder **);
    int32_t (*getDataPosition)(const AParcel *);
    binder_status_t (*setDataPosition)(const AParcel *, int32_t);

    binder_status_t (*writeInt32)(AParcel *, int32_t);
    binder_status_t (*writeInt64)(AParcel *, int64_t);
    binder_status_t (*writeFloat)(AParcel *, float);
    binder_status_t (*writeBool)(AParcel *, bool);
    binder_status_t (*writeByte)(AParcel *, int8_t);
    binder_status_t (*writeString)(AParcel *, const char *, int32_t);
    binder_status_t (*writeInt32Array)(AParcel *, const int32_t *, int32_t);
    binder_status_t (*writeByteArray)(AParcel *, const int8_t *, int32_t);
    binder_status_t (*writeParcelFileDescriptor)(AParcel *, int);
    binder_status_t (*writeStrongBinder)(AParcel *, AIBinder *);
    binder_status_t (*writeParcelableArray)(AParcel *, const void *, int32_t,
                                            AParcel_writeParcelableElement);
} g_bn;

static AIBinder_Class *g_cls_config;
static AIBinder_Class *g_cls_module;
static AIBinder_Class *g_cls_stream_out;
static AIBinder_Class *g_cls_stream_comm;
static AIBinder_Class *g_cls_factory;
static AIBinder_Class *g_cls_bt_factory;
static AIBinder_Class *g_cls_bt_provider;
static AIBinder_Class *g_cls_bt_port;

static int (*g_alog_vprint)(int prio, const char *tag, const char *fmt, va_list ap);

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_alog_vprint) {
        g_alog_vprint(4 /* ANDROID_LOG_INFO */, "sailfish_audio", fmt, ap);
    } else {
        fprintf(stderr, "[sailfish_audio] ");
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
    LOAD(getUserData, "AIBinder_getUserData");
    *(void **)&g_bn.markVintfStability = dlsym(h, "AIBinder_markVintfStability");
    LOAD(incStrong, "AIBinder_incStrong");
    LOAD(decStrong, "AIBinder_decStrong");
    LOAD(addService, "AServiceManager_addService");
    LOAD(associateClass, "AIBinder_associateClass");
    LOAD(prepareTransaction, "AIBinder_prepareTransaction");
    LOAD(transact, "AIBinder_transact");
    LOAD(parcelDelete, "AParcel_delete");
    LOAD(startThreadPool, "ABinderProcess_startThreadPool");
    LOAD(joinThreadPool, "ABinderProcess_joinThreadPool");
    LOAD(readInt32, "AParcel_readInt32");
    LOAD(readInt64, "AParcel_readInt64");
    LOAD(readFloat, "AParcel_readFloat");
    LOAD(readBool, "AParcel_readBool");
    LOAD(readByte, "AParcel_readByte");
    LOAD(readString, "AParcel_readString");
    LOAD(readByteArray, "AParcel_readByteArray");
    LOAD(readStrongBinder, "AParcel_readStrongBinder");
    LOAD(getDataPosition, "AParcel_getDataPosition");
    LOAD(setDataPosition, "AParcel_setDataPosition");
    LOAD(writeInt32, "AParcel_writeInt32");
    LOAD(writeInt64, "AParcel_writeInt64");
    LOAD(writeFloat, "AParcel_writeFloat");
    LOAD(writeBool, "AParcel_writeBool");
    LOAD(writeByte, "AParcel_writeByte");
    LOAD(writeString, "AParcel_writeString");
    LOAD(writeInt32Array, "AParcel_writeInt32Array");
    LOAD(writeByteArray, "AParcel_writeByteArray");
    LOAD(writeParcelFileDescriptor, "AParcel_writeParcelFileDescriptor");
    LOAD(writeStrongBinder, "AParcel_writeStrongBinder");
    LOAD(writeParcelableArray, "AParcel_writeParcelableArray");
#undef LOAD
    return true;
}

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* =========================================================================
 * ALSA Mixer + PCM helpers (mainline Linux 6.18 Q6DSP + TFA9891 speaker)
 * ========================================================================= */

static int alsa_ctl_set(int ctl_fd, const char *name, long val, const char *enum_str) {
    struct snd_ctl_elem_list list;
    memset(&list, 0, sizeof(list));
    if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0 || list.count == 0)
        return -1;
    struct snd_ctl_elem_id *ids = calloc(list.count, sizeof(*ids));
    if (!ids) return -1;
    list.space = list.count;
    list.pids = ids;
    int rc = -1;
    if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0) {
        for (unsigned int i = 0; i < list.used; i++) {
            if (strcmp((const char *)ids[i].name, name) != 0)
                continue;
            struct snd_ctl_elem_info info;
            memset(&info, 0, sizeof(info));
            info.id = ids[i];
            if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0)
                break;
            struct snd_ctl_elem_value ev;
            memset(&ev, 0, sizeof(ev));
            ev.id = ids[i];
            if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_READ, &ev) < 0)
                break;
            if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
                unsigned int item = (unsigned int)val;
                if (enum_str) {
                    for (unsigned int k = 0; k < info.value.enumerated.items; k++) {
                        info.value.enumerated.item = k;
                        if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0 &&
                            strcmp(info.value.enumerated.name, enum_str) == 0) {
                            item = k;
                            break;
                        }
                    }
                }
                for (unsigned int c = 0; c < info.count; c++)
                    ev.value.enumerated.item[c] = item;
            } else {
                for (unsigned int c = 0; c < info.count; c++)
                    ev.value.integer.value[c] = val;
            }
            if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev) == 0)
                rc = 0;
            break;
        }
    }
    free(ids);
    return rc;
}

static void alsa_set_speaker_route(bool enable) {
    int ctl = open("/dev/snd/controlC0", O_RDWR);
    if (ctl < 0) return;
    if (enable)
        alsa_ctl_set(ctl, "Amp Input", 0, "Left");
    alsa_ctl_set(ctl, "QUAT_MI2S_RX Audio Mixer MultiMedia1", enable ? 1 : 0, NULL);
    close(ctl);
}

static void param_set_mask(struct snd_pcm_hw_params *p, int var, unsigned int bit) {
    struct snd_mask *m = &p->masks[var - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m->bits, 0, sizeof(m->bits));
    m->bits[bit / 32] |= (1U << (bit % 32));
}

static void param_set_min_max(struct snd_pcm_hw_params *p, int var, unsigned int min, unsigned int max) {
    struct snd_interval *i = &p->intervals[var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = min;
    i->max = max;
    i->openmin = 0;
    i->openmax = 0;
    i->integer = 1;
    i->empty = 0;
}

static void param_init(struct snd_pcm_hw_params *p) {
    memset(p, 0, sizeof(*p));
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_MASK; i <= SNDRV_PCM_HW_PARAM_LAST_MASK; i++)
        memset(p->masks[i - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits, 0xff, sizeof(p->masks[0].bits));
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; i++) {
        p->intervals[i - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = 0;
        p->intervals[i - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = ~0U;
    }
    p->rmask = ~0U;
    p->info = ~0U;
}

static int alsa_pcm_open_speaker(void) {
    alsa_set_speaker_route(true);
    int fd = open("/dev/snd/pcmC0D0p", O_RDWR);
    if (fd < 0) {
        log_msg("open /dev/snd/pcmC0D0p failed: %s", strerror(errno));
        return -1;
    }
    struct snd_pcm_hw_params hw;
    param_init(&hw);
    param_set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    param_set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    param_set_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    param_set_min_max(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2, 2);
    param_set_min_max(&hw, SNDRV_PCM_HW_PARAM_RATE, 48000, 48000);
    param_set_min_max(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 960, 960);
    param_set_min_max(&hw, SNDRV_PCM_HW_PARAM_PERIODS, 6, 6);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
        log_msg("HW_PARAMS failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = 960;
    sw.start_threshold = 960 * 2;
    sw.stop_threshold = 960 * 6;
    sw.boundary = 960 * 6;
    while (sw.boundary * 2 <= (1UL << 30)) sw.boundary *= 2;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        log_msg("SW_PARAMS failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        log_msg("PREPARE failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    log_msg("Opened ALSA speaker /dev/snd/pcmC0D0p (48kHz S16_LE 2ch 4x960)");
    return fd;
}

/* =========================================================================
 * Android FMQ (AidlMessageQueue) ring buffer + EventFlag futex helpers
 * ========================================================================= */

#define FMQ_FLAG_NOT_FULL  (1u << 0) /* 0x1: reader -> writer (verified in libaudiohal@aidl) */
#define FMQ_FLAG_NOT_EMPTY (1u << 1) /* 0x2: writer -> reader (verified in libaudiohal@aidl) */

typedef struct {
    int fd;
    uint8_t *base;
    size_t total_size;
    size_t ring_bytes;
    uint32_t quantum;
    _Atomic uint64_t *read_ptr;
    _Atomic uint64_t *write_ptr;
    _Atomic uint32_t *ef_word;
    uint8_t *data;
} fmq_t;

static bool fmq_create(fmq_t *q, uint32_t quantum, uint32_t count) {
    memset(q, 0, sizeof(*q));
    q->quantum = quantum;
    q->ring_bytes = (size_t)quantum * (size_t)count;
    size_t raw = 64 + q->ring_bytes;
    q->total_size = (raw + 4095) & ~4095ULL;

    q->fd = (int)syscall(SYS_memfd_create, "sailfish_fmq", MFD_CLOEXEC);
    if (q->fd < 0) {
        log_msg("memfd_create failed: %s", strerror(errno));
        return false;
    }
    if (ftruncate(q->fd, (off_t)q->total_size) < 0) {
        close(q->fd);
        q->fd = -1;
        return false;
    }
    q->base = mmap(NULL, q->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, q->fd, 0);
    if (q->base == MAP_FAILED) {
        close(q->fd);
        q->fd = -1;
        return false;
    }
    memset(q->base, 0, q->total_size);
    q->read_ptr  = (_Atomic uint64_t *)(q->base + 0);
    q->write_ptr = (_Atomic uint64_t *)(q->base + 8);
    q->ef_word   = (_Atomic uint32_t *)(q->base + 16);
    q->data      = q->base + 64;
    return true;
}

static void fmq_destroy(fmq_t *q) {
    if (q->base && q->base != MAP_FAILED)
        munmap(q->base, q->total_size);
    if (q->fd >= 0)
        close(q->fd);
    memset(q, 0, sizeof(*q));
    q->fd = -1;
}

static void fmq_ef_wake(fmq_t *q, uint32_t bits) {
    atomic_fetch_or_explicit(q->ef_word, bits, memory_order_release);
    syscall(SYS_futex, (uint32_t *)q->ef_word, FUTEX_WAKE_BITSET, INT_MAX, NULL, NULL, bits);
}

static void fmq_ef_wait(fmq_t *q, uint32_t bits, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    for (;;) {
        uint32_t cur = atomic_load_explicit(q->ef_word, memory_order_acquire);
        if (cur & bits) {
            atomic_fetch_and_explicit(q->ef_word, ~bits, memory_order_acq_rel);
            return;
        }
        long r = syscall(SYS_futex, (uint32_t *)q->ef_word, FUTEX_WAIT_BITSET, cur, &ts, NULL, bits);
        if (r < 0 && errno == ETIMEDOUT)
            return;
    }
}

static size_t fmq_avail_read(fmq_t *q) {
    uint64_t w = atomic_load_explicit(q->write_ptr, memory_order_acquire);
    uint64_t r = atomic_load_explicit(q->read_ptr, memory_order_relaxed);
    return (w >= r) ? (size_t)(w - r) : 0;
}

static size_t fmq_read_bytes(fmq_t *q, void *dst, size_t nbytes) {
    size_t avail = fmq_avail_read(q);
    if (nbytes > avail) nbytes = avail;
    if (nbytes == 0) return 0;
    uint64_t r = atomic_load_explicit(q->read_ptr, memory_order_relaxed);
    size_t off = (size_t)(r % q->ring_bytes);
    size_t first = q->ring_bytes - off;
    if (first > nbytes) first = nbytes;
    memcpy(dst, q->data + off, first);
    if (nbytes > first)
        memcpy((uint8_t *)dst + first, q->data, nbytes - first);
    atomic_store_explicit(q->read_ptr, r + nbytes, memory_order_release);
    fmq_ef_wake(q, FMQ_FLAG_NOT_FULL);
    return nbytes;
}

static bool fmq_write_bytes(fmq_t *q, const void *src, size_t nbytes) {
    uint64_t w = atomic_load_explicit(q->write_ptr, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(q->read_ptr, memory_order_acquire);
    if (q->ring_bytes - (size_t)(w - r) < nbytes)
        return false;
    size_t off = (size_t)(w % q->ring_bytes);
    size_t first = q->ring_bytes - off;
    if (first > nbytes) first = nbytes;
    memcpy(q->data + off, src, first);
    if (nbytes > first)
        memcpy(q->data, (const uint8_t *)src + first, nbytes - first);
    atomic_store_explicit(q->write_ptr, w + nbytes, memory_order_release);
    fmq_ef_wake(q, FMQ_FLAG_NOT_EMPTY);
    return true;
}

/* =========================================================================
 * AIDL Parcelable helpers
 * ========================================================================= */

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

static void write_str(AParcel *p, const char *s) {
    g_bn.writeString(p, s, (int32_t)strlen(s));
}

typedef struct {
    int32_t fd_index;
    int32_t offset;
    int64_t extent;
} grantor_t;

static int32_t write_grantor_elem(AParcel *p, const void *arr, size_t idx) {
    const grantor_t *g = &((const grantor_t *)arr)[idx];
    g_bn.writeInt32(p, 1); /* non-null */
    int32_t s = parcel_begin(p);
    g_bn.writeInt32(p, g->fd_index);
    g_bn.writeInt32(p, g->offset);
    g_bn.writeInt64(p, g->extent);
    parcel_end(p, s);
    return STATUS_OK;
}

static int32_t write_pfd_elem(AParcel *p, const void *arr, size_t idx) {
    int fd = ((const int *)arr)[idx];
    return g_bn.writeParcelFileDescriptor(p, fd);
}

static void write_mq_descriptor(AParcel *p, const fmq_t *q) {
    grantor_t grantors[4] = {
        { 0, 0,  8 },
        { 0, 8,  8 },
        { 0, 64, (int64_t)q->ring_bytes },
        { 0, 16, 4 },
    };
    int32_t s = parcel_begin(p);
    g_bn.writeParcelableArray(p, grantors, 4, write_grantor_elem);
    /* NativeHandle */
    g_bn.writeInt32(p, 1); /* non-null */
    int32_t nh = parcel_begin(p);
    g_bn.writeParcelableArray(p, &q->fd, 1, write_pfd_elem);
    g_bn.writeInt32Array(p, NULL, 0);
    parcel_end(p, nh);
    /* quantum, flags (kSynchronizedReadWrite = 1) */
    g_bn.writeInt32(p, (int32_t)q->quantum);
    g_bn.writeInt32(p, 1);
    parcel_end(p, s);
}

static void write_format_pcm16(AParcel *p) {
    int32_t s = parcel_begin(p);
    g_bn.writeByte(p, 1); /* AudioFormatType::PCM */
    g_bn.writeByte(p, 1); /* PcmType::INT_16_BIT */
    write_str(p, "");
    parcel_end(p, s);
}

static int32_t write_ch_layout_stereo_elem(AParcel *p, const void *arr, size_t idx) {
    (void)arr; (void)idx;
    g_bn.writeInt32(p, 1); /* non-null */
    g_bn.writeInt32(p, 3); /* AudioChannelLayout::layoutMask */
    g_bn.writeInt32(p, 3); /* LAYOUT_STEREO = FL|FR = 3 */
    return STATUS_OK;
}

static void write_ch_layout_stereo(AParcel *p) {
    g_bn.writeInt32(p, 3); /* layoutMask */
    g_bn.writeInt32(p, 3); /* LAYOUT_STEREO */
}

static int32_t g_bt_sample_rate = 44100;

static int32_t write_profile_elem(AParcel *p, const void *arr, size_t idx) {
    (void)idx;
    int32_t mode = arr ? *(const int32_t *)arr : 0;
    int32_t rates_spk[1] = { 48000 };
    int32_t rates_bt_all[2] = { 44100, 48000 };
    int32_t rates_bt_one[1] = { g_bt_sample_rate > 0 ? g_bt_sample_rate : 44100 };
    int32_t dummy = 0;
    g_bn.writeInt32(p, 1); /* non-null */
    int32_t s = parcel_begin(p);
    write_str(p, "");
    g_bn.writeInt32(p, 1);
    write_format_pcm16(p);
    g_bn.writeParcelableArray(p, &dummy, 1, write_ch_layout_stereo_elem);
    if (mode == 2)
        g_bn.writeInt32Array(p, rates_bt_one, 1);
    else if (mode == 1)
        g_bn.writeInt32Array(p, rates_bt_all, 2);
    else
        g_bn.writeInt32Array(p, rates_spk, 1);
    g_bn.writeInt32(p, 0); /* AudioEncapsulationType::NONE */
    parcel_end(p, s);
    return STATUS_OK;
}

static int32_t write_empty_elem(AParcel *p, const void *arr, size_t idx) {
    (void)p; (void)arr; (void)idx;
    return STATUS_OK;
}

static int8_t g_bt_ext_mac[6] = { 0 };
static bool g_bt_has_mac = false;

static void write_device_desc(AParcel *p, int32_t dev_type, const char *conn,
                              const char *addr, const int8_t *mac) {
    int32_t s = parcel_begin(p);
    /* AudioDeviceDescription */
    g_bn.writeInt32(p, 1);
    int32_t dd = parcel_begin(p);
    g_bn.writeInt32(p, dev_type);
    write_str(p, conn);
    parcel_end(p, dd);
    /* AudioDeviceAddress: tag=1 mac (byte[6]) or tag=0 id (string) */
    g_bn.writeInt32(p, 1);
    if (mac) {
        g_bn.writeInt32(p, 1); /* mac */
        g_bn.writeByteArray(p, mac, 6);
    } else {
        g_bn.writeInt32(p, 0); /* id */
        write_str(p, addr);
    }
    parcel_end(p, s);
}

static void write_port_ext_device(AParcel *p, int32_t dev_type, const char *conn,
                                  const char *addr, const int8_t *mac, int32_t flags) {
    g_bn.writeInt32(p, 1); /* AudioPortExt::device */
    g_bn.writeInt32(p, 1); /* non-null */
    int32_t s = parcel_begin(p);
    g_bn.writeInt32(p, 1);
    write_device_desc(p, dev_type, conn, addr, mac);
    g_bn.writeInt32(p, flags);
    g_bn.writeParcelableArray(p, NULL, 0, write_empty_elem); /* encodedFormats */
    g_bn.writeInt32(p, 0); /* encapsulationModes */
    g_bn.writeInt32(p, 0); /* encapsulationMetadataTypes */
    g_bn.writeInt32(p, 0); /* speakerLayout = null */
    parcel_end(p, s);
}

static void write_port_ext_speaker(AParcel *p) {
    write_port_ext_device(p, 140 /* OUT_SPEAKER */, "", "", NULL, 1 /* DEFAULT_DEVICE */);
}

static char g_bt_ext_addr[64] = "";

static void write_port_ext_bt_a2dp(AParcel *p, bool connected) {
    if (connected && g_bt_has_mac)
        write_port_ext_device(p, 133 /* OUT_DEVICE */, "bt-a2dp", "", g_bt_ext_mac, 0);
    else
        write_port_ext_device(p, 133 /* OUT_DEVICE */, "bt-a2dp",
                              connected ? g_bt_ext_addr : "", NULL, 0);
}

static void write_port_ext_mix(AParcel *p, int32_t handle) {
    g_bn.writeInt32(p, 2); /* AudioPortExt::mix */
    g_bn.writeInt32(p, 1); /* non-null */
    int32_t s = parcel_begin(p);
    g_bn.writeInt32(p, handle);
    /* AudioPortMixExtUseCase: unspecified (tag=0, bool=false) */
    g_bn.writeInt32(p, 1);
    g_bn.writeInt32(p, 0);
    g_bn.writeBool(p, false);
    g_bn.writeInt32(p, 1); /* maxOpenStreamCount */
    g_bn.writeInt32(p, 1); /* maxActiveStreamCount */
    g_bn.writeInt32(p, 0); /* recommendedMuteDurationMs */
    parcel_end(p, s);
}

static void write_audio_port(AParcel *p, int32_t port_id) {
    int32_t prof_mode = (port_id >= 13) ? 2 : (port_id >= 11 ? 1 : 0);
    const char *name = "primary output";
    if (port_id == 1) name = "Speaker";
    else if (port_id == 11) name = "BT A2DP Out";
    else if (port_id == 12) name = "a2dp output";
    else if (port_id >= 13) name = "BT A2DP Connected";

    int32_t s = parcel_begin(p);
    g_bn.writeInt32(p, port_id);
    write_str(p, name);
    g_bn.writeParcelableArray(p, &prof_mode, 1, write_profile_elem);
    /* AudioIoFlags: output (tag=1) */
    g_bn.writeInt32(p, 1); /* non-null */
    g_bn.writeInt32(p, 1); /* tag=output */
    g_bn.writeInt32(p, port_id == 2 ? (1 << 1) : 0); /* PRIMARY = bit 1 */
    g_bn.writeParcelableArray(p, NULL, 0, write_empty_elem); /* extraAudioDescriptors */
    g_bn.writeParcelableArray(p, NULL, 0, write_empty_elem); /* gains */
    /* AudioPortExt */
    g_bn.writeInt32(p, 1); /* non-null */
    if (port_id == 1)
        write_port_ext_speaker(p);
    else if (port_id == 11)
        write_port_ext_bt_a2dp(p, false);
    else if (port_id >= 13)
        write_port_ext_bt_a2dp(p, true);
    else
        write_port_ext_mix(p, 0);
    parcel_end(p, s);
}

static int32_t write_audio_port_elem(AParcel *p, const void *arr, size_t idx) {
    int32_t id = ((const int32_t *)arr)[idx];
    g_bn.writeInt32(p, 1); /* non-null */
    write_audio_port(p, id);
    return STATUS_OK;
}

typedef struct {
    int32_t id;
    int32_t port_id;
    int32_t sample_rate;
    int32_t mix_handle;
    bool in_use;
} port_cfg_t;

static void write_port_cfg(AParcel *p, const port_cfg_t *c) {
    int32_t s = parcel_begin(p);
    g_bn.writeInt32(p, c->id);
    g_bn.writeInt32(p, c->port_id);
    /* Int sampleRate */
    g_bn.writeInt32(p, 1);
    int32_t sr = parcel_begin(p);
    g_bn.writeInt32(p, c->sample_rate ? c->sample_rate : 48000);
    parcel_end(p, sr);
    /* AudioChannelLayout */
    g_bn.writeInt32(p, 1);
    write_ch_layout_stereo(p);
    /* AudioFormatDescription */
    g_bn.writeInt32(p, 1);
    write_format_pcm16(p);
    /* AudioGainConfig = null */
    g_bn.writeInt32(p, 0);
    /* AudioIoFlags */
    g_bn.writeInt32(p, 1);
    g_bn.writeInt32(p, 1); /* output */
    g_bn.writeInt32(p, c->port_id == 2 ? (1 << 1) : 0);
    /* AudioPortExt */
    g_bn.writeInt32(p, 1);
    if (c->port_id == 1)
        write_port_ext_speaker(p);
    else if (c->port_id == 11)
        write_port_ext_bt_a2dp(p, false);
    else if (c->port_id >= 13)
        write_port_ext_bt_a2dp(p, true);
    else
        write_port_ext_mix(p, c->mix_handle);
    parcel_end(p, s);
}

static int32_t write_port_cfg_elem(AParcel *p, const void *arr, size_t idx) {
    g_bn.writeInt32(p, 1); /* non-null */
    write_port_cfg(p, &((const port_cfg_t *)arr)[idx]);
    return STATUS_OK;
}

typedef struct {
    int32_t id;
    int32_t src_ids[4];
    int32_t src_cnt;
    int32_t dst_ids[4];
    int32_t dst_cnt;
    bool in_use;
} patch_t;

static void write_patch(AParcel *p, const patch_t *pt) {
    int32_t lat = 20;
    int32_t s = parcel_begin(p);
    g_bn.writeInt32(p, pt->id);
    g_bn.writeInt32Array(p, pt->src_ids, pt->src_cnt);
    g_bn.writeInt32Array(p, pt->dst_ids, pt->dst_cnt);
    g_bn.writeInt32(p, 960); /* minimumStreamBufferSizeFrames */
    g_bn.writeInt32Array(p, &lat, 1); /* latenciesMs[] */
    parcel_end(p, s);
}

static int32_t write_patch_elem(AParcel *p, const void *arr, size_t idx) {
    g_bn.writeInt32(p, 1); /* non-null */
    write_patch(p, &((const patch_t *)arr)[idx]);
    return STATUS_OK;
}

typedef struct {
    int32_t src_id;
    int32_t dst_id;
} route_def_t;

static int32_t write_route_elem(AParcel *p, const void *arr, size_t idx) {
    const route_def_t *rt = &((const route_def_t *)arr)[idx];
    int32_t src = rt->src_id;
    g_bn.writeInt32(p, 1); /* non-null */
    int32_t s = parcel_begin(p);
    g_bn.writeInt32Array(p, &src, 1);
    g_bn.writeInt32(p, rt->dst_id);
    g_bn.writeBool(p, false);
    parcel_end(p, s);
    return STATUS_OK;
}

/* =========================================================================
 * StreamDescriptor Command / Reply binary structs (verified against A17)
 * ========================================================================= */

enum {
    CMD_EXIT = 0,
    CMD_GET_STATUS = 1,
    CMD_START = 2,
    CMD_BURST = 3,
    CMD_DRAIN = 4,
    CMD_STANDBY = 5,
    CMD_PAUSE = 6,
    CMD_FLUSH = 7,
    CMD_FLUSH_FROM_FRAME = 8,
};

enum {
    STATE_STANDBY = 1,
    STATE_IDLE = 2,
    STATE_ACTIVE = 3,
    STATE_PAUSED = 4,
    STATE_DRAINING = 5,
    STATE_DRAIN_PAUSED = 6,
    STATE_TRANSFERRING = 7,
    STATE_TRANSFER_PAUSED = 8,
    STATE_ERROR = 9,
};

typedef struct {
    int8_t  tag;
    uint8_t pad[3];
    int32_t val;
} __attribute__((packed)) stream_cmd_t;

typedef struct {
    int64_t frames;
    int64_t time_ns;
} stream_pos_t;

typedef struct {
    int32_t      status;
    int32_t      fmq_byte_count;
    stream_pos_t observable;
    stream_pos_t hardware;
    int32_t      latency_ms;
    int32_t      xrun_frames;
    int32_t      state;
    int32_t      extra;
} stream_reply_t;

_Static_assert(sizeof(stream_cmd_t) == 8, "stream_cmd_t must be 8 bytes");
_Static_assert(sizeof(stream_reply_t) == 56, "stream_reply_t must be 56 bytes");

/* =========================================================================
 * Global Module & Stream state
 * ========================================================================= */

typedef struct stream_out {
    pthread_mutex_t lock;
    bool is_bt_a2dp;
    fmq_t cmd_mq;
    fmq_t reply_mq;
    fmq_t data_mq;
    pthread_t worker;
    bool worker_started;
    _Atomic bool stop;
    int pcm_fd;
    int32_t state;
    int64_t frames_written;
    float hw_vol[2];
    AIBinder *common_binder;
} stream_out_t;

static struct {
    pthread_mutex_t lock;
    AIBinder *host_if;
    fmq_t data_mq;
    bool session_active;
    bool stream_started;
    bool ext_connected;
    int32_t ext_port_id;
    int32_t next_port_id;
    int32_t sample_rate;
} g_bt = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .ext_port_id = 13,
    .next_port_id = 12,
    .sample_rate = 44100,
};

static void bt_port_call(transaction_code_t code, bool has_bool, bool arg_bool) {
    pthread_mutex_lock(&g_bt.lock);
    AIBinder *target = g_bt.host_if;
    if (target) g_bn.incStrong(target);
    pthread_mutex_unlock(&g_bt.lock);
    if (!target) return;

    g_bn.associateClass(target, g_cls_bt_port);
    AParcel *in = NULL, *out = NULL;
    binder_status_t r1 = g_bn.prepareTransaction(target, &in);
    binder_status_t r2 = -1;
    if (r1 == STATUS_OK) {
        if (has_bool) g_bn.writeBool(in, arg_bool);
        r2 = g_bn.transact(target, code, &in, &out, 0);
        if (out) g_bn.parcelDelete(out);
    }
    log_msg("BT A2DP: bt_port_call(tx=%u) prep=%d tx_rc=%d", code, r1, r2);
    g_bn.decStrong(target);
}

static void bt_ensure_started(void) {
    pthread_mutex_lock(&g_bt.lock);
    bool need = g_bt.session_active && !g_bt.stream_started;
    if (need) g_bt.stream_started = true;
    pthread_mutex_unlock(&g_bt.lock);
    if (need) {
        log_msg("BT A2DP: calling IBluetoothAudioPort::startStream(false)");
        bt_port_call(2 /* startStream */, true, false);
    }
}

static void bt_suspend_stream(void) {
    pthread_mutex_lock(&g_bt.lock);
    bool need = g_bt.session_active && g_bt.stream_started;
    if (need) g_bt.stream_started = false;
    pthread_mutex_unlock(&g_bt.lock);
    if (need) {
        log_msg("BT A2DP: calling IBluetoothAudioPort::suspendStream()");
        bt_port_call(4 /* suspendStream */, false, false);
    }
}

static void bt_write_pcm(const void *buf, size_t nbytes) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    int tries = 0;
    while (off < nbytes && tries < 60) {
        pthread_mutex_lock(&g_bt.lock);
        if (!g_bt.session_active) {
            pthread_mutex_unlock(&g_bt.lock);
            int sr = g_bt.sample_rate > 0 ? g_bt.sample_rate : 44100;
            usleep((useconds_t)((nbytes * 1000000ULL) / (unsigned long long)(sr * 4)));
            return;
        }
        uint64_t w = atomic_load_explicit(g_bt.data_mq.write_ptr, memory_order_relaxed);
        uint64_t r = atomic_load_explicit(g_bt.data_mq.read_ptr, memory_order_acquire);
        size_t space = (w >= r && g_bt.data_mq.ring_bytes > (size_t)(w - r))
                       ? (g_bt.data_mq.ring_bytes - (size_t)(w - r)) : 0;
        size_t chunk = nbytes - off;
        if (chunk > space) chunk = space;
        chunk &= ~3UL; /* keep 4-byte stereo frame alignment */
        if (chunk > 0 && fmq_write_bytes(&g_bt.data_mq, p + off, chunk)) {
            off += chunk;
            pthread_mutex_unlock(&g_bt.lock);
            continue;
        }
        pthread_mutex_unlock(&g_bt.lock);
        usleep(3000);
        tries++;
    }
}

static struct {
    pthread_mutex_t lock;
    float master_vol;
    bool master_mute;
    bool mic_mute;
    int32_t next_cfg_id;
    int32_t next_patch_id;
    port_cfg_t cfgs[32];
    patch_t patches[16];
} g_mod = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .master_vol = 1.0f,
    .master_mute = false,
    .mic_mute = false,
    .next_cfg_id = 100,
    .next_patch_id = 200,
};

static void *stream_worker(void *arg) {
    stream_out_t *st = (stream_out_t *)arg;
    int16_t pcm_buf[8192];

    struct sched_param sp = { .sched_priority = 10 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0)
        log_msg("Stream worker thread started (SCHED_FIFO prio 10)");
    else
        log_msg("Stream worker thread started (default sched)");
    while (!atomic_load(&st->stop)) {
        if (fmq_avail_read(&st->cmd_mq) < sizeof(stream_cmd_t)) {
            fmq_ef_wait(&st->cmd_mq, FMQ_FLAG_NOT_EMPTY, 50);
            continue;
        }
        stream_cmd_t cmd;
        if (fmq_read_bytes(&st->cmd_mq, &cmd, sizeof(cmd)) != sizeof(cmd))
            continue;

        stream_reply_t rep;
        memset(&rep, 0, sizeof(rep));
        rep.status = STATUS_OK;
        rep.latency_ms = 20;

        switch (cmd.tag) {
        case CMD_EXIT:
            atomic_store(&st->stop, true);
            break;
        case CMD_GET_STATUS:
            break;
        case CMD_START:
            if (st->is_bt_a2dp) {
                bt_ensure_started();
            } else if (st->pcm_fd < 0) {
                st->pcm_fd = alsa_pcm_open_speaker();
            }
            st->state = STATE_ACTIVE;
            break;
        case CMD_BURST: {
            int32_t req = cmd.val;
            if (req < 0) req = 0;
            if ((size_t)req > sizeof(pcm_buf)) req = (int32_t)sizeof(pcm_buf);
            size_t got = fmq_read_bytes(&st->data_mq, pcm_buf, (size_t)req);
            if (got > 0) {
                float g = g_mod.master_mute ? 0.0f : (g_mod.master_vol * st->hw_vol[0]);
                if (g < 0.0f) g = 0.0f;
                if (g > 1.0f) g = 1.0f;
                size_t samples = got / 2;
                if (g < 0.999f) {
                    for (size_t i = 0; i < samples; i++)
                        pcm_buf[i] = (int16_t)((float)pcm_buf[i] * g);
                }
                int frames = (int)(got / 4);
                if (st->is_bt_a2dp) {
                    bt_ensure_started();
                    bt_write_pcm(pcm_buf, got);
                } else {
                    if (st->pcm_fd < 0)
                        st->pcm_fd = alsa_pcm_open_speaker();
                    if (st->pcm_fd >= 0 && frames > 0) {
                        int off = 0;
                        while (off < frames) {
                            struct snd_xferi xfer;
                            memset(&xfer, 0, sizeof(xfer));
                            xfer.buf = pcm_buf + off * 2;
                            xfer.frames = (snd_pcm_uframes_t)(frames - off);
                            if (ioctl(st->pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer) < 0) {
                                if (errno == EPIPE) {
                                    ioctl(st->pcm_fd, SNDRV_PCM_IOCTL_PREPARE);
                                    continue;
                                }
                                break;
                            }
                            if (xfer.result <= 0) break;
                            off += (int)xfer.result;
                        }
                    }
                }
                st->frames_written += frames;
                rep.fmq_byte_count = (int32_t)got;
            }
            st->state = STATE_ACTIVE;
            break;
        }
        case CMD_DRAIN:
            if (!st->is_bt_a2dp && st->pcm_fd >= 0)
                ioctl(st->pcm_fd, SNDRV_PCM_IOCTL_DRAIN);
            st->state = STATE_IDLE;
            break;
        case CMD_STANDBY:
            if (st->is_bt_a2dp) {
                bt_suspend_stream();
            } else if (st->pcm_fd >= 0) {
                ioctl(st->pcm_fd, SNDRV_PCM_IOCTL_DROP);
                close(st->pcm_fd);
                st->pcm_fd = -1;
                alsa_set_speaker_route(false);
            }
            st->state = STATE_STANDBY;
            break;
        case CMD_PAUSE:
            if (st->is_bt_a2dp)
                bt_suspend_stream();
            st->state = STATE_PAUSED;
            break;
        case CMD_FLUSH:
        case CMD_FLUSH_FROM_FRAME:
            if (!st->is_bt_a2dp && st->pcm_fd >= 0) {
                ioctl(st->pcm_fd, SNDRV_PCM_IOCTL_DROP);
                ioctl(st->pcm_fd, SNDRV_PCM_IOCTL_PREPARE);
            }
            st->state = STATE_IDLE;
            break;
        default:
            break;
        }

        int64_t t = now_ns();
        rep.observable.frames = st->frames_written;
        rep.observable.time_ns = t;
        rep.hardware.frames = st->frames_written;
        rep.hardware.time_ns = t;
        rep.state = st->state;
        fmq_write_bytes(&st->reply_mq, &rep, sizeof(rep));
    }

    if (st->pcm_fd >= 0) {
        ioctl(st->pcm_fd, SNDRV_PCM_IOCTL_DROP);
        close(st->pcm_fd);
        st->pcm_fd = -1;
        alsa_set_speaker_route(false);
    }
    log_msg("Stream worker thread exiting");
    return NULL;
}

static void stream_close(stream_out_t *st) {
    if (!st) return;
    if (st->worker_started) {
        atomic_store(&st->stop, true);
        fmq_ef_wake(&st->cmd_mq, FMQ_FLAG_NOT_EMPTY);
        pthread_join(st->worker, NULL);
        st->worker_started = false;
    }
}

/* =========================================================================
 * AIDL Transaction Handlers: IStreamCommon, IStreamOut, IConfig, IModule
 * ========================================================================= */

static void *on_create_passthrough(void *args) { return args; }
static void on_destroy_noop(void *userData) { (void)userData; }

static void on_destroy_stream_out(void *userData) {
    stream_out_t *st = (stream_out_t *)userData;
    if (!st) return;
    stream_close(st);
    fmq_destroy(&st->cmd_mq);
    fmq_destroy(&st->reply_mq);
    fmq_destroy(&st->data_mq);
    if (st->common_binder)
        g_bn.decStrong(st->common_binder);
    free(st);
}

static binder_status_t handle_common_meta(transaction_code_t code, AParcel *out) {
    if (code == TX_GET_VERSION) {
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, AUDIO_HAL_VERSION);
        return STATUS_OK;
    }
    if (code == TX_GET_HASH) {
        g_bn.writeInt32(out, STATUS_OK);
        write_str(out, AUDIO_HAL_HASH);
        return STATUS_OK;
    }
    return -1;
}

static binder_status_t on_transact_stream_comm(AIBinder *b, transaction_code_t code,
                                               const AParcel *in, AParcel *out) {
    (void)in;
    if (handle_common_meta(code, out) == STATUS_OK) return STATUS_OK;
    stream_out_t *st = (stream_out_t *)g_bn.getUserData(b);
    switch (code) {
    case 1: /* close */
        stream_close(st);
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    case 2: /* prepareToClose */
    case 3: /* updateHwAvSyncId */
    case 5: /* setVendorParameters */
    case 6: /* addEffect */
    case 7: /* removeEffect */
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    case 4: /* getVendorParameters */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeParcelableArray(out, NULL, 0, write_empty_elem);
        return STATUS_OK;
    default:
        g_bn.writeInt32(out, EX_UNSUPPORTED_OPERATION);
        write_str(out, "unsupported");
        return STATUS_OK;
    }
}

static binder_status_t on_transact_stream_out(AIBinder *b, transaction_code_t code,
                                              const AParcel *in, AParcel *out) {
    if (handle_common_meta(code, out) == STATUS_OK) return STATUS_OK;
    stream_out_t *st = (stream_out_t *)g_bn.getUserData(b);
    switch (code) {
    case 1: /* getStreamCommon */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeStrongBinder(out, st->common_binder);
        return STATUS_OK;
    case 2: /* updateMetadata */
    case 3: /* updateOffloadMetadata */
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    case 4: /* getHwVolume */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, 2);
        g_bn.writeFloat(out, st->hw_vol[0]);
        g_bn.writeFloat(out, st->hw_vol[1]);
        return STATUS_OK;
    case 5: { /* setHwVolume(float[]) */
        int32_t len = 0;
        g_bn.readInt32(in, &len);
        for (int32_t i = 0; i < len && i < 2; i++)
            g_bn.readFloat(in, &st->hw_vol[i]);
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    }
    case 10: /* getRecommendedLatencyModes */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32Array(out, NULL, 0);
        return STATUS_OK;
    default:
        g_bn.writeInt32(out, EX_UNSUPPORTED_OPERATION);
        write_str(out, "unsupported");
        return STATUS_OK;
    }
}

typedef struct {
    int8_t  index;
    int32_t atten_mb;
} curve_pt_t;

typedef struct {
    const char *name;
    int32_t min_idx;
    int32_t max_idx;
} vol_grp_def_t;

static int32_t write_curve_pt_elem(AParcel *p, const void *arr, size_t idx) {
    const curve_pt_t *pt = &((const curve_pt_t *)arr)[idx];
    g_bn.writeInt32(p, 1); /* non-null */
    int32_t s = parcel_begin(p);
    g_bn.writeByte(p, pt->index);
    g_bn.writeInt32(p, pt->atten_mb);
    parcel_end(p, s);
    return STATUS_OK;
}

static int32_t write_vol_curve_elem(AParcel *p, const void *arr, size_t idx) {
    int8_t cat = ((const int8_t *)arr)[idx];
    static const curve_pt_t pts[] = {
        {   1, -5800 },
        {  20, -4000 },
        {  60, -1700 },
        { 100,     0 },
    };
    g_bn.writeInt32(p, 1); /* non-null */
    int32_t s = parcel_begin(p);
    g_bn.writeByte(p, cat);
    g_bn.writeParcelableArray(p, pts, 4, write_curve_pt_elem);
    parcel_end(p, s);
    return STATUS_OK;
}

static int32_t write_vol_grp_elem(AParcel *p, const void *arr, size_t idx) {
    const vol_grp_def_t *vg = &((const vol_grp_def_t *)arr)[idx];
    static const int8_t cats[] = { 0, 1, 2, 3, 4 };
    g_bn.writeInt32(p, 1); /* non-null */
    int32_t s = parcel_begin(p);
    write_str(p, vg->name);
    g_bn.writeInt32(p, vg->min_idx);
    g_bn.writeInt32(p, vg->max_idx);
    g_bn.writeParcelableArray(p, cats, 5, write_vol_curve_elem);
    parcel_end(p, s);
    return STATUS_OK;
}

static binder_status_t on_transact_config(AIBinder *b, transaction_code_t code,
                                          const AParcel *in, AParcel *out) {
    (void)b; (void)in;
    if (handle_common_meta(code, out) == STATUS_OK) return STATUS_OK;
    switch (code) {
    case 1: { /* getSurroundSoundConfig */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, 1); /* non-null */
        int32_t s = parcel_begin(out);
        g_bn.writeParcelableArray(out, NULL, 0, write_empty_elem);
        parcel_end(out, s);
        return STATUS_OK;
    }
    case 2: { /* getEngineConfig */
        static const vol_grp_def_t vgs[] = {
            { "AUDIO_STREAM_VOICE_CALL",       1,  7 },
            { "AUDIO_STREAM_SYSTEM",           0,  7 },
            { "AUDIO_STREAM_RING",             0,  7 },
            { "AUDIO_STREAM_MUSIC",            0, 15 },
            { "AUDIO_STREAM_ALARM",            1,  7 },
            { "AUDIO_STREAM_NOTIFICATION",     0,  7 },
            { "AUDIO_STREAM_ENFORCED_AUDIBLE", 0,  7 },
            { "AUDIO_STREAM_DTMF",             0, 15 },
            { "AUDIO_STREAM_TTS",              0, 15 },
            { "AUDIO_STREAM_ACCESSIBILITY",    1, 15 },
            { "AUDIO_STREAM_ASSISTANT",        0, 15 },
            { "AUDIO_STREAM_CALL_ASSISTANT",   0,  7 },
            { "AUDIO_STREAM_REROUTING",        0, 15 },
            { "AUDIO_STREAM_PATCH",            0, 15 },
        };
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, 1); /* non-null */
        int32_t s = parcel_begin(out);
        g_bn.writeInt32(out, -1); /* defaultProductStrategyId */
        g_bn.writeParcelableArray(out, NULL, 0, write_empty_elem); /* productStrategies */
        g_bn.writeParcelableArray(out, vgs, (int32_t)(sizeof(vgs) / sizeof(vgs[0])),
                                  write_vol_grp_elem);
        g_bn.writeInt32(out, 0); /* capSpecificConfig = null */
        parcel_end(out, s);
        return STATUS_OK;
    }
    default:
        g_bn.writeInt32(out, EX_UNSUPPORTED_OPERATION);
        write_str(out, "unsupported");
        return STATUS_OK;
    }
}

typedef struct {
    char buf[64];
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

static void skip_parcelable(const AParcel *in) {
    int32_t nn = 0, sz = 0;
    if (g_bn.readInt32(in, &nn) != STATUS_OK || !nn) return;
    int32_t pos = g_bn.getDataPosition(in);
    if (g_bn.readInt32(in, &sz) == STATUS_OK && sz >= 4)
        g_bn.setDataPosition(in, pos + sz);
}

static void skip_parcelable_array(const AParcel *in) {
    int32_t n = 0;
    if (g_bn.readInt32(in, &n) != STATUS_OK || n <= 0) return;
    for (int32_t i = 0; i < n; i++)
        skip_parcelable(in);
}

typedef struct {
    int8_t mac[6];
    bool ok;
} mac_buf_t;

static bool mac_buf_alloc(void *data, int32_t len, int8_t **out) {
    mac_buf_t *mb = (mac_buf_t *)data;
    mb->ok = false;
    if (len != 6) {
        *out = NULL;
        return false;
    }
    mb->ok = true;
    *out = mb->mac;
    return true;
}

static void extract_port_dev_addr(const AParcel *in, char *dst, size_t dst_sz,
                                  int8_t *mac_out, bool *has_mac) {
    dst[0] = '\0';
    *has_mac = false;
    int32_t nn = 0, psz = 0, pid = 0;
    if (g_bn.readInt32(in, &nn) != STATUS_OK || !nn) return;
    if (g_bn.readInt32(in, &psz) != STATUS_OK) return;
    g_bn.readInt32(in, &pid);
    str_buf_t tmp;
    g_bn.readString(in, &tmp, str_buf_alloc); /* name */
    skip_parcelable_array(in); /* profiles */
    /* AudioIoFlags (union: nonnull + tag + int32) */
    int32_t fnn = 0, ftag = 0, fval = 0;
    if (g_bn.readInt32(in, &fnn) == STATUS_OK && fnn) {
        g_bn.readInt32(in, &ftag);
        g_bn.readInt32(in, &fval);
    }
    skip_parcelable_array(in); /* extraAudioDescriptors */
    skip_parcelable_array(in); /* gains */
    /* AudioPortExt (union: nonnull + tag) */
    int32_t enn = 0, etag = 0;
    if (g_bn.readInt32(in, &enn) != STATUS_OK || !enn) return;
    if (g_bn.readInt32(in, &etag) != STATUS_OK || etag != 1) return;
    /* AudioPortDeviceExt */
    int32_t dnn = 0, dsz = 0;
    if (g_bn.readInt32(in, &dnn) != STATUS_OK || !dnn) return;
    g_bn.readInt32(in, &dsz);
    /* AudioDevice */
    int32_t adnn = 0, adsz = 0;
    if (g_bn.readInt32(in, &adnn) != STATUS_OK || !adnn) return;
    g_bn.readInt32(in, &adsz);
    skip_parcelable(in); /* AudioDeviceDescription */
    /* AudioDeviceAddress (union: nonnull + tag) */
    int32_t ann = 0, atag = 0;
    if (g_bn.readInt32(in, &ann) == STATUS_OK && ann &&
        g_bn.readInt32(in, &atag) == STATUS_OK) {
        if (atag == 0) {
            str_buf_t addr;
            addr.buf[0] = '\0';
            if (g_bn.readString(in, &addr, str_buf_alloc) == STATUS_OK) {
                strncpy(dst, addr.buf, dst_sz - 1);
                dst[dst_sz - 1] = '\0';
            }
        } else if (atag == 1) {
            mac_buf_t mb;
            memset(&mb, 0, sizeof(mb));
            if (g_bn.readByteArray(in, &mb, mac_buf_alloc) == STATUS_OK && mb.ok) {
                memcpy(mac_out, mb.mac, 6);
                *has_mac = true;
            }
        }
    }
}

static binder_status_t on_transact_module(AIBinder *b, transaction_code_t code,
                                          const AParcel *in, AParcel *out) {
    bool is_a2dp = (g_bn.getUserData(b) != NULL);
    if (handle_common_meta(code, out) == STATUS_OK) return STATUS_OK;

    switch (code) {
    case 1:  /* setModuleDebug */
    case 28: /* updateAudioMode */
    case 29: /* updateScreenRotation */
    case 30: /* updateScreenState */
    case 34: /* setVendorParameters */
    case 35: /* addDeviceEffect */
    case 36: /* removeDeviceEffect */
    case 41: /* prepareToDisconnectExternalDevice */
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;

    case 2:  /* getTelephony */
    case 3:  /* getBluetooth */
    case 4:  /* getBluetoothA2dp */
    case 5:  /* getBluetoothLe */
    case 31: /* getSoundDose */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeStrongBinder(out, NULL);
        return STATUS_OK;

    case 6: { /* connectExternalDevice(in AudioPort) -> AudioPort */
        if (!is_a2dp) {
            g_bn.writeInt32(out, EX_UNSUPPORTED_OPERATION);
            write_str(out, "not a2dp");
            return STATUS_OK;
        }
        char addr[64];
        int8_t mac[6] = { 0 };
        bool has_mac = false;
        extract_port_dev_addr(in, addr, sizeof(addr), mac, &has_mac);
        pthread_mutex_lock(&g_bt.lock);
        strncpy(g_bt_ext_addr, addr, sizeof(g_bt_ext_addr) - 1);
        g_bt_ext_addr[sizeof(g_bt_ext_addr) - 1] = '\0';
        memcpy(g_bt_ext_mac, mac, 6);
        g_bt_has_mac = has_mac;
        g_bt.ext_port_id = ++g_bt.next_port_id;
        int32_t new_pid = g_bt.ext_port_id;
        g_bt.ext_connected = true;
        pthread_mutex_unlock(&g_bt.lock);
        log_msg("IModule/a2dp: connectExternalDevice has_mac=%d (%02x:%02x:%02x:%02x:%02x:%02x) -> port %d",
                has_mac ? 1 : 0, (uint8_t)mac[0], (uint8_t)mac[1], (uint8_t)mac[2],
                (uint8_t)mac[3], (uint8_t)mac[4], (uint8_t)mac[5], new_pid);
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, 1);
        write_audio_port(out, new_pid);
        return STATUS_OK;
    }

    case 7: { /* disconnectExternalDevice(portId) */
        if (is_a2dp) {
            pthread_mutex_lock(&g_bt.lock);
            g_bt.ext_connected = false;
            pthread_mutex_unlock(&g_bt.lock);
            log_msg("IModule/a2dp: disconnectExternalDevice");
        }
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    }

    case 8: { /* getAudioPatches */
        patch_t active[16];
        int32_t n = 0;
        pthread_mutex_lock(&g_mod.lock);
        for (int i = 0; i < 16; i++)
            if (g_mod.patches[i].in_use) active[n++] = g_mod.patches[i];
        pthread_mutex_unlock(&g_mod.lock);
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeParcelableArray(out, active, n, write_patch_elem);
        return STATUS_OK;
    }

    case 9: { /* getAudioPort(portId) */
        int32_t pid = 0;
        g_bn.readInt32(in, &pid);
        bool valid = is_a2dp ? (pid == 11 || pid == 12 || pid == g_bt.ext_port_id) : (pid == 1 || pid == 2);
        if (!valid) {
            g_bn.writeInt32(out, EX_ILLEGAL_ARGUMENT);
            write_str(out, "unknown port");
            return STATUS_OK;
        }
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, 1);
        write_audio_port(out, pid);
        return STATUS_OK;
    }

    case 10: { /* getAudioPortConfigs */
        port_cfg_t active[32];
        int32_t n = 0;
        pthread_mutex_lock(&g_mod.lock);
        for (int i = 0; i < 32; i++) {
            if (!g_mod.cfgs[i].in_use) continue;
            bool cfg_bt = (g_mod.cfgs[i].port_id >= 11);
            if (cfg_bt == is_a2dp) active[n++] = g_mod.cfgs[i];
        }
        pthread_mutex_unlock(&g_mod.lock);
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeParcelableArray(out, active, n, write_port_cfg_elem);
        return STATUS_OK;
    }

    case 11: { /* getAudioPorts */
        if (is_a2dp) {
            int32_t ids[3] = { 11, 12, g_bt.ext_port_id };
            int32_t n = g_bt.ext_connected ? 3 : 2;
            g_bn.writeInt32(out, STATUS_OK);
            g_bn.writeParcelableArray(out, ids, n, write_audio_port_elem);
        } else {
            int32_t ids[2] = { 1, 2 };
            g_bn.writeInt32(out, STATUS_OK);
            g_bn.writeParcelableArray(out, ids, 2, write_audio_port_elem);
        }
        return STATUS_OK;
    }

    case 12: /* getAudioRoutes */
    case 13: { /* getAudioRoutesForAudioPort */
        if (is_a2dp) {
            route_def_t rts[2] = { { 12, 11 }, { 12, g_bt.ext_port_id } };
            int32_t n = g_bt.ext_connected ? 2 : 1;
            g_bn.writeInt32(out, STATUS_OK);
            g_bn.writeParcelableArray(out, rts, n, write_route_elem);
        } else {
            route_def_t rt = { 2, 1 };
            g_bn.writeInt32(out, STATUS_OK);
            g_bn.writeParcelableArray(out, &rt, 1, write_route_elem);
        }
        return STATUS_OK;
    }

    case 15: { /* openOutputStream */
        stream_out_t *st = calloc(1, sizeof(*st));
        if (!st) {
            g_bn.writeInt32(out, EX_ILLEGAL_ARGUMENT);
            write_str(out, "oom");
            return STATUS_OK;
        }
        pthread_mutex_init(&st->lock, NULL);
        st->is_bt_a2dp = is_a2dp;
        st->pcm_fd = -1;
        st->state = STATE_STANDBY;
        st->hw_vol[0] = 1.0f;
        st->hw_vol[1] = 1.0f;
        if (!fmq_create(&st->cmd_mq, sizeof(stream_cmd_t), 16) ||
            !fmq_create(&st->reply_mq, sizeof(stream_reply_t), 16) ||
            !fmq_create(&st->data_mq, 1, 16384)) {
            on_destroy_stream_out(st);
            g_bn.writeInt32(out, EX_ILLEGAL_ARGUMENT);
            write_str(out, "fmq alloc failed");
            return STATUS_OK;
        }
        st->common_binder = g_bn.newBinder(g_cls_stream_comm, st);
        AIBinder *out_binder = g_bn.newBinder(g_cls_stream_out, st);
        if (g_bn.markVintfStability) {
            g_bn.markVintfStability(st->common_binder);
            g_bn.markVintfStability(out_binder);
        }
        if (pthread_create(&st->worker, NULL, stream_worker, st) == 0)
            st->worker_started = true;

        log_msg("openOutputStream: created stream & FMQs (cmd=%d rep=%d data=%d)",
                st->cmd_mq.fd, st->reply_mq.fd, st->data_mq.fd);

        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, 1); /* OpenOutputStreamReturn non-null */
        int32_t ret_s = parcel_begin(out);
        g_bn.writeStrongBinder(out, out_binder);
        /* StreamDescriptor */
        g_bn.writeInt32(out, 1);
        int32_t sd = parcel_begin(out);
        g_bn.writeInt32(out, 1);
        write_mq_descriptor(out, &st->cmd_mq);
        g_bn.writeInt32(out, 1);
        write_mq_descriptor(out, &st->reply_mq);
        g_bn.writeInt32(out, 4);    /* frameSizeBytes = 2ch * 2B */
        g_bn.writeInt64(out, 3840); /* bufferSizeFrames = 4 * 960 */
        /* StreamDescriptor::AudioBuffer (union tag=0 fmq) */
        g_bn.writeInt32(out, 1);
        g_bn.writeInt32(out, 0); /* tag=fmq */
        g_bn.writeInt32(out, 1);
        write_mq_descriptor(out, &st->data_mq);
        parcel_end(out, sd);
        parcel_end(out, ret_s);
        g_bn.decStrong(out_binder);
        return STATUS_OK;
    }

    case 17: { /* setAudioPatch(in AudioPatch) */
        int32_t nonnull = 0, psz = 0, id = 0;
        g_bn.readInt32(in, &nonnull);
        int32_t pstart = g_bn.getDataPosition(in);
        g_bn.readInt32(in, &psz);
        g_bn.readInt32(in, &id);
        patch_t pt;
        memset(&pt, 0, sizeof(pt));
        pt.id = id;
        int32_t nsrc = 0, ndst = 0;
        g_bn.readInt32(in, &nsrc);
        for (int32_t i = 0; i < nsrc; i++) {
            int32_t v = 0; g_bn.readInt32(in, &v);
            if (i < 4) pt.src_ids[pt.src_cnt++] = v;
        }
        g_bn.readInt32(in, &ndst);
        for (int32_t i = 0; i < ndst; i++) {
            int32_t v = 0; g_bn.readInt32(in, &v);
            if (i < 4) pt.dst_ids[pt.dst_cnt++] = v;
        }
        g_bn.setDataPosition(in, pstart + psz);

        pthread_mutex_lock(&g_mod.lock);
        if (pt.id == 0) pt.id = ++g_mod.next_patch_id;
        pt.in_use = true;
        int slot = -1;
        for (int i = 0; i < 16; i++) {
            if (g_mod.patches[i].in_use && g_mod.patches[i].id == pt.id) { slot = i; break; }
            if (!g_mod.patches[i].in_use && slot < 0) slot = i;
        }
        if (slot >= 0) g_mod.patches[slot] = pt;
        pthread_mutex_unlock(&g_mod.lock);

        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, 1);
        write_patch(out, &pt);
        return STATUS_OK;
    }

    case 18: { /* setAudioPortConfig(in AudioPortConfig, out AudioPortConfig suggested) -> bool */
        int32_t nonnull = 0, psz = 0, id = 0, port_id = 0;
        g_bn.readInt32(in, &nonnull);
        int32_t pstart = g_bn.getDataPosition(in);
        g_bn.readInt32(in, &psz);
        g_bn.readInt32(in, &id);
        g_bn.readInt32(in, &port_id);
        g_bn.setDataPosition(in, pstart + psz);

        if (port_id != 1 && port_id != 2) port_id = 1;
        port_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        pthread_mutex_lock(&g_mod.lock);
        cfg.id = (id > 0) ? id : ++g_mod.next_cfg_id;
        cfg.port_id = port_id;
        cfg.sample_rate = 48000;
        cfg.in_use = true;
        int slot = -1;
        for (int i = 0; i < 32; i++) {
            if (g_mod.cfgs[i].in_use && g_mod.cfgs[i].id == cfg.id) { slot = i; break; }
            if (!g_mod.cfgs[i].in_use && slot < 0) slot = i;
        }
        if (slot >= 0) g_mod.cfgs[slot] = cfg;
        pthread_mutex_unlock(&g_mod.lock);

        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeBool(out, true); /* applied */
        g_bn.writeInt32(out, 1);   /* out_suggested non-null */
        write_port_cfg(out, &cfg);
        return STATUS_OK;
    }

    case 19: { /* resetAudioPatch(id) */
        int32_t id = 0;
        g_bn.readInt32(in, &id);
        pthread_mutex_lock(&g_mod.lock);
        for (int i = 0; i < 16; i++)
            if (g_mod.patches[i].in_use && g_mod.patches[i].id == id)
                g_mod.patches[i].in_use = false;
        pthread_mutex_unlock(&g_mod.lock);
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    }

    case 20: { /* resetAudioPortConfig(id) */
        int32_t id = 0;
        g_bn.readInt32(in, &id);
        pthread_mutex_lock(&g_mod.lock);
        for (int i = 0; i < 32; i++)
            if (g_mod.cfgs[i].in_use && g_mod.cfgs[i].id == id)
                g_mod.cfgs[i].in_use = false;
        pthread_mutex_unlock(&g_mod.lock);
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    }

    case 21: /* getMasterMute */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeBool(out, g_mod.master_mute);
        return STATUS_OK;
    case 22: /* setMasterMute */
        g_bn.readBool(in, &g_mod.master_mute);
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    case 23: /* getMasterVolume */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeFloat(out, g_mod.master_vol);
        return STATUS_OK;
    case 24: /* setMasterVolume */
        g_bn.readFloat(in, &g_mod.master_vol);
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    case 25: /* getMicMute */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeBool(out, g_mod.mic_mute);
        return STATUS_OK;
    case 26: /* setMicMute */
        g_bn.readBool(in, &g_mod.mic_mute);
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;

    case 27: /* getMicrophones */
    case 33: /* getVendorParameters */
    case 37: /* getMmapPolicyInfos */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeParcelableArray(out, NULL, 0, write_empty_elem);
        return STATUS_OK;

    case 38: /* supportsVariableLatency */
    case 42: /* getFlushFromFrameSupport */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeBool(out, false);
        return STATUS_OK;

    case 39: /* getAAudioMixerBurstCount */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, 2);
        return STATUS_OK;

    case 40: /* getAAudioHardwareBurstMinUsec */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, 20000);
        return STATUS_OK;

    default:
        log_msg("IModule unhandled tx=%u", code);
        g_bn.writeInt32(out, EX_UNSUPPORTED_OPERATION);
        write_str(out, "unsupported");
        return STATUS_OK;
    }
}

static binder_status_t on_transact_factory(AIBinder *b, transaction_code_t code,
                                           const AParcel *in, AParcel *out) {
    (void)b; (void)in;
    if (code == TX_GET_VERSION) {
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, AUDIO_HAL_VERSION);
        return STATUS_OK;
    }
    if (code == TX_GET_HASH) {
        g_bn.writeInt32(out, STATUS_OK);
        write_str(out, "notfrozen");
        return STATUS_OK;
    }
    switch (code) {
    case 1: /* queryEffects */
    case 2: /* queryProcessing */
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeParcelableArray(out, NULL, 0, write_empty_elem);
        return STATUS_OK;
    case 3: /* createEffect */
        g_bn.writeInt32(out, EX_UNSUPPORTED_OPERATION);
        write_str(out, "no effects");
        return STATUS_OK;
    case 4: /* destroyEffect */
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    default:
        log_msg("IFactory unhandled tx=%u", code);
        g_bn.writeInt32(out, EX_UNSUPPORTED_OPERATION);
        write_str(out, "unsupported");
        return STATUS_OK;
    }
}

static int32_t write_bt_pcm_cap_elem(AParcel *p, const void *arr, size_t idx) {
    (void)arr; (void)idx;
    static const int32_t rates[] = { 44100, 48000 };
    static const int8_t chs[] = { 2 /* STEREO */ };
    static const int8_t bits[] = { 16 };
    static const int32_t intervals[] = { 20000 };
    g_bn.writeInt32(p, 1); /* non-null AudioCapabilities */
    g_bn.writeInt32(p, 0); /* union tag = pcmCapabilities */
    g_bn.writeInt32(p, 1); /* non-null PcmCapabilities */
    int32_t s = parcel_begin(p);
    g_bn.writeInt32Array(p, rates, 2);
    g_bn.writeByteArray(p, chs, 1);
    g_bn.writeByteArray(p, bits, 1);
    g_bn.writeInt32Array(p, intervals, 1);
    parcel_end(p, s);
    return STATUS_OK;
}

static binder_status_t on_transact_bt_provider(AIBinder *b, transaction_code_t code,
                                               const AParcel *in, AParcel *out) {
    (void)b;
    if (code == TX_GET_VERSION) {
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, BT_HAL_VERSION);
        return STATUS_OK;
    }
    if (code == TX_GET_HASH) {
        g_bn.writeInt32(out, STATUS_OK);
        write_str(out, "notfrozen");
        return STATUS_OK;
    }
    switch (code) {
    case 1: { /* endSession */
        pthread_mutex_lock(&g_bt.lock);
        if (g_bt.host_if) {
            g_bn.decStrong(g_bt.host_if);
            g_bt.host_if = NULL;
        }
        if (g_bt.session_active) {
            fmq_destroy(&g_bt.data_mq);
            g_bt.session_active = false;
        }
        g_bt.stream_started = false;
        pthread_mutex_unlock(&g_bt.lock);
        log_msg("BT Provider: endSession");
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    }
    case 2: { /* startSession(hostIf, audioConfig, latencyModes) -> MQDescriptor */
        AIBinder *host_if = NULL;
        g_bn.readStrongBinder(in, &host_if);
        int32_t nonnull = 0, tag = 0, sr = 44100;
        g_bn.readInt32(in, &nonnull);
        if (nonnull) {
            g_bn.readInt32(in, &tag);
            if (tag == 0) { /* pcmConfig */
                int32_t pnn = 0, psz = 0;
                g_bn.readInt32(in, &pnn);
                if (pnn) {
                    g_bn.readInt32(in, &psz);
                    g_bn.readInt32(in, &sr);
                }
            }
        }
        pthread_mutex_lock(&g_bt.lock);
        if (g_bt.host_if) g_bn.decStrong(g_bt.host_if);
        g_bt.host_if = host_if;
        if (g_bt.session_active) fmq_destroy(&g_bt.data_mq);
        g_bt.sample_rate = (sr > 0) ? sr : 44100;
        g_bt_sample_rate = g_bt.sample_rate;
        g_bt.session_active = fmq_create(&g_bt.data_mq, 1, 65536);
        g_bt.stream_started = false;
        bool ok = g_bt.session_active;
        pthread_mutex_unlock(&g_bt.lock);

        log_msg("BT Provider: startSession sr=%d fmq_ok=%d", g_bt.sample_rate, ok ? 1 : 0);
        if (!ok) {
            g_bn.writeInt32(out, EX_ILLEGAL_ARGUMENT);
            write_str(out, "fmq failed");
            return STATUS_OK;
        }
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, 1); /* non-null MQDescriptor */
        write_mq_descriptor(out, &g_bt.data_mq);
        return STATUS_OK;
    }
    case 3: { /* streamStarted(status) */
        int32_t st = 0;
        g_bn.readInt32(in, &st);
        pthread_mutex_lock(&g_bt.lock);
        g_bt.stream_started = (st == 1);
        pthread_mutex_unlock(&g_bt.lock);
        log_msg("BT Provider: streamStarted status=%d", st);
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    }
    case 4: { /* streamSuspended(status) */
        int32_t st = 0;
        g_bn.readInt32(in, &st);
        pthread_mutex_lock(&g_bt.lock);
        g_bt.stream_started = false;
        pthread_mutex_unlock(&g_bt.lock);
        log_msg("BT Provider: streamSuspended status=%d", st);
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    }
    case 5: /* updateAudioConfiguration */
    case 6: /* setLowLatencyModeAllowed */
        g_bn.writeInt32(out, STATUS_OK);
        return STATUS_OK;
    default:
        log_msg("BT Provider unhandled tx=%u", code);
        g_bn.writeInt32(out, EX_UNSUPPORTED_OPERATION);
        write_str(out, "unsupported");
        return STATUS_OK;
    }
}

static binder_status_t on_transact_bt_factory(AIBinder *b, transaction_code_t code,
                                              const AParcel *in, AParcel *out) {
    (void)b;
    if (code == TX_GET_VERSION) {
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeInt32(out, BT_HAL_VERSION);
        return STATUS_OK;
    }
    if (code == TX_GET_HASH) {
        g_bn.writeInt32(out, STATUS_OK);
        write_str(out, "notfrozen");
        return STATUS_OK;
    }
    switch (code) {
    case 1: { /* getProviderCapabilities(int8 sessionType) */
        int8_t stype = 0;
        g_bn.readByte(in, &stype);
        g_bn.writeInt32(out, STATUS_OK);
        if (stype == 1 /* A2DP_SOFTWARE_ENCODING_DATAPATH */) {
            int32_t dummy = 0;
            g_bn.writeParcelableArray(out, &dummy, 1, write_bt_pcm_cap_elem);
        } else {
            g_bn.writeParcelableArray(out, NULL, 0, write_empty_elem);
        }
        return STATUS_OK;
    }
    case 2: { /* openProvider(int8 sessionType) */
        int8_t stype = 0;
        g_bn.readByte(in, &stype);
        log_msg("BT Factory: openProvider sessionType=%d", (int)stype);
        if (stype != 1) {
            g_bn.writeInt32(out, EX_UNSUPPORTED_OPERATION);
            write_str(out, "only a2dp sw encoding");
            return STATUS_OK;
        }
        AIBinder *prov = g_bn.newBinder(g_cls_bt_provider, NULL);
        if (g_bn.markVintfStability)
            g_bn.markVintfStability(prov);
        g_bn.writeInt32(out, STATUS_OK);
        g_bn.writeStrongBinder(out, prov);
        g_bn.decStrong(prov);
        return STATUS_OK;
    }
    default:
        log_msg("BT Factory unhandled tx=%u", code);
        g_bn.writeInt32(out, EX_UNSUPPORTED_OPERATION);
        write_str(out, "unsupported");
        return STATUS_OK;
    }
}

static binder_status_t on_transact_noop(AIBinder *b, transaction_code_t code,
                                        const AParcel *in, AParcel *out) {
    (void)b; (void)code; (void)in; (void)out;
    return STATUS_OK;
}

int main(void) {
    log_msg("Starting Sailfish Native Android 17 AIDL Audio HAL");
    if (!load_binder_ndk())
        return 1;

    /* Pre-populate default speaker & primary output port configs */
    g_mod.cfgs[0] = (port_cfg_t){ .id = 101, .port_id = 1, .sample_rate = 48000, .in_use = true };
    g_mod.cfgs[1] = (port_cfg_t){ .id = 102, .port_id = 2, .sample_rate = 48000, .in_use = true };
    g_mod.next_cfg_id = 102;

    g_cls_config      = g_bn.Class_define(DESC_ICONFIG, on_create_passthrough, on_destroy_noop, on_transact_config);
    g_cls_module      = g_bn.Class_define(DESC_IMODULE, on_create_passthrough, on_destroy_noop, on_transact_module);
    g_cls_stream_out  = g_bn.Class_define(DESC_ISTREAM_OUT, on_create_passthrough, on_destroy_stream_out, on_transact_stream_out);
    g_cls_stream_comm = g_bn.Class_define(DESC_ISTREAM_COMM, on_create_passthrough, on_destroy_noop, on_transact_stream_comm);
    g_cls_factory     = g_bn.Class_define(DESC_IFACTORY, on_create_passthrough, on_destroy_noop, on_transact_factory);
    g_cls_bt_factory  = g_bn.Class_define(DESC_BT_FACTORY, on_create_passthrough, on_destroy_noop, on_transact_bt_factory);
    g_cls_bt_provider = g_bn.Class_define(DESC_BT_PROVIDER, on_create_passthrough, on_destroy_noop, on_transact_bt_provider);
    g_cls_bt_port     = g_bn.Class_define(DESC_BT_PORT, on_create_passthrough, on_destroy_noop, on_transact_noop);

    AIBinder *cfg_b  = g_bn.newBinder(g_cls_config, NULL);
    AIBinder *mod_b  = g_bn.newBinder(g_cls_module, NULL);
    AIBinder *a2dp_b = g_bn.newBinder(g_cls_module, (void *)1);
    AIBinder *fx_b   = g_bn.newBinder(g_cls_factory, NULL);
    AIBinder *btf_b  = g_bn.newBinder(g_cls_bt_factory, NULL);
    if (g_bn.markVintfStability) {
        g_bn.markVintfStability(cfg_b);
        g_bn.markVintfStability(mod_b);
        g_bn.markVintfStability(a2dp_b);
        g_bn.markVintfStability(fx_b);
        g_bn.markVintfStability(btf_b);
    }

    g_bn.startThreadPool();
    binder_status_t r1 = g_bn.addService(cfg_b,  DESC_ICONFIG "/default");
    binder_status_t r2 = g_bn.addService(mod_b,  DESC_IMODULE "/default");
    binder_status_t r3 = g_bn.addService(a2dp_b, DESC_IMODULE "/a2dp");
    binder_status_t r4 = g_bn.addService(fx_b,   DESC_IFACTORY "/default");
    binder_status_t r5 = g_bn.addService(btf_b,  DESC_BT_FACTORY "/default");
    log_msg("Registered IConfig=%d IModule=%d IModule/a2dp=%d IFactory=%d BTFactory=%d",
            r1, r2, r3, r4, r5);

    g_bn.joinThreadPool();
    return 0;
}
