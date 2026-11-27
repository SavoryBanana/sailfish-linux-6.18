#ifndef SAILFISH_GRALLOC_H
#define SAILFISH_GRALLOC_H

#include <stdint.h>

#define SAILFISH_GRALLOC_MAGIC 0x53464735U /* "SFG5" */

/* Android HAL pixel formats */
enum {
    HAL_FMT_RGBA_8888            = 1,
    HAL_FMT_RGBX_8888            = 2,
    HAL_FMT_RGB_888              = 3,
    HAL_FMT_RGB_565              = 4,
    HAL_FMT_BGRA_8888            = 5,
    HAL_FMT_RGBA_FP16            = 22,
    HAL_FMT_BLOB                 = 33,
    HAL_FMT_IMPLEMENTATION_DEF   = 34,
    HAL_FMT_YCBCR_420_888        = 35,
    HAL_FMT_RGBA_1010102         = 43,
    HAL_FMT_R_8                  = 56,
    HAL_FMT_YV12                 = 0x32315659,
    HAL_FMT_YCRCB_420_SP         = 0x11,
};

#define FOURCC_CODE(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* Standard Android native_handle_t header */
#ifndef NATIVE_HANDLE_H_
typedef struct native_handle {
    int version; /* sizeof(native_handle_t) */
    int numFds;
    int numInts;
    int data[];
} native_handle_t;

typedef const native_handle_t *buffer_handle_t;
#endif

/*
 * Sailfish Gralloc 5 native handle payload layout:
 *   numFds  = 1: data[0] = dmabuf_fd
 *   numInts = SAILFISH_GRALLOC_NUM_INTS: stored immediately after data[0]
 */
typedef struct sailfish_gralloc_ints {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t stride;          /* pixel stride (64-pixel aligned for Adreno 530 / MDP5) */
    uint32_t stride_bytes;    /* byte stride of plane 0 */
    uint32_t format;          /* HAL pixel format (resolved if IMPLEMENTATION_DEFINED) */
    uint32_t requested_format;/* original requested HAL pixel format */
    uint32_t drm_fourcc;
    uint32_t bpp;
    uint32_t layer_count;
    uint32_t usage_lo;
    uint32_t usage_hi;
    uint32_t size_lo;
    uint32_t size_hi;
    uint32_t buffer_id_lo;
    uint32_t buffer_id_hi;
    int32_t  dataspace;
    int32_t  blend_mode;
    uint32_t name_words[16];  /* 64-byte NUL-terminated debug name */
} sailfish_gralloc_ints_t;

#define SAILFISH_GRALLOC_NUM_FDS  1
#define SAILFISH_GRALLOC_NUM_INTS ((int)(sizeof(sailfish_gralloc_ints_t) / sizeof(int)))

static inline const sailfish_gralloc_ints_t *sailfish_handle_ints_const(const native_handle_t *h) {
    if (!h || h->numFds < 1 || h->numInts < SAILFISH_GRALLOC_NUM_INTS)
        return NULL;
    const sailfish_gralloc_ints_t *info = (const sailfish_gralloc_ints_t *)&h->data[h->numFds];
    if (info->magic != SAILFISH_GRALLOC_MAGIC)
        return NULL;
    return info;
}

static inline sailfish_gralloc_ints_t *sailfish_handle_ints_mut(native_handle_t *h) {
    if (!h || h->numFds < 1 || h->numInts < SAILFISH_GRALLOC_NUM_INTS)
        return NULL;
    sailfish_gralloc_ints_t *info = (sailfish_gralloc_ints_t *)&h->data[h->numFds];
    if (info->magic != SAILFISH_GRALLOC_MAGIC)
        return NULL;
    return info;
}

static inline uint32_t sailfish_format_info(uint32_t req_fmt, uint32_t *out_resolved,
                                            uint32_t *out_fourcc, uint32_t *out_bpp) {
    uint32_t fmt = req_fmt;
    if (fmt == HAL_FMT_IMPLEMENTATION_DEF)
        fmt = HAL_FMT_RGBA_8888;
    uint32_t fourcc = FOURCC_CODE('A', 'B', '2', '4');
    uint32_t bpp = 4;
    switch (fmt) {
    case HAL_FMT_RGBA_8888:
        fourcc = FOURCC_CODE('A', 'B', '2', '4'); bpp = 4; break;
    case HAL_FMT_RGBX_8888:
        fourcc = FOURCC_CODE('X', 'B', '2', '4'); bpp = 4; break;
    case HAL_FMT_BGRA_8888:
        fourcc = FOURCC_CODE('A', 'R', '2', '4'); bpp = 4; break;
    case HAL_FMT_RGB_888:
        fourcc = FOURCC_CODE('B', 'G', '2', '4'); bpp = 3; break;
    case HAL_FMT_RGB_565:
        fourcc = FOURCC_CODE('R', 'G', '1', '6'); bpp = 2; break;
    case HAL_FMT_RGBA_FP16:
        fourcc = FOURCC_CODE('A', 'B', '4', 'H'); bpp = 8; break;
    case HAL_FMT_RGBA_1010102:
        fourcc = FOURCC_CODE('A', 'B', '3', '0'); bpp = 4; break;
    case HAL_FMT_R_8:
    case HAL_FMT_BLOB:
        fourcc = FOURCC_CODE('R', '8', ' ', ' '); bpp = 1; break;
    case HAL_FMT_YCBCR_420_888:
    case HAL_FMT_YCRCB_420_SP:
        fourcc = FOURCC_CODE('N', 'V', '1', '2'); bpp = 1; break;
    case HAL_FMT_YV12:
        fourcc = FOURCC_CODE('Y', 'V', '1', '2'); bpp = 1; break;
    default:
        fourcc = FOURCC_CODE('A', 'B', '2', '4'); bpp = 4; break;
    }
    if (out_resolved) *out_resolved = fmt;
    if (out_fourcc) *out_fourcc = fourcc;
    if (out_bpp) *out_bpp = bpp;
    return fmt;
}

#endif /* SAILFISH_GRALLOC_H */
