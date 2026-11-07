typedef unsigned int uint32_t;
typedef unsigned long uint64_t;
typedef unsigned long uintptr_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

struct timespec {
    long tv_sec;
    long tv_nsec;
};

static volatile int g_vsync_fd = -1;
static volatile char g_is_vsync_fd[1024];
static volatile int g_vsync_enabled = 1;
static volatile uint64_t g_last_vsync_ns = 0;

__attribute__((weak)) int *__errno(void);

static inline long sys_ret(long r) {
    if (r < 0 && r >= -4095) {
        if (__errno) *__errno() = (int)(-r);
        return -1;
    }
    return r;
}

static inline long sys_openat(int dfd, const char *filename, int flags, int mode) {
    register long x0 __asm__("x0") = dfd;
    register long x1 __asm__("x1") = (long)filename;
    register long x2 __asm__("x2") = flags;
    register long x3 __asm__("x3") = mode;
    register long x8 __asm__("x8") = 56;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys_read(int fd, void *buf, unsigned long count) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = count;
    register long x8 __asm__("x8") = 63;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys_close(int fd) {
    register long x0 __asm__("x0") = fd;
    register long x8 __asm__("x8") = 57;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys_write(int fd, const void *buf, unsigned long count) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = count;
    register long x8 __asm__("x8") = 64;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys_mprotect(void *addr, unsigned long len, int prot) {
    register long x0 __asm__("x0") = (long)addr;
    register long x1 __asm__("x1") = len;
    register long x2 __asm__("x2") = prot;
    register long x8 __asm__("x8") = 226;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys_ppoll(struct pollfd *fds, unsigned long nfds, const struct timespec *timeout, const void *sigmask, unsigned long sigsetsize) {
    register long x0 __asm__("x0") = (long)fds;
    register long x1 __asm__("x1") = (long)nfds;
    register long x2 __asm__("x2") = (long)timeout;
    register long x3 __asm__("x3") = (long)sigmask;
    register long x4 __asm__("x4") = (long)sigsetsize;
    register long x8 __asm__("x8") = 73;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys_clock_gettime(int clockid, struct timespec *tp) {
    register long x0 __asm__("x0") = (long)clockid;
    register long x1 __asm__("x1") = (long)tp;
    register long x8 __asm__("x8") = 113;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys_nanosleep(const struct timespec *req, struct timespec *rem) {
    register long x0 __asm__("x0") = (long)req;
    register long x1 __asm__("x1") = (long)rem;
    register long x8 __asm__("x8") = 101;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys_pread64(int fd, void *buf, unsigned long count, unsigned long offset) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = count;
    register long x3 __asm__("x3") = offset;
    register long x8 __asm__("x8") = 67;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory", "cc");
    return x0;
}

static inline void flush_icache(void *addr, unsigned long len) {
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + len;
    for (uintptr_t p = start & ~63UL; p < end; p += 64) {
        __asm__ __volatile__("dc cvau, %0" : : "r"(p) : "memory");
    }
    __asm__ __volatile__("dsb ish" ::: "memory");
    for (uintptr_t p = start & ~63UL; p < end; p += 64) {
        __asm__ __volatile__("ic ivau, %0" : : "r"(p) : "memory");
    }
    __asm__ __volatile__("dsb ish; isb" ::: "memory");
}

static uintptr_t parse_hex(const char **p) {
    uintptr_t val = 0;
    while (**p) {
        char c = **p;
        if (c >= '0' && c <= '9') val = (val << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f') val = (val << 4) | (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val = (val << 4) | (c - 'A' + 10);
        else break;
        (*p)++;
    }
    return val;
}

static int str_contains(const char *haystack, const char *needle) {
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (needle[j] && haystack[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static inline int is_vsync(int fd) {
    if (fd >= 0) {
        if (fd == g_vsync_fd) return 1;
        if (fd < 1024 && g_is_vsync_fd[fd]) return 1;
    }
    return 0;
}

static void klog(const char *msg, int len) {
    int fd = sys_openat(-100, "/dev/kmsg", 1, 0);
    if (fd >= 0) {
        sys_write(fd, msg, len);
        sys_close(fd);
    }
    sys_write(2, msg, len);
}

static char g_maps_buf[65536];

__attribute__((constructor)) void shim_init(void) {
    int fd = sys_openat(-100, "/proc/self/maps", 0, 0);
    if (fd < 0) return;

    long total = 0;
    while (total < (long)sizeof(g_maps_buf) - 1) {
        long n = sys_read(fd, g_maps_buf + total, sizeof(g_maps_buf) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    sys_close(fd);
    if (total <= 0) return;
    g_maps_buf[total] = '\0';

    char *line = g_maps_buf;
    while (line && *line) {
        char *next = 0;
        for (int i = 0; line[i]; i++) {
            if (line[i] == '\n') {
                line[i] = '\0';
                next = line + i + 1;
                break;
            }
        }

        if (str_contains(line, "r-xp") && str_contains(line, "surfaceflinger")) {
            const char *p = line;
            uintptr_t start = parse_hex(&p);
            if (*p == '-') p++;
            uintptr_t end = parse_hex(&p);

            for (uintptr_t addr = start; addr <= end - 16; addr += 4) {
                uint32_t w1 = *(volatile uint32_t *)addr;
                uint32_t w2 = *(volatile uint32_t *)(addr + 4);
                if ((w1 & 0xfffffc00) == 0x3941e400 && (w2 & 0xfff80000) == 0x36080000) {
                    uintptr_t page = (addr + 4) & ~4095UL;
                    sys_mprotect((void *)page, 8192, 7); // PROT_READ|PROT_WRITE|PROT_EXEC
                    *(volatile uint32_t *)(addr + 4) = 0xd503201f; // NOP
                    flush_icache((void *)(addr + 4), 4);
                    sys_mprotect((void *)page, 8192, 5); // PROT_READ|PROT_EXEC
                    klog("[+] libsdm_shim: Bypassed 'output buffer not gpu writeable' check!\n", 67);
                }
            }
        }
        line = next;
    }
}

int ioctl(int fd, unsigned long req, void *arg) {
    if ((req & 0xffff) == 0x6da6) {
        uint32_t *op = (uint32_t *)arg;
        if (op && *op == 2) {
            uint32_t *fps = (uint32_t *)((char *)arg + 8);
            *fps = 60;
            return 0;
        }
        return 0;
    }
    if ((req & 0xffff) == 0x6da0) {
        if (arg) {
            int en = (*(int *)arg != 0);
            if (en != g_vsync_enabled) {
                g_vsync_enabled = en;
                if (en) {
                    sys_write(2, "[+] libsdm_shim: VSYNC enabled\n", 31);
                } else {
                    sys_write(2, "[-] libsdm_shim: VSYNC disabled\n", 32);
                }
            }
        }
        return 0; // MSMFB_OVERLAY_VSYNC_CTRL
    }
    if ((req & 0xff00) == 0x6d00) {
        return 0; // Handle MSMFB_METADATA_SET (165), MSMFB_DISPLAY_COMMIT (164), etc.
    }
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = req;
    register long x2 __asm__("x2") = (long)arg;
    register long x8 __asm__("x8") = 29;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
    return (int)sys_ret(x0);
}

int openat(int dfd, const char *filename, int flags, int mode) {
    long ret = sys_openat(dfd, filename, flags, mode);
    if (ret >= 0 && filename && str_contains(filename, "vsync_event")) {
        g_vsync_fd = (int)ret;
        if (ret < 1024) g_is_vsync_fd[ret] = 1;
        sys_write(2, "[+] libsdm_shim: Tracked vsync_event fd\n", 40);
    }
    return (int)sys_ret(ret);
}

int open(const char *filename, int flags, int mode) {
    return openat(-100, filename, flags, mode);
}

int open64(const char *filename, int flags, int mode) {
    return openat(-100, filename, flags, mode);
}

int openat64(int dfd, const char *filename, int flags, int mode) {
    return openat(dfd, filename, flags, mode);
}

int close(int fd) {
    if (fd >= 0 && fd < 1024 && g_is_vsync_fd[fd]) {
        g_is_vsync_fd[fd] = 0;
        if (fd == g_vsync_fd) g_vsync_fd = -1;
    }
    return (int)sys_ret(sys_close(fd));
}

long pread(int fd, void *buf, unsigned long count, unsigned long offset) {
    if (is_vsync(fd) && buf && count > 0) {
        struct timespec now;
        sys_clock_gettime(1 /* CLOCK_MONOTONIC */, &now);
        uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;

        char str[64];
        int len = 0;
        str[len++] = 'V'; str[len++] = 'S'; str[len++] = 'Y';
        str[len++] = 'N'; str[len++] = 'C'; str[len++] = '=';

        char digits[24];
        int dlen = 0;
        uint64_t val = now_ns;
        if (val == 0) digits[dlen++] = '0';
        while (val > 0) {
            digits[dlen++] = '0' + (val % 10);
            val /= 10;
        }
        for (int i = dlen - 1; i >= 0; i--) {
            str[len++] = digits[i];
        }
        str[len++] = '\n';
        str[len] = '\0';

        static int pulse_count = 0;
        (void)pulse_count;

        if (count > (unsigned long)len) count = (unsigned long)len;
        char *dst = (char *)buf;
        for (unsigned long i = 0; i < count; i++) {
            dst[i] = str[i];
        }
        return (long)count;
    }
    return sys_ret(sys_pread64(fd, buf, count, offset));
}

long pread64(int fd, void *buf, unsigned long count, unsigned long offset) {
    return pread(fd, buf, count, offset);
}

long read(int fd, void *buf, unsigned long count) {
    if (is_vsync(fd) && buf && count > 0) {
        return pread(fd, buf, count, 0);
    }
    return sys_ret(sys_read(fd, buf, count));
}

int poll(struct pollfd *fds, unsigned long nfds, int timeout) {
    int vsync_idx = -1;
    for (unsigned long i = 0; i < nfds; i++) {
        if (fds[i].fd >= 0 && (is_vsync(fds[i].fd) || (g_vsync_fd < 0 && (fds[i].events & 0x0002)))) {
            vsync_idx = (int)i;
            if (g_vsync_fd < 0) {
                g_vsync_fd = fds[i].fd;
                if (fds[i].fd < 1024) g_is_vsync_fd[fds[i].fd] = 1;
            }
            break;
        }
    }

    if (vsync_idx < 0) {
        struct timespec ts;
        struct timespec *tsp = 0;
        if (timeout >= 0) {
            ts.tv_sec = timeout / 1000;
            ts.tv_nsec = (timeout % 1000) * 1000000L;
            tsp = &ts;
        }
        return (int)sys_ret(sys_ppoll(fds, nfds, tsp, 0, 8));
    }

    while (1) {
        if (!g_vsync_enabled) {
            struct timespec ts = {0, 50000000L}; // 50ms sleep
            sys_nanosleep(&ts, 0);
            if (timeout == 0) return 0;
            continue;
        }

        struct timespec now;
        sys_clock_gettime(1 /* CLOCK_MONOTONIC */, &now);
        uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;

        if (g_last_vsync_ns == 0) {
            g_last_vsync_ns = now_ns;
        }

        uint64_t target_ns = g_last_vsync_ns + 16666667ULL;
        if (now_ns < target_ns) {
            uint64_t diff_ns = target_ns - now_ns;
            struct timespec wait_ts;
            wait_ts.tv_sec = diff_ns / 1000000000ULL;
            wait_ts.tv_nsec = diff_ns % 1000000000ULL;
            sys_nanosleep(&wait_ts, 0);
        }

        sys_clock_gettime(1 /* CLOCK_MONOTONIC */, &now);
        now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
        if (now_ns > target_ns + 50000000ULL) {
            g_last_vsync_ns = now_ns;
        } else {
            g_last_vsync_ns = target_ns;
        }

        for (unsigned long i = 0; i < nfds; i++) {
            fds[i].revents = 0;
        }
        fds[vsync_idx].revents = 0x0002; // POLLPRI
        return 1;
    }
}

int ppoll(struct pollfd *fds, unsigned long nfds, const struct timespec *timeout, const void *sigmask) {
    int timeout_ms = -1;
    if (timeout) {
        timeout_ms = (int)(timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000);
    }
    return poll(fds, nfds, timeout_ms);
}

void _ZN3sdm18HWCBufferAllocator24GetAlignedWidthAndHeightEiiijPiS1_(void *this, int width, int height, int format, unsigned int usage, int *aligned_w, int *aligned_h) {
    (void)this;
    (void)format;
    (void)usage;
    if (aligned_w) *aligned_w = (width + 63) & ~63;
    if (aligned_h) *aligned_h = (height + 31) & ~31;
}

void compute_aligned_width_and_height(int width, int height, int bpp, int tile_mode, int raster_mode, int padding_threshold, int *aligned_w, int *aligned_h) {
    (void)bpp;
    (void)tile_mode;
    (void)raster_mode;
    (void)padding_threshold;
    if (aligned_w) *aligned_w = (width + 63) & ~63;
    if (aligned_h) *aligned_h = (height + 31) & ~31;
}

int isUBWCSupportedByGpu(unsigned int format) {
    (void)format;
    return 0;
}

void _ZN8gralloc113AdrenoMemInfo20AlignUnCompressedRGBEiiiiPjS1_(void *this, int width, int height, int format, int tile_enabled, unsigned int *aligned_w, unsigned int *aligned_h) {
    (void)this;
    (void)format;
    (void)tile_enabled;
    if (aligned_w) *aligned_w = (unsigned int)((width + 63) & ~63);
    if (aligned_h) *aligned_h = (unsigned int)((height + 31) & ~31);
}

int _ZN8gralloc113AdrenoMemInfo20IsUBWCSupportedByGPUEi(void *this, int format) {
    (void)this;
    (void)format;
    return 0;
}

int _ZN8gralloc19Allocator13IsUBwcEnabledEi25gralloc1_producer_usage_t25gralloc1_consumer_usage_t(void *this, int format, uint64_t prod, uint64_t cons) {
    (void)this;
    (void)format;
    (void)prod;
    (void)cons;
    return 0;
}

int _ZN8gralloc19Allocator15IsUBwcSupportedEi(void *this, int format) {
    (void)this;
    (void)format;
    return 0;
}

int _ZN8gralloc19BufferMgr18ValidateBufferSizeEPK16private_handle_tNS_10BufferInfoE(void *this, const void *hnd, void *info) {
    (void)this;
    (void)hnd;
    (void)info;
    return 0;
}

int _ZN8gralloc113BufferManager18ValidateBufferSizeEPK16private_handle_tNS_16BufferDescriptorE(void *this, const void *hnd, void *desc) {
    (void)this;
    (void)hnd;
    (void)desc;
    return 0;
}

int _ZN8gralloc111GrallocImpl18validateBufferSizeEP15gralloc1_devicePK13native_handlePK31gralloc1_buffer_descriptor_infoj(void *dev, const void *hnd, const void *info, uint32_t stride) {
    (void)dev;
    (void)hnd;
    (void)info;
    (void)stride;
    return 0;
}

int _ZNK7android14Gralloc2Mapper18validateBufferSizeEPK13native_handlejjijmj(void *this, const void *hnd, uint32_t w, uint32_t h, int fmt, uint32_t lc, uint64_t usage, uint32_t stride) {
    (void)this;
    (void)hnd;
    (void)w;
    (void)h;
    (void)fmt;
    (void)lc;
    (void)usage;
    (void)stride;
    return 0;
}

int _ZNK7android14Gralloc2Mapper18validateBufferSizeEPK13native_handlejjijyj(void *this, const void *hnd, uint32_t w, uint32_t h, int fmt, uint32_t lc, uint64_t usage, uint32_t stride) {
    (void)this;
    (void)hnd;
    (void)w;
    (void)h;
    (void)fmt;
    (void)lc;
    (void)usage;
    (void)stride;
    return 0;
}

int _ZNK7android14Gralloc2Mapper18validateBufferSizeEPK13native_handlejjjimj(void *this, const void *hnd, uint32_t w, uint32_t h, uint32_t lc, int fmt, uint64_t usage, uint32_t stride) {
    (void)this;
    (void)hnd;
    (void)w;
    (void)h;
    (void)lc;
    (void)fmt;
    (void)usage;
    (void)stride;
    return 0;
}

int _ZNK7android14Gralloc2Mapper18validateBufferSizeEPK13native_handlejjjiyj(void *this, const void *hnd, uint32_t w, uint32_t h, uint32_t lc, int fmt, uint64_t usage, uint32_t stride) {
    (void)this;
    (void)hnd;
    (void)w;
    (void)h;
    (void)lc;
    (void)fmt;
    (void)usage;
    (void)stride;
    return 0;
}

int android_get_device_api_level(void) {
    return 29;
}

int _ZNK7android14Gralloc2Mapper28validateBufferDescriptorInfoEPNS_8hardware8graphics6mapper4V2_17IMapper20BufferDescriptorInfoE(void *this, void *info) {
    (void)this;
    if (info) {
        uint64_t *usage = (uint64_t *)((char *)info + 0x10);
        *usage &= 0x0fffffffULL;
        /* Keep HW_RENDER (0x200) without adding SW_READ/SW_WRITE (0x33) so both
         * createDescriptor and validateBufferSize take the AdrenoMemInfo path in
         * gralloc.msm8996.so, while libadreno_utils.so disables UBWC/tiling. */
        *usage |= 0x200ULL;
    }
    return 0;
}

