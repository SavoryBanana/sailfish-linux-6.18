#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sailfish_gralloc.h"

/* Stable-C AIMapper v5 ABI (verified against Android 17 libui.so) */
typedef int32_t AIMapper_Error;

enum {
    AIMAPPER_ERROR_NONE           = 0,
    AIMAPPER_ERROR_BAD_DESCRIPTOR = 1,
    AIMAPPER_ERROR_BAD_BUFFER     = 2,
    AIMAPPER_ERROR_BAD_VALUE      = 3,
    AIMAPPER_ERROR_NO_RESOURCES   = 5,
    AIMAPPER_ERROR_UNSUPPORTED    = 7,
};

typedef enum AIMapper_Version {
    AIMAPPER_VERSION_5 = 5,
} AIMapper_Version;

typedef struct ARect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} ARect;

typedef struct AIMapper_MetadataType {
    const char *name;
    int64_t value;
} AIMapper_MetadataType;

typedef struct AIMapper_MetadataTypeDescription {
    AIMapper_MetadataType metadataType;
    const char *description;
    bool isGettable;
    bool isSettable;
    uint8_t reserved[32];
} AIMapper_MetadataTypeDescription;

typedef void (*AIMapper_DumpBufferCallback)(void *context, AIMapper_MetadataType metadataType,
                                            const void *value, size_t valueSize);
typedef void (*AIMapper_BeginDumpBufferCallback)(void *context);

typedef struct AIMapper_v5 {
    AIMapper_Error (*importBuffer)(const native_handle_t *handle, buffer_handle_t *outBufferHandle);
    AIMapper_Error (*freeBuffer)(buffer_handle_t buffer);
    AIMapper_Error (*getTransportSize)(buffer_handle_t buffer, uint32_t *outNumFds, uint32_t *outNumInts);
    AIMapper_Error (*lock)(buffer_handle_t buffer, uint64_t cpuUsage, ARect accessRegion,
                           int acquireFence, void **outData);
    AIMapper_Error (*unlock)(buffer_handle_t buffer, int *releaseFence);
    AIMapper_Error (*flushLockedBuffer)(buffer_handle_t buffer);
    AIMapper_Error (*rereadLockedBuffer)(buffer_handle_t buffer);
    int32_t (*getMetadata)(buffer_handle_t buffer, AIMapper_MetadataType metadataType,
                           void *destBuffer, size_t destBufferSize);
    int32_t (*getStandardMetadata)(buffer_handle_t buffer, int64_t standardMetadataType,
                                   void *destBuffer, size_t destBufferSize);
    AIMapper_Error (*setMetadata)(buffer_handle_t buffer, AIMapper_MetadataType metadataType,
                                  const void *metadata, size_t metadataSize);
    AIMapper_Error (*setStandardMetadata)(buffer_handle_t buffer, int64_t standardMetadataType,
                                          const void *metadata, size_t metadataSize);
    AIMapper_Error (*listSupportedMetadataTypes)(const AIMapper_MetadataTypeDescription **outDescriptionList,
                                                 size_t *outNumberOfDescriptions);
    AIMapper_Error (*dumpBuffer)(buffer_handle_t buffer, AIMapper_DumpBufferCallback dumpBufferCallback,
                                 void *context);
    AIMapper_Error (*dumpAllBuffers)(AIMapper_BeginDumpBufferCallback beginDumpBufferCallback,
                                     AIMapper_DumpBufferCallback dumpBufferCallback, void *context);
    AIMapper_Error (*getReservedRegion)(buffer_handle_t buffer, void **outReservedRegion,
                                        uint64_t *outReservedSize);
} AIMapper_v5;

typedef struct AIMapper {
    AIMapper_Version version;
    union {
        AIMapper_v5 v5;
    };
} AIMapper;

/* StandardMetadataType IDs */
enum {
    SMT_BUFFER_ID              = 1,
    SMT_NAME                   = 2,
    SMT_WIDTH                  = 3,
    SMT_HEIGHT                 = 4,
    SMT_LAYER_COUNT            = 5,
    SMT_PIXEL_FORMAT_REQUESTED = 6,
    SMT_PIXEL_FORMAT_FOURCC    = 7,
    SMT_PIXEL_FORMAT_MODIFIER  = 8,
    SMT_USAGE                  = 9,
    SMT_ALLOCATION_SIZE        = 10,
    SMT_PROTECTED_CONTENT      = 11,
    SMT_COMPRESSION            = 12,
    SMT_INTERLACED             = 13,
    SMT_CHROMA_SITING          = 14,
    SMT_PLANE_LAYOUTS          = 15,
    SMT_CROP                   = 16,
    SMT_DATASPACE              = 17,
    SMT_BLEND_MODE             = 18,
    SMT_STRIDE                 = 23,
};

/* Imported buffer tracking (for mmap caching) */
typedef struct imported_buf {
    native_handle_t *handle;
    void *mapped_addr;
    size_t mapped_size;
    int lock_count;
    struct imported_buf *next;
} imported_buf_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static imported_buf_t *g_imported = NULL;

static imported_buf_t *find_imported_locked(buffer_handle_t buffer) {
    for (imported_buf_t *it = g_imported; it; it = it->next) {
        if ((buffer_handle_t)it->handle == buffer)
            return it;
    }
    return NULL;
}

static void wait_and_close_fence(int fence_fd) {
    if (fence_fd < 0)
        return;
    struct pollfd pfd = { .fd = fence_fd, .events = POLLIN };
    int ret;
    do {
        ret = poll(&pfd, 1, 3000);
    } while (ret < 0 && errno == EINTR);
    close(fence_fd);
}

static void dmabuf_sync(int fd, uint64_t flags) {
    if (fd < 0)
        return;
    struct dma_buf_sync sync = { .flags = flags };
    int ret;
    do {
        ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (ret < 0 && errno == EINTR);
}

static AIMapper_Error sf_importBuffer(const native_handle_t *handle,
                                      buffer_handle_t *outBufferHandle) {
    if (!handle || !outBufferHandle)
        return AIMAPPER_ERROR_BAD_BUFFER;
    const sailfish_gralloc_ints_t *info = sailfish_handle_ints_const(handle);
    if (!info)
        return AIMAPPER_ERROR_BAD_BUFFER;

    int dup_fd = fcntl(handle->data[0], F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0)
        return AIMAPPER_ERROR_NO_RESOURCES;

    size_t hsz = sizeof(native_handle_t) +
                 (size_t)(SAILFISH_GRALLOC_NUM_FDS + SAILFISH_GRALLOC_NUM_INTS) * sizeof(int);
    native_handle_t *copy = (native_handle_t *)calloc(1, hsz);
    if (!copy) {
        close(dup_fd);
        return AIMAPPER_ERROR_NO_RESOURCES;
    }
    copy->version = (int)sizeof(native_handle_t);
    copy->numFds = SAILFISH_GRALLOC_NUM_FDS;
    copy->numInts = SAILFISH_GRALLOC_NUM_INTS;
    copy->data[0] = dup_fd;
    memcpy(&copy->data[SAILFISH_GRALLOC_NUM_FDS], info, sizeof(*info));

    imported_buf_t *node = (imported_buf_t *)calloc(1, sizeof(*node));
    if (!node) {
        close(dup_fd);
        free(copy);
        return AIMAPPER_ERROR_NO_RESOURCES;
    }
    node->handle = copy;

    pthread_mutex_lock(&g_lock);
    node->next = g_imported;
    g_imported = node;
    pthread_mutex_unlock(&g_lock);

    *outBufferHandle = copy;
    return AIMAPPER_ERROR_NONE;
}

static AIMapper_Error sf_freeBuffer(buffer_handle_t buffer) {
    if (!buffer)
        return AIMAPPER_ERROR_BAD_BUFFER;

    imported_buf_t *found = NULL;
    pthread_mutex_lock(&g_lock);
    imported_buf_t **pp = &g_imported;
    while (*pp) {
        if ((buffer_handle_t)(*pp)->handle == buffer) {
            found = *pp;
            *pp = found->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_lock);

    if (!found)
        return AIMAPPER_ERROR_BAD_BUFFER;

    if (found->mapped_addr && found->mapped_addr != MAP_FAILED)
        munmap(found->mapped_addr, found->mapped_size);
    if (found->handle->data[0] >= 0)
        close(found->handle->data[0]);
    free(found->handle);
    free(found);
    return AIMAPPER_ERROR_NONE;
}

static AIMapper_Error sf_getTransportSize(buffer_handle_t buffer, uint32_t *outNumFds,
                                          uint32_t *outNumInts) {
    if (!sailfish_handle_ints_const(buffer) || !outNumFds || !outNumInts)
        return AIMAPPER_ERROR_BAD_BUFFER;
    *outNumFds = SAILFISH_GRALLOC_NUM_FDS;
    *outNumInts = (uint32_t)SAILFISH_GRALLOC_NUM_INTS;
    return AIMAPPER_ERROR_NONE;
}

static AIMapper_Error sf_lock(buffer_handle_t buffer, uint64_t cpuUsage, ARect accessRegion,
                              int acquireFence, void **outData) {
    (void)cpuUsage;
    (void)accessRegion;
    wait_and_close_fence(acquireFence);

    if (!buffer || !outData)
        return AIMAPPER_ERROR_BAD_BUFFER;
    const sailfish_gralloc_ints_t *info = sailfish_handle_ints_const(buffer);
    if (!info)
        return AIMAPPER_ERROR_BAD_BUFFER;

    uint64_t total_size = ((uint64_t)info->size_hi << 32) | info->size_lo;
    if (total_size == 0)
        return AIMAPPER_ERROR_BAD_BUFFER;

    pthread_mutex_lock(&g_lock);
    imported_buf_t *node = find_imported_locked(buffer);
    if (!node) {
        pthread_mutex_unlock(&g_lock);
        return AIMAPPER_ERROR_BAD_BUFFER;
    }
    if (!node->mapped_addr) {
        void *addr = mmap(NULL, (size_t)total_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, node->handle->data[0], 0);
        if (addr == MAP_FAILED) {
            pthread_mutex_unlock(&g_lock);
            return AIMAPPER_ERROR_NO_RESOURCES;
        }
        node->mapped_addr = addr;
        node->mapped_size = (size_t)total_size;
    }
    node->lock_count++;
    void *ptr = node->mapped_addr;
    int fd = node->handle->data[0];
    pthread_mutex_unlock(&g_lock);

    dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
    *outData = ptr;
    return AIMAPPER_ERROR_NONE;
}

static AIMapper_Error sf_unlock(buffer_handle_t buffer, int *releaseFence) {
    if (releaseFence)
        *releaseFence = -1;
    if (!buffer)
        return AIMAPPER_ERROR_BAD_BUFFER;

    pthread_mutex_lock(&g_lock);
    imported_buf_t *node = find_imported_locked(buffer);
    if (!node) {
        pthread_mutex_unlock(&g_lock);
        return AIMAPPER_ERROR_BAD_BUFFER;
    }
    if (node->lock_count > 0)
        node->lock_count--;
    int fd = node->handle->data[0];
    pthread_mutex_unlock(&g_lock);

    dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
    return AIMAPPER_ERROR_NONE;
}

static AIMapper_Error sf_flushLockedBuffer(buffer_handle_t buffer) {
    if (!sailfish_handle_ints_const(buffer))
        return AIMAPPER_ERROR_BAD_BUFFER;
    dmabuf_sync(buffer->data[0], DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    return AIMAPPER_ERROR_NONE;
}

static AIMapper_Error sf_rereadLockedBuffer(buffer_handle_t buffer) {
    if (!sailfish_handle_ints_const(buffer))
        return AIMAPPER_ERROR_BAD_BUFFER;
    dmabuf_sync(buffer->data[0], DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    return AIMAPPER_ERROR_NONE;
}

/* Metadata byte-stream encoder matching Android libgralloctypes */
typedef struct meta_writer {
    uint8_t *buf;
    size_t cap;
    size_t pos;
} meta_writer_t;

static void mw_bytes(meta_writer_t *w, const void *src, size_t n) {
    if (w->buf && w->pos + n <= w->cap)
        memcpy(w->buf + w->pos, src, n);
    w->pos += n;
}

static void mw_i32(meta_writer_t *w, int32_t v) { mw_bytes(w, &v, sizeof(v)); }
static void mw_u32(meta_writer_t *w, uint32_t v) { mw_bytes(w, &v, sizeof(v)); }
static void mw_i64(meta_writer_t *w, int64_t v) { mw_bytes(w, &v, sizeof(v)); }
static void mw_u64(meta_writer_t *w, uint64_t v) { mw_bytes(w, &v, sizeof(v)); }

static void mw_str(meta_writer_t *w, const char *s) {
    int64_t len = (int64_t)strlen(s);
    mw_i64(w, len);
    mw_bytes(w, s, (size_t)len);
}

static void mw_ext_type(meta_writer_t *w, const char *name, int64_t val) {
    mw_str(w, name);
    mw_i64(w, val);
}

static void mw_std_hdr(meta_writer_t *w, int64_t smt) {
    mw_ext_type(w, "android.hardware.graphics.common.StandardMetadataType", smt);
}

static void mw_plane_component(meta_writer_t *w, int64_t comp_type,
                               int64_t offset_bits, int64_t size_bits) {
    mw_ext_type(w, "android.hardware.graphics.common.PlaneLayoutComponentType", comp_type);
    mw_i64(w, offset_bits);
    mw_i64(w, size_bits);
}

static void encode_plane_layouts(meta_writer_t *w, const sailfish_gralloc_ints_t *info) {
    /* PlaneLayoutComponentType: R=1024, G=2048, B=4096, A=1073741824, Y=1, CB=2, CR=4, RAW=1 */
    const int64_t COMP_Y = 1, COMP_CB = 2, COMP_CR = 4;
    const int64_t COMP_R = 1024, COMP_G = 2048, COMP_B = 4096, COMP_A = 1073741824;

    if (info->format == HAL_FMT_YCBCR_420_888 || info->format == HAL_FMT_YCRCB_420_SP) {
        uint64_t y_size = (uint64_t)info->stride_bytes * info->height;
        uint64_t uv_size = (uint64_t)info->stride_bytes * ((info->height + 1) / 2);
        mw_i64(w, 2); /* 2 planes: Y, interleaved UV */
        /* Plane 0: Y */
        mw_i64(w, 1);
        mw_plane_component(w, COMP_Y, 0, 8);
        mw_i64(w, 0);                        /* offsetInBytes */
        mw_i64(w, 8);                        /* sampleIncrementInBits */
        mw_i64(w, (int64_t)info->stride_bytes);
        mw_i64(w, (int64_t)info->width);
        mw_i64(w, (int64_t)info->height);
        mw_i64(w, (int64_t)y_size);
        mw_i64(w, 1);                        /* horizontalSubsampling */
        mw_i64(w, 1);                        /* verticalSubsampling */
        /* Plane 1: UV (NV12) */
        mw_i64(w, 2);
        mw_plane_component(w, COMP_CB, 0, 8);
        mw_plane_component(w, COMP_CR, 8, 8);
        mw_i64(w, (int64_t)y_size);
        mw_i64(w, 16);
        mw_i64(w, (int64_t)info->stride_bytes);
        mw_i64(w, (int64_t)((info->width + 1) / 2));
        mw_i64(w, (int64_t)((info->height + 1) / 2));
        mw_i64(w, (int64_t)uv_size);
        mw_i64(w, 2);
        mw_i64(w, 2);
        return;
    }

    uint64_t plane_size = (uint64_t)info->stride_bytes * info->height;
    mw_i64(w, 1); /* 1 plane */
    switch (info->format) {
    case HAL_FMT_RGBX_8888:
        mw_i64(w, 3);
        mw_plane_component(w, COMP_R, 0, 8);
        mw_plane_component(w, COMP_G, 8, 8);
        mw_plane_component(w, COMP_B, 16, 8);
        break;
    case HAL_FMT_BGRA_8888:
        mw_i64(w, 4);
        mw_plane_component(w, COMP_B, 0, 8);
        mw_plane_component(w, COMP_G, 8, 8);
        mw_plane_component(w, COMP_R, 16, 8);
        mw_plane_component(w, COMP_A, 24, 8);
        break;
    case HAL_FMT_RGB_888:
        mw_i64(w, 3);
        mw_plane_component(w, COMP_R, 0, 8);
        mw_plane_component(w, COMP_G, 8, 8);
        mw_plane_component(w, COMP_B, 16, 8);
        break;
    case HAL_FMT_RGB_565:
        mw_i64(w, 3);
        mw_plane_component(w, COMP_B, 0, 5);
        mw_plane_component(w, COMP_G, 5, 6);
        mw_plane_component(w, COMP_R, 11, 5);
        break;
    case HAL_FMT_RGBA_FP16:
        mw_i64(w, 4);
        mw_plane_component(w, COMP_R, 0, 16);
        mw_plane_component(w, COMP_G, 16, 16);
        mw_plane_component(w, COMP_B, 32, 16);
        mw_plane_component(w, COMP_A, 48, 16);
        break;
    case HAL_FMT_RGBA_1010102:
        mw_i64(w, 4);
        mw_plane_component(w, COMP_R, 0, 10);
        mw_plane_component(w, COMP_G, 10, 10);
        mw_plane_component(w, COMP_B, 20, 10);
        mw_plane_component(w, COMP_A, 30, 2);
        break;
    case HAL_FMT_R_8:
    case HAL_FMT_BLOB:
        mw_i64(w, 1);
        mw_plane_component(w, COMP_R, 0, 8);
        break;
    case HAL_FMT_RGBA_8888:
    default:
        mw_i64(w, 4);
        mw_plane_component(w, COMP_R, 0, 8);
        mw_plane_component(w, COMP_G, 8, 8);
        mw_plane_component(w, COMP_B, 16, 8);
        mw_plane_component(w, COMP_A, 24, 8);
        break;
    }
    mw_i64(w, 0); /* offsetInBytes */
    mw_i64(w, (int64_t)(info->bpp * 8));
    mw_i64(w, (int64_t)info->stride_bytes);
    mw_i64(w, (int64_t)info->width);
    mw_i64(w, (int64_t)info->height);
    mw_i64(w, (int64_t)plane_size);
    mw_i64(w, 1);
    mw_i64(w, 1);
}

static int32_t sf_getStandardMetadata(buffer_handle_t buffer, int64_t smt,
                                      void *destBuffer, size_t destBufferSize) {
    const sailfish_gralloc_ints_t *info = sailfish_handle_ints_const(buffer);
    if (!info)
        return -AIMAPPER_ERROR_BAD_BUFFER;

    meta_writer_t w = { .buf = (uint8_t *)destBuffer, .cap = destBufferSize, .pos = 0 };

    switch (smt) {
    case SMT_BUFFER_ID:
        mw_std_hdr(&w, smt);
        mw_u64(&w, ((uint64_t)info->buffer_id_hi << 32) | info->buffer_id_lo);
        return (int32_t)w.pos;
    case SMT_NAME: {
        char name[65];
        memcpy(name, info->name_words, 64);
        name[64] = '\0';
        mw_std_hdr(&w, smt);
        mw_str(&w, name);
        return (int32_t)w.pos;
    }
    case SMT_WIDTH:
        mw_std_hdr(&w, smt);
        mw_u64(&w, info->width);
        return (int32_t)w.pos;
    case SMT_HEIGHT:
        mw_std_hdr(&w, smt);
        mw_u64(&w, info->height);
        return (int32_t)w.pos;
    case SMT_LAYER_COUNT:
        mw_std_hdr(&w, smt);
        mw_u64(&w, info->layer_count ? info->layer_count : 1);
        return (int32_t)w.pos;
    case SMT_PIXEL_FORMAT_REQUESTED:
        mw_std_hdr(&w, smt);
        mw_i32(&w, (int32_t)info->requested_format);
        return (int32_t)w.pos;
    case SMT_PIXEL_FORMAT_FOURCC:
        mw_std_hdr(&w, smt);
        mw_u32(&w, info->drm_fourcc);
        return (int32_t)w.pos;
    case SMT_PIXEL_FORMAT_MODIFIER:
        mw_std_hdr(&w, smt);
        mw_u64(&w, 0); /* DRM_FORMAT_MOD_LINEAR */
        return (int32_t)w.pos;
    case SMT_USAGE:
        mw_std_hdr(&w, smt);
        mw_u64(&w, ((uint64_t)info->usage_hi << 32) | info->usage_lo);
        return (int32_t)w.pos;
    case SMT_ALLOCATION_SIZE:
        mw_std_hdr(&w, smt);
        mw_u64(&w, ((uint64_t)info->size_hi << 32) | info->size_lo);
        return (int32_t)w.pos;
    case SMT_PROTECTED_CONTENT:
        mw_std_hdr(&w, smt);
        mw_u64(&w, 0);
        return (int32_t)w.pos;
    case SMT_COMPRESSION:
        mw_std_hdr(&w, smt);
        mw_ext_type(&w, "android.hardware.graphics.common.Compression", 0);
        return (int32_t)w.pos;
    case SMT_INTERLACED:
        mw_std_hdr(&w, smt);
        mw_ext_type(&w, "android.hardware.graphics.common.Interlaced", 0);
        return (int32_t)w.pos;
    case SMT_CHROMA_SITING:
        mw_std_hdr(&w, smt);
        mw_ext_type(&w, "android.hardware.graphics.common.ChromaSiting", 0);
        return (int32_t)w.pos;
    case SMT_PLANE_LAYOUTS:
        mw_std_hdr(&w, smt);
        encode_plane_layouts(&w, info);
        return (int32_t)w.pos;
    case SMT_CROP:
        mw_std_hdr(&w, smt);
        mw_i64(&w, 1); /* 1 rect */
        mw_i64(&w, 0);
        mw_i64(&w, 0);
        mw_i64(&w, (int64_t)info->width);
        mw_i64(&w, (int64_t)info->height);
        return (int32_t)w.pos;
    case SMT_DATASPACE:
        mw_std_hdr(&w, smt);
        mw_i32(&w, info->dataspace);
        return (int32_t)w.pos;
    case SMT_BLEND_MODE:
        mw_std_hdr(&w, smt);
        mw_i32(&w, info->blend_mode);
        return (int32_t)w.pos;
    case SMT_STRIDE:
        mw_std_hdr(&w, smt);
        mw_u32(&w, info->stride);
        return (int32_t)w.pos;
    default:
        return -AIMAPPER_ERROR_UNSUPPORTED;
    }
}

static int32_t sf_getMetadata(buffer_handle_t buffer, AIMapper_MetadataType metadataType,
                              void *destBuffer, size_t destBufferSize) {
    if (metadataType.name &&
        strcmp(metadataType.name, "android.hardware.graphics.common.StandardMetadataType") == 0) {
        return sf_getStandardMetadata(buffer, metadataType.value, destBuffer, destBufferSize);
    }
    return -AIMAPPER_ERROR_UNSUPPORTED;
}

static AIMapper_Error sf_setStandardMetadata(buffer_handle_t buffer, int64_t smt,
                                             const void *metadata, size_t metadataSize) {
    if (!buffer || !metadata)
        return AIMAPPER_ERROR_BAD_VALUE;
    sailfish_gralloc_ints_t *info = sailfish_handle_ints_mut((native_handle_t *)buffer);
    if (!info)
        return AIMAPPER_ERROR_BAD_BUFFER;

    /* StandardMetadataType header is 8 + 53 + 8 = 69 bytes */
    const size_t hdr_sz = 8 + 53 + 8;
    if (metadataSize < hdr_sz + 4)
        return AIMAPPER_ERROR_BAD_VALUE;

    int32_t val = 0;
    memcpy(&val, (const uint8_t *)metadata + hdr_sz, sizeof(val));

    if (smt == SMT_DATASPACE) {
        info->dataspace = val;
        return AIMAPPER_ERROR_NONE;
    }
    if (smt == SMT_BLEND_MODE) {
        info->blend_mode = val;
        return AIMAPPER_ERROR_NONE;
    }
    return AIMAPPER_ERROR_UNSUPPORTED;
}

static AIMapper_Error sf_setMetadata(buffer_handle_t buffer, AIMapper_MetadataType metadataType,
                                     const void *metadata, size_t metadataSize) {
    if (metadataType.name &&
        strcmp(metadataType.name, "android.hardware.graphics.common.StandardMetadataType") == 0) {
        return sf_setStandardMetadata(buffer, metadataType.value, metadata, metadataSize);
    }
    return AIMAPPER_ERROR_UNSUPPORTED;
}

static AIMapper_Error sf_listSupportedMetadataTypes(
        const AIMapper_MetadataTypeDescription **outDescriptionList,
        size_t *outNumberOfDescriptions) {
    static const AIMapper_MetadataTypeDescription descs[] = {
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_BUFFER_ID }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_NAME }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_WIDTH }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_HEIGHT }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_LAYER_COUNT }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_PIXEL_FORMAT_REQUESTED }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_PIXEL_FORMAT_FOURCC }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_PIXEL_FORMAT_MODIFIER }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_USAGE }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_ALLOCATION_SIZE }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_PROTECTED_CONTENT }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_COMPRESSION }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_INTERLACED }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_CHROMA_SITING }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_PLANE_LAYOUTS }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_CROP }, "", true, false, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_DATASPACE }, "", true, true, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_BLEND_MODE }, "", true, true, {0} },
        { { "android.hardware.graphics.common.StandardMetadataType", SMT_STRIDE }, "", true, false, {0} },
    };
    if (!outDescriptionList || !outNumberOfDescriptions)
        return AIMAPPER_ERROR_BAD_VALUE;
    *outDescriptionList = descs;
    *outNumberOfDescriptions = sizeof(descs) / sizeof(descs[0]);
    return AIMAPPER_ERROR_NONE;
}

static AIMapper_Error sf_dumpBuffer(buffer_handle_t buffer,
                                    AIMapper_DumpBufferCallback cb, void *ctx) {
    (void)buffer;
    (void)cb;
    (void)ctx;
    return AIMAPPER_ERROR_NONE;
}

static AIMapper_Error sf_dumpAllBuffers(AIMapper_BeginDumpBufferCallback begin_cb,
                                        AIMapper_DumpBufferCallback cb, void *ctx) {
    (void)begin_cb;
    (void)cb;
    (void)ctx;
    return AIMAPPER_ERROR_NONE;
}

static AIMapper_Error sf_getReservedRegion(buffer_handle_t buffer, void **outReservedRegion,
                                           uint64_t *outReservedSize) {
    (void)buffer;
    if (outReservedRegion) *outReservedRegion = NULL;
    if (outReservedSize) *outReservedSize = 0;
    return AIMAPPER_ERROR_UNSUPPORTED;
}

static AIMapper g_mapper = {
    .version = AIMAPPER_VERSION_5,
    .v5 = {
        .importBuffer               = sf_importBuffer,
        .freeBuffer                 = sf_freeBuffer,
        .getTransportSize           = sf_getTransportSize,
        .lock                       = sf_lock,
        .unlock                     = sf_unlock,
        .flushLockedBuffer          = sf_flushLockedBuffer,
        .rereadLockedBuffer         = sf_rereadLockedBuffer,
        .getMetadata                = sf_getMetadata,
        .getStandardMetadata        = sf_getStandardMetadata,
        .setMetadata                = sf_setMetadata,
        .setStandardMetadata        = sf_setStandardMetadata,
        .listSupportedMetadataTypes = sf_listSupportedMetadataTypes,
        .dumpBuffer                 = sf_dumpBuffer,
        .dumpAllBuffers             = sf_dumpAllBuffers,
        .getReservedRegion          = sf_getReservedRegion,
    },
};

__attribute__((visibility("default")))
const uint32_t ANDROID_HAL_STABLEC_VERSION = AIMAPPER_VERSION_5;

__attribute__((visibility("default")))
AIMapper_Error AIMapper_loadIMapper(AIMapper **outImplementation) {
    if (!outImplementation)
        return AIMAPPER_ERROR_BAD_VALUE;
    *outImplementation = &g_mapper;
    return AIMAPPER_ERROR_NONE;
}
