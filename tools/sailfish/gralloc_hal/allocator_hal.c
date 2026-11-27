#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "sailfish_gralloc.h"

typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;

typedef void *(*AIBinder_Class_onCreate)(void *args);
typedef void (*AIBinder_Class_onDestroy)(void *userData);
typedef int32_t (*AIBinder_Class_onTransact)(AIBinder *binder, uint32_t code,
                                             const AParcel *in, AParcel *out);
typedef bool (*AParcel_byteArrayAllocator)(void *arrayData, int32_t length, int8_t **outBuffer);

enum {
    TX_ALLOCATE                    = 1,
    TX_ALLOCATE2                   = 2,
    TX_IS_SUPPORTED                = 3,
    TX_GET_IMAPPER_LIBRARY_SUFFIX  = 4,
    TX_GET_INTERFACE_HASH          = 16777214,
    TX_GET_INTERFACE_VERSION       = 16777215,
};

enum {
    STATUS_OK_VAL          = 0,
    STATUS_UNKNOWN_TX_VAL  = -EBADMSG,
    EX_SERVICE_SPECIFIC    = -8,
    ALLOC_ERR_BAD_DESC     = 1,
    ALLOC_ERR_NO_RESOURCES = 5,
    ALLOC_ERR_UNSUPPORTED  = 7,
};

static const char IALLOC_DESC[]     = "android.hardware.graphics.allocator.IAllocator";
static const char IALLOC_INSTANCE[] = "android.hardware.graphics.allocator.IAllocator/default";
static const char IALLOC_HASH[]     = "9499fec09c544e9de5be3c87125721600f8ade66";
static const char MAPPER_SUFFIX[]   = "sailfish";

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
    AStatus *(*AStatus_fromServiceSpecificError)(int32_t);
    void (*AStatus_delete)(AStatus *);
    int32_t (*writeStatusHeader)(AParcel *, const AStatus *);
    int32_t (*readInt32)(const AParcel *, int32_t *);
    int32_t (*readInt64)(const AParcel *, int64_t *);
    int32_t (*readByteArray)(const AParcel *, void *, AParcel_byteArrayAllocator);
    int32_t (*writeBool)(AParcel *, bool);
    int32_t (*writeInt32)(AParcel *, int32_t);
    int32_t (*writeInt32Array)(AParcel *, const int32_t *, int32_t);
    int32_t (*writeString)(AParcel *, const char *, int32_t);
    int32_t (*writeParcelFileDescriptor)(AParcel *, int);
    int32_t (*getDataPosition)(const AParcel *);
    int32_t (*setDataPosition)(const AParcel *, int32_t);
} g_ndk;

static int g_heap_fd = -1;
static uint64_t g_next_buf_id = 1;

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
        { "AServiceManager_addService", (void **)&g_ndk.addService },
        { "ABinderProcess_setThreadPoolMaxThreadCount", (void **)&g_ndk.setThreadPoolMaxThreadCount },
        { "ABinderProcess_startThreadPool", (void **)&g_ndk.startThreadPool },
        { "ABinderProcess_joinThreadPool", (void **)&g_ndk.joinThreadPool },
        { "AStatus_newOk", (void **)&g_ndk.AStatus_newOk },
        { "AStatus_fromServiceSpecificError", (void **)&g_ndk.AStatus_fromServiceSpecificError },
        { "AStatus_delete", (void **)&g_ndk.AStatus_delete },
        { "AParcel_writeStatusHeader", (void **)&g_ndk.writeStatusHeader },
        { "AParcel_readInt32", (void **)&g_ndk.readInt32 },
        { "AParcel_readInt64", (void **)&g_ndk.readInt64 },
        { "AParcel_readByteArray", (void **)&g_ndk.readByteArray },
        { "AParcel_writeBool", (void **)&g_ndk.writeBool },
        { "AParcel_writeInt32", (void **)&g_ndk.writeInt32 },
        { "AParcel_writeInt32Array", (void **)&g_ndk.writeInt32Array },
        { "AParcel_writeString", (void **)&g_ndk.writeString },
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

typedef struct {
    int8_t *buf;
    int32_t cap;
    int32_t len;
} fixed_byte_buf_t;

static bool fixed_byte_alloc(void *arrayData, int32_t length, int8_t **outBuffer) {
    fixed_byte_buf_t *b = (fixed_byte_buf_t *)arrayData;
    if (length < 0) {
        b->len = 0;
        *outBuffer = NULL;
        return true;
    }
    if (length > b->cap)
        return false;
    b->len = length;
    *outBuffer = b->buf;
    return true;
}

typedef struct {
    char name[129];
    int32_t width;
    int32_t height;
    int32_t layerCount;
    int32_t format;
    int64_t usage;
    int64_t reservedSize;
} buf_desc_info_t;

static int32_t read_buffer_descriptor_info(const AParcel *in, buf_desc_info_t *d) {
    memset(d, 0, sizeof(*d));
    int32_t non_null = 0;
    int32_t rc = g_ndk.readInt32(in, &non_null);
    if (rc != STATUS_OK_VAL) return rc;
    if (non_null == 0) return -EINVAL;

    int32_t start_pos = g_ndk.getDataPosition(in);
    int32_t psize = 0;
    if ((rc = g_ndk.readInt32(in, &psize)) != STATUS_OK_VAL) return rc;
    if (psize < 4) return -EINVAL;

    int8_t raw_name[256];
    memset(raw_name, 0, sizeof(raw_name));
    fixed_byte_buf_t fbb = { .buf = raw_name, .cap = (int32_t)sizeof(raw_name), .len = 0 };
    if ((rc = g_ndk.readByteArray(in, &fbb, fixed_byte_alloc)) != STATUS_OK_VAL) return rc;
    size_t copy_n = fbb.len > 128 ? 128 : (size_t)fbb.len;
    memcpy(d->name, raw_name, copy_n);
    d->name[copy_n] = '\0';

    if ((rc = g_ndk.readInt32(in, &d->width)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.readInt32(in, &d->height)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.readInt32(in, &d->layerCount)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.readInt32(in, &d->format)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.readInt64(in, &d->usage)) != STATUS_OK_VAL) return rc;
    if ((rc = g_ndk.readInt64(in, &d->reservedSize)) != STATUS_OK_VAL) return rc;

    /* Skip additionalOptions / any future fields cleanly */
    return g_ndk.setDataPosition(in, start_pos + psize);
}

static int alloc_dmabuf(uint64_t size) {
    if (g_heap_fd < 0) {
        /* Prefer system-uncached (write-combined) for GPU/MDP5 DMA coherency */
        g_heap_fd = open("/dev/dma_heap/system-uncached", O_RDWR | O_CLOEXEC);
        if (g_heap_fd < 0)
            g_heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
        if (g_heap_fd < 0)
            return -1;
    }
    struct dma_heap_allocation_data req;
    memset(&req, 0, sizeof(req));
    req.len = (size + 4095ULL) & ~4095ULL;
    if (req.len == 0)
        req.len = 4096;
    req.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(g_heap_fd, DMA_HEAP_IOCTL_ALLOC, &req) < 0)
        return -1;
    return (int)req.fd;
}

static bool compute_layout(const buf_desc_info_t *d, sailfish_gralloc_ints_t *out) {
    if (d->width <= 0 || d->height <= 0)
        return false;
    uint32_t layers = d->layerCount > 0 ? (uint32_t)d->layerCount : 1;
    uint32_t resolved = 0, fourcc = 0, bpp = 4;
    sailfish_format_info((uint32_t)d->format, &resolved, &fourcc, &bpp);

    uint32_t stride_px;
    uint32_t stride_bytes;
    uint64_t total_size;

    if (resolved == HAL_FMT_BLOB) {
        stride_px = (uint32_t)d->width;
        stride_bytes = stride_px;
        total_size = (uint64_t)stride_bytes * (uint64_t)d->height * layers;
    } else if (resolved == HAL_FMT_YCBCR_420_888 || resolved == HAL_FMT_YCRCB_420_SP ||
               resolved == HAL_FMT_YV12) {
        stride_px = ((uint32_t)d->width + 63U) & ~63U;
        stride_bytes = stride_px;
        uint32_t h_aligned = ((uint32_t)d->height + 1U) & ~1U;
        total_size = ((uint64_t)stride_bytes * h_aligned * 3ULL / 2ULL) * layers;
    } else {
        /* 64-pixel alignment matches Mesa Freedreno u_gralloc_fallback (FD530 gmem_align_w=64) */
        stride_px = ((uint32_t)d->width + 63U) & ~63U;
        stride_bytes = stride_px * bpp;
        total_size = (uint64_t)stride_bytes * (uint64_t)d->height * layers;
    }

    total_size = (total_size + 4095ULL) & ~4095ULL;
    if (total_size == 0)
        total_size = 4096;

    memset(out, 0, sizeof(*out));
    out->magic = SAILFISH_GRALLOC_MAGIC;
    out->width = (uint32_t)d->width;
    out->height = (uint32_t)d->height;
    out->stride = stride_px;
    out->stride_bytes = stride_bytes;
    out->format = resolved;
    out->requested_format = (uint32_t)d->format;
    out->drm_fourcc = fourcc;
    out->bpp = bpp;
    out->layer_count = layers;
    out->usage_lo = (uint32_t)((uint64_t)d->usage & 0xffffffffULL);
    out->usage_hi = (uint32_t)(((uint64_t)d->usage >> 32) & 0xffffffffULL);
    out->size_lo = (uint32_t)(total_size & 0xffffffffULL);
    out->size_hi = (uint32_t)((total_size >> 32) & 0xffffffffULL);
    strncpy((char *)out->name_words, d->name, sizeof(out->name_words) - 1);
    return true;
}

static int32_t handle_allocate2(const AParcel *in, AParcel *out) {
    buf_desc_info_t desc;
    int32_t rc = read_buffer_descriptor_info(in, &desc);
    if (rc != STATUS_OK_VAL)
        return write_status_err(out, ALLOC_ERR_BAD_DESC);

    int32_t count = 0;
    if ((rc = g_ndk.readInt32(in, &count)) != STATUS_OK_VAL)
        return write_status_err(out, ALLOC_ERR_BAD_DESC);
    if (count < 0 || count > 64)
        return write_status_err(out, ALLOC_ERR_BAD_DESC);

    sailfish_gralloc_ints_t layout;
    if (!compute_layout(&desc, &layout))
        return write_status_err(out, ALLOC_ERR_UNSUPPORTED);

    uint64_t total_size = ((uint64_t)layout.size_hi << 32) | layout.size_lo;
    int fds[64];
    for (int i = 0; i < count; i++) {
        fds[i] = alloc_dmabuf(total_size);
        if (fds[i] < 0) {
            for (int j = 0; j < i; j++) close(fds[j]);
            return write_status_err(out, ALLOC_ERR_NO_RESOURCES);
        }
    }

    if ((rc = write_status_ok(out)) != STATUS_OK_VAL)
        goto out_close;

    /* Non-null AllocationResult */
    if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) goto out_close;
    int32_t ar_pos = 0;
    if ((rc = begin_parcelable(out, &ar_pos)) != STATUS_OK_VAL) goto out_close;
    if ((rc = g_ndk.writeInt32(out, (int32_t)layout.stride)) != STATUS_OK_VAL) goto out_close;
    /* NativeHandle[] buffers */
    if ((rc = g_ndk.writeInt32(out, count)) != STATUS_OK_VAL) goto out_close;

    for (int i = 0; i < count; i++) {
        uint64_t id = __atomic_fetch_add(&g_next_buf_id, 1, __ATOMIC_RELAXED);
        layout.buffer_id_lo = (uint32_t)(id & 0xffffffffULL);
        layout.buffer_id_hi = (uint32_t)((id >> 32) & 0xffffffffULL);

        /* Non-null NativeHandle */
        if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) goto out_close;
        int32_t nh_pos = 0;
        if ((rc = begin_parcelable(out, &nh_pos)) != STATUS_OK_VAL) goto out_close;
        /* ParcelFileDescriptor[] fds (1 element; AParcel_writeParcelFileDescriptor writes its own null/non-null header) */
        if ((rc = g_ndk.writeInt32(out, 1)) != STATUS_OK_VAL) goto out_close;
        if ((rc = g_ndk.writeParcelFileDescriptor(out, fds[i])) != STATUS_OK_VAL) goto out_close;
        /* int32_t[] ints */
        if ((rc = g_ndk.writeInt32Array(out, (const int32_t *)&layout,
                                        SAILFISH_GRALLOC_NUM_INTS)) != STATUS_OK_VAL) goto out_close;
        if ((rc = end_parcelable(out, nh_pos)) != STATUS_OK_VAL) goto out_close;
    }

    rc = end_parcelable(out, ar_pos);

out_close:
    for (int i = 0; i < count; i++)
        close(fds[i]);
    return rc;
}

static int32_t handle_is_supported(const AParcel *in, AParcel *out) {
    buf_desc_info_t desc;
    int32_t rc = read_buffer_descriptor_info(in, &desc);
    if (rc != STATUS_OK_VAL)
        return write_status_err(out, ALLOC_ERR_BAD_DESC);

    sailfish_gralloc_ints_t layout;
    bool ok = compute_layout(&desc, &layout);
    if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
    return g_ndk.writeBool(out, ok);
}

static void *on_create(void *args) { return args; }
static void on_destroy(void *userData) { (void)userData; }

static int32_t on_transact(AIBinder *binder, uint32_t code,
                           const AParcel *in, AParcel *out) {
    (void)binder;
    int32_t rc;
    switch (code) {
    case TX_ALLOCATE2:
        return handle_allocate2(in, out);
    case TX_IS_SUPPORTED:
        return handle_is_supported(in, out);
    case TX_GET_IMAPPER_LIBRARY_SUFFIX:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeString(out, MAPPER_SUFFIX, (int32_t)(sizeof(MAPPER_SUFFIX) - 1));
    case TX_ALLOCATE:
        return write_status_err(out, ALLOC_ERR_UNSUPPORTED);
    case TX_GET_INTERFACE_VERSION:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 2);
    case TX_GET_INTERFACE_HASH:
        if ((rc = write_status_ok(out)) != STATUS_OK_VAL) return rc;
        return g_ndk.writeString(out, IALLOC_HASH, (int32_t)(sizeof(IALLOC_HASH) - 1));
    default:
        return STATUS_UNKNOWN_TX_VAL;
    }
}

int main(void) {
    void *lib = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!lib || !resolve_ndk(lib)) {
        kmsg_log("<3>sailfish_allocator_hal: failed to load libbinder_ndk.so\n");
        return 1;
    }

    g_heap_fd = open("/dev/dma_heap/system-uncached", O_RDWR | O_CLOEXEC);
    if (g_heap_fd < 0)
        g_heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (g_heap_fd < 0)
        kmsg_log("<4>sailfish_allocator_hal: /dev/dma_heap/system[-uncached] not open yet (will retry)\n");

    g_ndk.setThreadPoolMaxThreadCount(4);
    g_ndk.startThreadPool();

    AIBinder_Class *cls = g_ndk.Class_define(IALLOC_DESC, on_create, on_destroy, on_transact);
    if (!cls) {
        kmsg_log("<3>sailfish_allocator_hal: AIBinder_Class_define failed\n");
        return 1;
    }

    AIBinder *binder = g_ndk.AIBinder_new(cls, NULL);
    if (!binder) {
        kmsg_log("<3>sailfish_allocator_hal: AIBinder_new failed\n");
        return 1;
    }
    g_ndk.markVintfStability(binder);

    int32_t st = g_ndk.addService(binder, IALLOC_INSTANCE);
    if (st != STATUS_OK_VAL) {
        kmsg_log("<3>sailfish_allocator_hal: AServiceManager_addService failed\n");
        return 1;
    }

    kmsg_log("<6>sailfish_allocator_hal: registered android.hardware.graphics.allocator.IAllocator/default (V2)\n");
    g_ndk.joinThreadPool();
    return 0;
}
