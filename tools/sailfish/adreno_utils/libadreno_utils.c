// SPDX-License-Identifier: MIT
/*
 * Open-source Adreno 530 (MSM8996 Pro) surface alignment utility library
 * Replaces proprietary /vendor/lib64/libadreno_utils.so (which requires
 * legacy /dev/kgsl-3d0) for mainline Linux 6.18 drm/msm + Mesa Freedreno.
 */

typedef unsigned int uint32_t;

static inline int align_up(int val, int align) {
    return (val + align - 1) & ~(align - 1);
}

int isUBWCSupportedByGpu(unsigned int format) {
    (void)format;
    /* Disable UBWC so all gralloc buffers use linear modifier (DRM_FORMAT_MOD_LINEAR)
     * required by Mesa Freedreno a5xx and mainline MDP5 scanout. */
    return 0;
}

unsigned int get_gpu_pixel_alignment(void) {
    return 1;
}

int ReadGpuID(unsigned int *gpu_id) {
    if (gpu_id)
        *gpu_id = 530;
    return 0;
}

int get_gpu_id(void) {
    return 530;
}

int compute_surface_padding(int width, int bpp, int surface_tile_height,
                            int screen_tile_height, int padding_threshold) {
    (void)bpp;
    (void)surface_tile_height;
    (void)screen_tile_height;
    (void)padding_threshold;
    /* Adreno 530 (FD530) requires 64-pixel pitch alignment (gmem_align_w = 64) */
    return align_up(width, 64);
}

void compute_aligned_width_and_height(int width, int height, int bpp,
                                      int tile_mode, int raster_mode,
                                      int padding_threshold,
                                      int *aligned_w, int *aligned_h) {
    (void)bpp;
    (void)tile_mode;
    (void)raster_mode;
    (void)padding_threshold;
    /* Match Mesa Freedreno fd5_layout.c & fd_layout_resource_for_handle:
     * gmem_align_w = 64 pixels, height alignment = 32 pixels. */
    if (aligned_w)
        *aligned_w = align_up(width, 64);
    if (aligned_h)
        *aligned_h = align_up(height, 32);
}

void compute_compressedfmt_aligned_width_and_height(int width, int height,
                                                    int format, int tile_mode,
                                                    int raster_mode,
                                                    int padding_threshold,
                                                    int *aligned_w, int *aligned_h,
                                                    int *bpp) {
    (void)format;
    (void)tile_mode;
    (void)raster_mode;
    (void)padding_threshold;
    if (aligned_w)
        *aligned_w = align_up(width, 64);
    if (aligned_h)
        *aligned_h = align_up(height, 32);
    if (bpp)
        *bpp = 4;
}

void compute_fmt_aligned_width_and_height(int width, int height, int plane_id,
                                          int format, unsigned int num_samples,
                                          int tile_mode, int raster_mode,
                                          int padding_threshold,
                                          int *aligned_w, int *aligned_h) {
    (void)plane_id;
    (void)format;
    (void)num_samples;
    (void)tile_mode;
    (void)raster_mode;
    (void)padding_threshold;
    if (aligned_w)
        *aligned_w = align_up(width, 64);
    if (aligned_h)
        *aligned_h = align_up(height, 32);
}

void link_adreno_compute_padding(void) {
}

/* gralloc1::AdrenoMemInfo::AlignUnCompressedRGB(int, int, int, int, unsigned int*, unsigned int*) */
void _ZN8gralloc113AdrenoMemInfo20AlignUnCompressedRGBEiiiiPjS1_(
    void *this_ptr, int width, int height, int format, int tile_enabled,
    unsigned int *aligned_w, unsigned int *aligned_h) {
    (void)this_ptr;
    (void)format;
    (void)tile_enabled;
    if (aligned_w)
        *aligned_w = (unsigned int)align_up(width, 64);
    if (aligned_h)
        *aligned_h = (unsigned int)align_up(height, 32);
}

/* gralloc1::AdrenoMemInfo::AlignCompressedRGB(int, int, int, unsigned int*, unsigned int*) */
void _ZN8gralloc113AdrenoMemInfo18AlignCompressedRGBEiiiPjS1_(
    void *this_ptr, int width, int height, int format,
    unsigned int *aligned_w, unsigned int *aligned_h) {
    (void)this_ptr;
    (void)format;
    if (aligned_w)
        *aligned_w = (unsigned int)align_up(width, 64);
    if (aligned_h)
        *aligned_h = (unsigned int)align_up(height, 32);
}

/* gralloc1::AdrenoMemInfo::IsUBWCSupportedByGPU(int) */
int _ZN8gralloc113AdrenoMemInfo20IsUBWCSupportedByGPUEi(void *this_ptr, int format) {
    (void)this_ptr;
    (void)format;
    return 0;
}

/* gralloc1::Allocator::IsUBwcEnabled */
int _ZN8gralloc19Allocator13IsUBwcEnabledEi25gralloc1_producer_usage_t25gralloc1_consumer_usage_t(
    void *this_ptr, int format, unsigned long prod, unsigned long cons) {
    (void)this_ptr;
    (void)format;
    (void)prod;
    (void)cons;
    return 0;
}

/* gralloc1::Allocator::IsUBwcSupported */
int _ZN8gralloc19Allocator15IsUBwcSupportedEi(void *this_ptr, int format) {
    (void)this_ptr;
    (void)format;
    return 0;
}

/* gralloc1::BufferManager::ValidateBufferSize */
int _ZN8gralloc113BufferManager18ValidateBufferSizeEPK16private_handle_tNS_16BufferDescriptorE(
    void *this_ptr, const void *hnd, void *desc) {
    (void)this_ptr;
    (void)hnd;
    (void)desc;
    return 0;
}

/* gralloc1::GrallocImpl::validateBufferSize */
int _ZN8gralloc111GrallocImpl18validateBufferSizeEP15gralloc1_devicePK13native_handlePK31gralloc1_buffer_descriptor_infoj(
    void *dev, const void *hnd, const void *info, unsigned int stride) {
    (void)dev;
    (void)hnd;
    (void)info;
    (void)stride;
    return 0;
}


