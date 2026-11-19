typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long int64_t;
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

static volatile int g_is_composer = -1;
static volatile int g_patch_lock = 0;
static volatile int g_drm_lock = 0;
static volatile int g_msmfb_log_budget = 24;
static volatile int g_commit_log_budget = 10;
static volatile int g_vsync_log_budget = 4;
static volatile int g_drm_err_budget = 24;
static uintptr_t g_hwc_debug_base = 0;
static void (*g_hwc_debug_all_fn)(int enable, int verbose_level) = 0;
static int g_hwc_debug_refreshed = 0;

__attribute__((weak)) int *__errno(void);
__attribute__((weak)) int __android_log_print(int prio, const char *tag, const char *fmt, ...);
__attribute__((weak)) void *dlsym(void *handle, const char *symbol);
__attribute__((weak)) void *__loader_dlopen(const char *filename, int flag, const void *caller_addr);
__attribute__((weak)) void *__loader_android_dlopen_ext(const char *filename, int flag, const void *extinfo, const void *caller_addr);

static inline void shim_spin_lock(volatile int *lock) {
    unsigned int tmp, status;
    __asm__ volatile(
        "1: ldaxr %w0, [%2]\n"
        "   cbnz  %w0, 1b\n"
        "   mov   %w0, #1\n"
        "   stxr  %w1, %w0, [%2]\n"
        "   cbnz  %w1, 1b\n"
        : "=&r"(tmp), "=&r"(status)
        : "r"(lock)
        : "memory", "cc");
}

static inline void shim_spin_unlock(volatile int *lock) {
    __asm__ volatile("stlr wzr, [%0]" :: "r"(lock) : "memory");
}

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

static inline long sys_ioctl(int fd, unsigned long req, void *arg) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)req;
    register long x2 __asm__("x2") = (long)arg;
    register long x8 __asm__("x8") = 29;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys_fcntl(int fd, int cmd, long arg) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = cmd;
    register long x2 __asm__("x2") = arg;
    register long x8 __asm__("x8") = 25;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
    return x0;
}

struct shim_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t _pad[18];
};

static inline long sys_fstat(int fd, struct shim_stat *st) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)st;
    register long x8 __asm__("x8") = 80; /* __NR_fstat on arm64 */
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys_futex(volatile int *uaddr, int op, int val, const struct timespec *timeout) {
    register long x0 __asm__("x0") = (long)uaddr;
    register long x1 __asm__("x1") = op;
    register long x2 __asm__("x2") = val;
    register long x3 __asm__("x3") = (long)timeout;
    register long x4 __asm__("x4") = 0;
    register long x5 __asm__("x5") = 0;
    register long x8 __asm__("x8") = 98; /* __NR_futex on arm64 */
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory", "cc");
    return x0;
}

__attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))
static void shim_memset(void *dst, int val, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char b = (unsigned char)val;
    for (unsigned long i = 0; i < n; i++) d[i] = b;
}

__attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))
static void shim_memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
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
    if (!haystack || !needle) return 0;
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (needle[j] && haystack[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static int str_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static void log_str(const char *msg) {
    if (__android_log_print) {
        __android_log_print(4 /* ANDROID_LOG_INFO */, "libsdm_shim", "%s", msg);
    }
}

static int fmt_append_str(char *dst, int pos, int cap, const char *s) {
    while (s && *s && pos < cap - 1) dst[pos++] = *s++;
    dst[pos] = '\0';
    return pos;
}

static int fmt_append_hex(char *dst, int pos, int cap, uint64_t val) {
    static const char hex[] = "0123456789abcdef";
    char tmp[16];
    int n = 0;
    pos = fmt_append_str(dst, pos, cap, "0x");
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 16) {
        tmp[n++] = hex[val & 0xf];
        val >>= 4;
    }
    while (n > 0 && pos < cap - 1) dst[pos++] = tmp[--n];
    dst[pos] = '\0';
    return pos;
}

static int fmt_append_uint(char *dst, int pos, int cap, uint64_t val) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) {
        tmp[n++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (n > 0 && pos < cap - 1) dst[pos++] = tmp[--n];
    dst[pos] = '\0';
    return pos;
}

static int shim_is_composer(void) {
    int st = g_is_composer;
    if (st >= 0) return st;

    int fd = (int)sys_openat(-100, "/proc/self/cmdline", 0, 0);
    int found = 0;
    if (fd >= 0) {
        char cmd[256];
        long n = sys_read(fd, cmd, sizeof(cmd) - 1);
        sys_close(fd);
        if (n > 0) {
            for (long i = 0; i < n; i++) {
                if (cmd[i] == '\0') cmd[i] = ' ';
            }
            cmd[n] = '\0';
            if (str_contains(cmd, "composer") || str_contains(cmd, "hwcomposer")) {
                found = 1;
            }
        }
    }
    g_is_composer = found;
    return found;
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

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct {
    int64_t d_tag;
    uint64_t d_val;
} Elf64_Dyn;

typedef struct {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} Elf64_Rela;

_Static_assert(sizeof(Elf64_Ehdr) == 64, "Elf64_Ehdr size");
_Static_assert(sizeof(Elf64_Phdr) == 56, "Elf64_Phdr size");
_Static_assert(sizeof(Elf64_Dyn) == 16, "Elf64_Dyn size");
_Static_assert(sizeof(Elf64_Sym) == 24, "Elf64_Sym size");
_Static_assert(sizeof(Elf64_Rela) == 24, "Elf64_Rela size");

int ioctl(int fd, unsigned long req, void *arg);
int open(const char *filename, int flags, int mode);
int open64(const char *filename, int flags, int mode);
int openat(int dfd, const char *filename, int flags, int mode);
int openat64(int dfd, const char *filename, int flags, int mode);
int __open_2(const char *filename, int flags);
int close(int fd);
long pread(int fd, void *buf, unsigned long count, unsigned long offset);
long pread64(int fd, void *buf, unsigned long count, unsigned long offset);
long read(int fd, void *buf, unsigned long count);
int poll(struct pollfd *fds, unsigned long nfds, int timeout);
int ppoll(struct pollfd *fds, unsigned long nfds, const struct timespec *timeout, const void *sigmask);
void _ZN3sdm18HWCBufferAllocator24GetAlignedWidthAndHeightEiiijPiS1_(void *this, int width, int height, int format, unsigned int usage, int *aligned_w, int *aligned_h);

static inline uintptr_t elf_ptr(uintptr_t bias, uint64_t val) {
    return (val >= bias) ? (uintptr_t)val : (bias + (uintptr_t)val);
}

static int write_ptr_slot(uintptr_t slot_addr, void *val, int restore_ro) {
    if (*(void **)slot_addr == val) return 0;
    uintptr_t page = slot_addr & ~4095UL;
    if (restore_ro) {
        if (sys_mprotect((void *)page, 4096, 3 /* PROT_READ | PROT_WRITE */) < 0) {
            return 0;
        }
    }
    *(void **)slot_addr = val;
    if (restore_ro) {
        sys_mprotect((void *)page, 4096, 1 /* PROT_READ */);
    }
    return 1;
}

static int addr_needs_mprotect(const Elf64_Phdr *phdrs, uint16_t phnum, uintptr_t bias, uintptr_t addr, int *restore_ro) {
    uintptr_t relro_start = 0, relro_end = 0;
    int writable = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        if (phdrs[i].p_type == 1 /* PT_LOAD */) {
            uintptr_t seg_s = bias + phdrs[i].p_vaddr;
            uintptr_t seg_e = seg_s + phdrs[i].p_memsz;
            if (addr >= seg_s && addr < seg_e) {
                writable = (phdrs[i].p_flags & 2 /* PF_W */) != 0;
            }
        } else if (phdrs[i].p_type == 0x6474e552U /* PT_GNU_RELRO */) {
            relro_start = bias + phdrs[i].p_vaddr;
            relro_end = (relro_start + phdrs[i].p_memsz) & ~4095UL;
        }
    }
    if (addr >= relro_start && addr < relro_end) {
        *restore_ro = 1;
        return 1;
    }
    if (!writable) {
        *restore_ro = 1;
        return 1;
    }
    *restore_ro = 0;
    return 0;
}

static void *lookup_hook_fn(const char *name) {
    static const struct {
        const char *name;
        void *fn;
    } hooks[] = {
        { "ioctl", (void *)ioctl },
        { "open", (void *)open },
        { "open64", (void *)open64 },
        { "__open_2", (void *)__open_2 },
        { "openat", (void *)openat },
        { "openat64", (void *)openat64 },
        { "close", (void *)close },
        { "pread", (void *)pread },
        { "pread64", (void *)pread64 },
        { "read", (void *)read },
        { "poll", (void *)poll },
        { "ppoll", (void *)ppoll },
        { "_ZN3sdm18HWCBufferAllocator24GetAlignedWidthAndHeightEiiijPiS1_",
          (void *)_ZN3sdm18HWCBufferAllocator24GetAlignedWidthAndHeightEiiijPiS1_ },
    };
    for (unsigned long i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        if (str_eq(name, hooks[i].name)) return hooks[i].fn;
    }
    return 0;
}

static int patch_rela_table(const Elf64_Rela *rela, uint64_t bytes, const Elf64_Sym *dynsym,
                            const char *dynstr, uint32_t nsyms, const Elf64_Phdr *phdrs,
                            uint16_t phnum, uintptr_t bias) {
    if (!rela || !bytes) return 0;
    int patched = 0;
    uint64_t count = bytes / sizeof(Elf64_Rela);
    for (uint64_t i = 0; i < count; i++) {
        uint32_t rtype = (uint32_t)(rela[i].r_info & 0xffffffffU);
        uint32_t rsym = (uint32_t)(rela[i].r_info >> 32);
        if (rsym == 0 || rsym >= nsyms) continue;
        if (rtype != 1026 /* JUMP_SLOT */ && rtype != 1025 /* GLOB_DAT */ && rtype != 257 /* ABS64 */) continue;
        const char *sname = dynstr + dynsym[rsym].st_name;
        void *hook = lookup_hook_fn(sname);
        if (!hook) continue;
        uintptr_t slot = bias + rela[i].r_offset;
        int ro = 0;
        addr_needs_mprotect(phdrs, phnum, bias, slot, &ro);
        patched += write_ptr_slot(slot, hook, ro);
    }
    return patched;
}

static void patch_one_sdm_elf(uintptr_t base, const char *path, int *sys_patched, int *got_patched,
                              void (**out_debug_all)(int, int), uintptr_t *out_debug_base) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return;
    if (eh->e_ident[4] != 2 /* ELFCLASS64 */ || eh->e_machine != 183 /* EM_AARCH64 */) return;
    if (eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0 || eh->e_phnum > 64) return;

    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(base + eh->e_phoff);
    uintptr_t min_vaddr = (uintptr_t)-1;
    const Elf64_Phdr *dyn_ph = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (phdrs[i].p_type == 1 /* PT_LOAD */ && phdrs[i].p_vaddr < min_vaddr) {
            min_vaddr = phdrs[i].p_vaddr;
        } else if (phdrs[i].p_type == 2 /* PT_DYNAMIC */) {
            dyn_ph = &phdrs[i];
        }
    }
    if (min_vaddr == (uintptr_t)-1 || !dyn_ph) return;

    uintptr_t bias = base - min_vaddr;
    const Elf64_Dyn *dyn = (const Elf64_Dyn *)(bias + dyn_ph->p_vaddr);
    const char *dynstr = 0;
    const Elf64_Sym *dynsym = 0;
    const uint32_t *hash = 0;
    const uint32_t *gnu_hash = 0;
    const Elf64_Rela *jmprel = 0, *rela = 0;
    uint64_t pltrelsz = 0, relasz = 0;

    for (const Elf64_Dyn *d = dyn; d->d_tag != 0; d++) {
        if (d->d_tag == 5) dynstr = (const char *)elf_ptr(bias, d->d_val);
        else if (d->d_tag == 6) dynsym = (const Elf64_Sym *)elf_ptr(bias, d->d_val);
        else if (d->d_tag == 4) hash = (const uint32_t *)elf_ptr(bias, d->d_val);
        else if (d->d_tag == 0x6ffffef5) gnu_hash = (const uint32_t *)elf_ptr(bias, d->d_val);
        else if (d->d_tag == 23) jmprel = (const Elf64_Rela *)elf_ptr(bias, d->d_val);
        else if (d->d_tag == 2) pltrelsz = d->d_val;
        else if (d->d_tag == 7) rela = (const Elf64_Rela *)elf_ptr(bias, d->d_val);
        else if (d->d_tag == 8) relasz = d->d_val;
    }
    if (!dynstr || !dynsym) return;

    uint32_t nsyms = 0;
    if (hash) {
        nsyms = hash[1];
    } else if (gnu_hash) {
        uint32_t nbuckets = gnu_hash[0];
        uint32_t symoffset = gnu_hash[1];
        uint32_t bloom_size = gnu_hash[2];
        const uint64_t *bloom = (const uint64_t *)(gnu_hash + 4);
        const uint32_t *buckets = (const uint32_t *)(bloom + bloom_size);
        const uint32_t *chains = buckets + nbuckets;
        uint32_t max_sym = symoffset;
        for (uint32_t b = 0; b < nbuckets; b++) {
            if (buckets[b] > max_sym) max_sym = buckets[b];
        }
        if (max_sym >= symoffset) {
            while ((chains[max_sym - symoffset] & 1U) == 0 && (max_sym - symoffset) < 100000U) {
                max_sym++;
            }
            nsyms = max_sym + 1;
        } else {
            nsyms = symoffset;
        }
    }
    if (nsyms == 0 || nsyms > 100000U) return;

    if (str_contains(path, "/libsdmutils.so")) {
        static const struct {
            const char *sym_name;
            void *fn;
        } sys_ptrs[] = {
            { "_ZN3sdm3Sys6ioctl_E", (void *)ioctl },
            { "_ZN3sdm3Sys5open_E", (void *)open },
            { "_ZN3sdm3Sys6pread_E", (void *)pread },
            { "_ZN3sdm3Sys5poll_E", (void *)poll },
            { "_ZN3sdm3Sys6close_E", (void *)close },
            { "_ZN3sdm3Sys5read_E", (void *)read },
        };
        for (uint32_t i = 1; i < nsyms; i++) {
            if (dynsym[i].st_shndx == 0 || dynsym[i].st_value == 0) continue;
            const char *sname = dynstr + dynsym[i].st_name;
            for (unsigned long j = 0; j < sizeof(sys_ptrs) / sizeof(sys_ptrs[0]); j++) {
                if (str_eq(sname, sys_ptrs[j].sym_name)) {
                    uintptr_t slot = bias + dynsym[i].st_value;
                    int ro = 0;
                    addr_needs_mprotect(phdrs, eh->e_phnum, bias, slot, &ro);
                    *sys_patched += write_ptr_slot(slot, sys_ptrs[j].fn, ro);
                }
            }
        }
    }

    if (str_contains(path, "/hwcomposer.msm8996.so")) {
        for (uint32_t i = 1; i < nsyms; i++) {
            if (dynsym[i].st_shndx == 0 || dynsym[i].st_value == 0) continue;
            const char *sname = dynstr + dynsym[i].st_name;
            if (str_eq(sname, "_ZN3sdm15HWCDebugHandler8DebugAllEbi")) {
                *out_debug_all = (void (*)(int, int))(bias + dynsym[i].st_value);
                *out_debug_base = base;
                break;
            }
        }
    }

    *got_patched += patch_rela_table(jmprel, pltrelsz, dynsym, dynstr, nsyms, phdrs, eh->e_phnum, bias);
    *got_patched += patch_rela_table(rela, relasz, dynsym, dynstr, nsyms, phdrs, eh->e_phnum, bias);
}

static char g_maps_buf[65536];

static int sdm_debug_requested(void) {
    int fd = (int)sys_openat(-100, "/data/local/tmp/sdm_debug", 0, 0);
    if (fd >= 0) {
        sys_close(fd);
        return 1;
    }
    return 0;
}

static void patch_sdm_loaded_libs(void) {
    if (!shim_is_composer()) return;

    int sys_patched = 0, got_patched = 0;
    void (*found_debug_all)(int, int) = 0;
    uintptr_t found_debug_base = 0;

    shim_spin_lock(&g_patch_lock);
    int fd = (int)sys_openat(-100, "/proc/self/maps", 0, 0);
    if (fd >= 0) {
        long total = 0;
        while (total < (long)sizeof(g_maps_buf) - 1) {
            long n = sys_read(fd, g_maps_buf + total, sizeof(g_maps_buf) - 1 - total);
            if (n <= 0) break;
            total += n;
        }
        sys_close(fd);
        if (total > 0) {
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
                if ((str_contains(line, " r--p 00000000 ") || str_contains(line, " r-xp 00000000 ")) &&
                    (str_contains(line, "/libsdmutils.so") ||
                     str_contains(line, "/hwcomposer.msm8996.so") ||
                     str_contains(line, "/libsdmcore.so") ||
                     str_contains(line, "/libsdmextension.so") ||
                     str_contains(line, "/libqdutils.so"))) {
                    const char *p = line;
                    uintptr_t base = parse_hex(&p);
                    if (base != 0) {
                        patch_one_sdm_elf(base, line, &sys_patched, &got_patched,
                                          &found_debug_all, &found_debug_base);
                    }
                }
                line = next;
            }
        }
    }
    shim_spin_unlock(&g_patch_lock);

    if (found_debug_all) {
        g_hwc_debug_all_fn = found_debug_all;
        if (g_hwc_debug_base != found_debug_base) {
            g_hwc_debug_base = found_debug_base;
            if (sdm_debug_requested()) {
                g_hwc_debug_all_fn(1, 3);
                g_hwc_debug_refreshed = 1;
                log_str("Enabled sdm::HWCDebugHandler::DebugAll(1, 3)");
            }
        }
    }
    if (sys_patched || got_patched) {
        char msg[128];
        int p = 0;
        p = fmt_append_str(msg, p, sizeof(msg), "Patched SDM libs: sys_ptrs=");
        p = fmt_append_uint(msg, p, sizeof(msg), (uint64_t)sys_patched);
        p = fmt_append_str(msg, p, sizeof(msg), " got_slots=");
        p = fmt_append_uint(msg, p, sizeof(msg), (uint64_t)got_patched);
        log_str(msg);
    }
}

__attribute__((constructor)) void shim_init(void) {
    if (shim_is_composer()) {
        log_str("Loaded in composer process; waiting for SDM libs");
        patch_sdm_loaded_libs();
        return;
    }

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

/* DRM/KMS & MSMFB_ATOMIC_COMMIT support for Linux 6.18 MDP5 (/dev/dri/card1) */

#define SHIM_O_RDWR    2
#define SHIM_O_CLOEXEC 02000000
#define SHIM_F_DUPFD_CLOEXEC 1030

#define SHIM_IOC(dir, type, nr, sz) \
    (((unsigned long)(dir) << 30) | ((unsigned long)(type) << 8) | \
     (unsigned long)(nr) | ((unsigned long)(sz) << 16))
#define SHIM_IOW(type, nr, st)  SHIM_IOC(1, (type), (nr), sizeof(st))
#define SHIM_IOWR(type, nr, st) SHIM_IOC(3, (type), (nr), sizeof(st))

#define SHIM_DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define SHIM_DRM_CLIENT_CAP_ATOMIC           3

#define SHIM_DRM_MODE_OBJECT_CRTC      0xccccccccU
#define SHIM_DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0U
#define SHIM_DRM_MODE_OBJECT_PLANE     0xeeeeeeeeU

#define SHIM_DRM_MODE_ATOMIC_NONBLOCK      0x0200U
#define SHIM_DRM_MODE_ATOMIC_ALLOW_MODESET 0x0400U

#define SHIM_DRM_MODE_ROTATE_0  (1U << 0)
#define SHIM_DRM_MODE_REFLECT_X (1U << 4)
#define SHIM_DRM_MODE_REFLECT_Y (1U << 5)

#define SHIM_DRM_MODE_BLEND_PREMULTI   0ULL
#define SHIM_DRM_MODE_BLEND_COVERAGE   1ULL
#define SHIM_DRM_MODE_BLEND_PIXEL_NONE 2ULL

#define SHIM_DRM_PLANE_TYPE_OVERLAY 0ULL
#define SHIM_DRM_PLANE_TYPE_PRIMARY 1ULL
#define SHIM_DRM_PLANE_TYPE_CURSOR  2ULL

#define SHIM_DRM_SYNCOBJ_CREATE_SIGNALED (1U << 0)
#define SHIM_DRM_SYNCOBJ_EXPORT_SYNC_FILE (1U << 0)

#define fourcc_u32(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

struct drm_set_client_cap { uint64_t capability; uint64_t value; };
struct drm_prime_handle { uint32_t handle; uint32_t flags; int fd; };
struct drm_gem_close { uint32_t handle; uint32_t pad; };
struct drm_mode_closefb { uint32_t fb_id; uint32_t pad; };
struct drm_syncobj_create { uint32_t handle; uint32_t flags; };
struct drm_syncobj_destroy { uint32_t handle; uint32_t pad; };
struct drm_syncobj_handle { uint32_t handle; uint32_t flags; int fd; uint32_t pad; uint64_t point; };

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

struct drm_mode_create_blob { uint64_t data; uint32_t length; uint32_t blob_id; };

struct drm_mode_atomic {
    uint32_t flags, count_objs;
    uint64_t objs_ptr, count_props_ptr, props_ptr, prop_values_ptr;
    uint64_t reserved, user_data;
};

_Static_assert(sizeof(struct drm_mode_modeinfo) == 68, "drm_mode_modeinfo size");
_Static_assert(sizeof(struct drm_mode_fb_cmd2) == 104, "drm_mode_fb_cmd2 size");
_Static_assert(sizeof(struct drm_mode_atomic) == 56, "drm_mode_atomic size");

#define SHIM_DRM_IOCTL_GEM_CLOSE             SHIM_IOW('d', 0x09, struct drm_gem_close)
#define SHIM_DRM_IOCTL_SET_CLIENT_CAP        SHIM_IOW('d', 0x0d, struct drm_set_client_cap)
#define SHIM_DRM_IOCTL_PRIME_FD_TO_HANDLE    SHIM_IOWR('d', 0x2e, struct drm_prime_handle)
#define SHIM_DRM_IOCTL_MODE_GETRESOURCES     SHIM_IOWR('d', 0xA0, struct drm_mode_card_res)
#define SHIM_DRM_IOCTL_MODE_GETCRTC          SHIM_IOWR('d', 0xA1, struct drm_mode_crtc)
#define SHIM_DRM_IOCTL_MODE_GETENCODER       SHIM_IOWR('d', 0xA6, struct drm_mode_get_encoder)
#define SHIM_DRM_IOCTL_MODE_GETCONNECTOR     SHIM_IOWR('d', 0xA7, struct drm_mode_get_connector)
#define SHIM_DRM_IOCTL_MODE_GETPROPERTY      SHIM_IOWR('d', 0xAA, struct drm_mode_get_property)
#define SHIM_DRM_IOCTL_MODE_GETPLANERESOURCES SHIM_IOWR('d', 0xB5, struct drm_mode_get_plane_res)
#define SHIM_DRM_IOCTL_MODE_GETPLANE         SHIM_IOWR('d', 0xB6, struct drm_mode_get_plane)
#define SHIM_DRM_IOCTL_MODE_ADDFB2           SHIM_IOWR('d', 0xB8, struct drm_mode_fb_cmd2)
#define SHIM_DRM_IOCTL_MODE_OBJ_GETPROPERTIES SHIM_IOWR('d', 0xB9, struct drm_mode_obj_get_properties)
#define SHIM_DRM_IOCTL_MODE_ATOMIC           SHIM_IOWR('d', 0xBC, struct drm_mode_atomic)
#define SHIM_DRM_IOCTL_MODE_CREATEPROPBLOB   SHIM_IOWR('d', 0xBD, struct drm_mode_create_blob)
#define SHIM_DRM_IOCTL_SYNCOBJ_CREATE        SHIM_IOWR('d', 0xBF, struct drm_syncobj_create)
#define SHIM_DRM_IOCTL_SYNCOBJ_DESTROY       SHIM_IOWR('d', 0xC0, struct drm_syncobj_destroy)
#define SHIM_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD  SHIM_IOWR('d', 0xC1, struct drm_syncobj_handle)
#define SHIM_DRM_IOCTL_MODE_CLOSEFB          SHIM_IOWR('d', 0xD0, struct drm_mode_closefb)

#define SHIM_MAX_DRM_PLANES 16

struct shim_drm_plane {
    uint32_t id;
    uint32_t type;
    uint32_t prop_fb_id;
    uint32_t prop_crtc_id;
    uint32_t prop_src_x;
    uint32_t prop_src_y;
    uint32_t prop_src_w;
    uint32_t prop_src_h;
    uint32_t prop_crtc_x;
    uint32_t prop_crtc_y;
    uint32_t prop_crtc_w;
    uint32_t prop_crtc_h;
    uint32_t prop_zpos;
    uint32_t prop_alpha;
    uint32_t prop_blend_mode;
    uint32_t prop_rotation;
    uint32_t prop_in_fence_fd;
};

#define SHIM_FB_CACHE_SIZE 24
#define SHIM_FB_CACHE_TTL  120ULL

struct shim_fb_cache_entry {
    int in_use;
    uint64_t dev;
    uint64_t ino;
    uint32_t w;
    uint32_t h;
    uint32_t fourcc;
    uint32_t pitch;
    uint32_t offset;
    uint32_t gem_handle;
    uint32_t fb_id;
    uint64_t last_seq;
};

static struct {
    int card_fd;
    int init_failed;
    int modeset_done;
    int crtc_active;
    int prev_out_fence;
    uint32_t conn_id;
    uint32_t prop_conn_crtc_id;
    uint32_t crtc_id;
    uint32_t prop_crtc_active;
    uint32_t prop_crtc_mode_id;
    uint32_t prop_crtc_out_fence_ptr;
    uint32_t mode_blob_id;
    uint32_t display_w;
    uint32_t display_h;
    uint32_t num_planes;
    uint32_t prev_used_planes;
    uint64_t commit_seq;
    struct shim_drm_plane planes[SHIM_MAX_DRM_PLANES];
    struct shim_fb_cache_entry fb_cache[SHIM_FB_CACHE_SIZE];
} g_drm = { .card_fd = -1, .crtc_active = 1, .prev_out_fence = -1, .display_w = 1080, .display_h = 1920 };

static void log_err_code(const char *prefix, long r) {
    if (g_drm_err_budget <= 0) return;
    g_drm_err_budget--;
    char msg[160];
    int p = 0;
    p = fmt_append_str(msg, p, sizeof(msg), prefix);
    p = fmt_append_str(msg, p, sizeof(msg), " err=");
    if (r < 0) {
        p = fmt_append_str(msg, p, sizeof(msg), "-");
        p = fmt_append_uint(msg, p, sizeof(msg), (uint64_t)(-r));
    } else {
        p = fmt_append_uint(msg, p, sizeof(msg), (uint64_t)r);
    }
    log_str(msg);
}

static uint32_t mdp_fmt_to_drm_fourcc(uint32_t mdp_fmt, uint32_t *out_cpp) {
    uint32_t fourcc = 0;
    uint32_t cpp = 4;
    switch (mdp_fmt) {
    case 13: fourcc = fourcc_u32('A', 'B', '2', '4'); break; /* MDP_RGBA_8888 -> DRM_FORMAT_ABGR8888 */
    case 15: fourcc = fourcc_u32('X', 'B', '2', '4'); break; /* MDP_RGBX_8888 -> DRM_FORMAT_XBGR8888 */
    case 14: fourcc = fourcc_u32('A', 'R', '2', '4'); break; /* MDP_BGRA_8888 -> DRM_FORMAT_ARGB8888 */
    case 28: fourcc = fourcc_u32('X', 'R', '2', '4'); break; /* MDP_BGRX_8888 -> DRM_FORMAT_XRGB8888 */
    case 4:  fourcc = fourcc_u32('R', 'A', '2', '4'); break; /* MDP_ARGB_8888 -> DRM_FORMAT_RGBA8888 */
    case 1:  fourcc = fourcc_u32('R', 'X', '2', '4'); break; /* MDP_XRGB_8888 -> DRM_FORMAT_RGBX8888 */
    case 0:  fourcc = fourcc_u32('R', 'G', '1', '6'); cpp = 2; break;
    case 25: fourcc = fourcc_u32('B', 'G', '1', '6'); cpp = 2; break;
    case 5:  fourcc = fourcc_u32('B', 'G', '2', '4'); cpp = 3; break;
    case 26: fourcc = fourcc_u32('R', 'G', '2', '4'); cpp = 3; break;
    default: return 0;
    }
    if (out_cpp) *out_cpp = cpp;
    return fourcc;
}

static uint32_t drm_find_prop(int fd, uint32_t obj_id, uint32_t obj_type,
                              const char *prop_name, uint64_t *out_val) {
    struct drm_mode_obj_get_properties gp;
    uint32_t ids[32];
    uint64_t vals[32];
    shim_memset(&gp, 0, sizeof(gp));
    gp.obj_id = obj_id;
    gp.obj_type = obj_type;
    if (sys_ioctl(fd, SHIM_DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0 || gp.count_props == 0) {
        return 0;
    }
    if (gp.count_props > 32) gp.count_props = 32;
    gp.props_ptr = (uint64_t)(uintptr_t)ids;
    gp.prop_values_ptr = (uint64_t)(uintptr_t)vals;
    if (sys_ioctl(fd, SHIM_DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0) return 0;

    for (uint32_t i = 0; i < gp.count_props; i++) {
        struct drm_mode_get_property prop;
        shim_memset(&prop, 0, sizeof(prop));
        prop.prop_id = ids[i];
        if (sys_ioctl(fd, SHIM_DRM_IOCTL_MODE_GETPROPERTY, &prop) == 0 &&
            str_eq(prop.name, prop_name)) {
            if (out_val) *out_val = vals[i];
            return ids[i];
        }
    }
    return 0;
}

static int drm_ensure_init_locked(void) {
    if (g_drm.card_fd >= 0) return 0;
    if (g_drm.init_failed) return -1;

    static const char *const cards[] = { "/dev/dri/card1", "/dev/dri/card0", "/dev/dri/card2" };
    int fd = -1;
    uint32_t crtcs[16], conns[16], encs[16], plane_ids[32];
    struct drm_mode_card_res res;

    for (unsigned long i = 0; i < sizeof(cards) / sizeof(cards[0]); i++) {
        int cand = (int)sys_openat(-100, cards[i], SHIM_O_RDWR | SHIM_O_CLOEXEC, 0);
        if (cand < 0) continue;

        struct drm_set_client_cap cap = { SHIM_DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
        if (sys_ioctl(cand, SHIM_DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) {
            sys_close(cand);
            continue;
        }
        cap.capability = SHIM_DRM_CLIENT_CAP_ATOMIC;
        cap.value = 1;
        if (sys_ioctl(cand, SHIM_DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) {
            sys_close(cand);
            continue;
        }
        shim_memset(&res, 0, sizeof(res));
        if (sys_ioctl(cand, SHIM_DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 &&
            res.count_crtcs > 0 && res.count_connectors > 0) {
            fd = cand;
            break;
        }
        sys_close(cand);
    }

    if (fd < 0) {
        log_err_code("DRM card open/cap failed", -1);
        g_drm.init_failed = 1;
        return -1;
    }

    if (res.count_crtcs > 16) res.count_crtcs = 16;
    if (res.count_connectors > 16) res.count_connectors = 16;
    if (res.count_encoders > 16) res.count_encoders = 16;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encs;
    res.count_fbs = 0;
    if (sys_ioctl(fd, SHIM_DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || res.count_crtcs == 0) {
        sys_close(fd);
        g_drm.init_failed = 1;
        return -1;
    }

    struct drm_mode_modeinfo modes[16];
    uint32_t conn_encs[16], conn_props[32];
    uint64_t conn_prop_vals[32];
    struct drm_mode_get_connector conn;
    uint32_t chosen_conn = 0, chosen_enc = 0;
    struct drm_mode_modeinfo chosen_mode;
    int have_mode = 0;

    for (uint32_t i = 0; i < res.count_connectors; i++) {
        shim_memset(&conn, 0, sizeof(conn));
        conn.connector_id = conns[i];
        if (sys_ioctl(fd, SHIM_DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        if (conn.count_modes > 16) conn.count_modes = 16;
        if (conn.count_encoders > 16) conn.count_encoders = 16;
        if (conn.count_props > 32) conn.count_props = 32;
        conn.modes_ptr = (uint64_t)(uintptr_t)modes;
        conn.encoders_ptr = (uint64_t)(uintptr_t)conn_encs;
        conn.props_ptr = (uint64_t)(uintptr_t)conn_props;
        conn.prop_values_ptr = (uint64_t)(uintptr_t)conn_prop_vals;
        if (sys_ioctl(fd, SHIM_DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;

        if ((conn.connection == 1 /* DRM_MODE_CONNECTED */ || chosen_conn == 0) && conn.count_modes > 0) {
            chosen_conn = conn.connector_id;
            chosen_enc = conn.encoder_id ? conn.encoder_id : (conn.count_encoders > 0 ? conn_encs[0] : 0);
            shim_memcpy(&chosen_mode, &modes[0], sizeof(chosen_mode));
            have_mode = 1;
            if (conn.connection == 1) break;
        }
    }

    uint32_t crtc_idx = 0;
    uint32_t chosen_crtc = crtcs[0];
    if (chosen_enc) {
        struct drm_mode_get_encoder enc;
        shim_memset(&enc, 0, sizeof(enc));
        enc.encoder_id = chosen_enc;
        if (sys_ioctl(fd, SHIM_DRM_IOCTL_MODE_GETENCODER, &enc) == 0) {
            for (uint32_t i = 0; i < res.count_crtcs; i++) {
                if (enc.crtc_id && crtcs[i] == enc.crtc_id) {
                    chosen_crtc = crtcs[i];
                    crtc_idx = i;
                    break;
                }
                if (!enc.crtc_id && (enc.possible_crtcs & (1U << i))) {
                    chosen_crtc = crtcs[i];
                    crtc_idx = i;
                    break;
                }
            }
        }
    }

    struct drm_mode_crtc gcrtc;
    shim_memset(&gcrtc, 0, sizeof(gcrtc));
    gcrtc.crtc_id = chosen_crtc;
    if (sys_ioctl(fd, SHIM_DRM_IOCTL_MODE_GETCRTC, &gcrtc) == 0 && gcrtc.mode_valid) {
        shim_memcpy(&chosen_mode, &gcrtc.mode, sizeof(chosen_mode));
        have_mode = 1;
    }

    if (!chosen_conn || !chosen_crtc || !have_mode) {
        sys_close(fd);
        g_drm.init_failed = 1;
        log_str("DRM missing connector/crtc/mode");
        return -1;
    }

    struct drm_mode_create_blob blob;
    shim_memset(&blob, 0, sizeof(blob));
    blob.data = (uint64_t)(uintptr_t)&chosen_mode;
    blob.length = sizeof(chosen_mode);
    if (sys_ioctl(fd, SHIM_DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) < 0 || blob.blob_id == 0) {
        sys_close(fd);
        g_drm.init_failed = 1;
        log_str("DRM CREATEPROPBLOB failed");
        return -1;
    }

    g_drm.conn_id = chosen_conn;
    g_drm.prop_conn_crtc_id = drm_find_prop(fd, chosen_conn, SHIM_DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", 0);
    g_drm.crtc_id = chosen_crtc;
    g_drm.prop_crtc_active = drm_find_prop(fd, chosen_crtc, SHIM_DRM_MODE_OBJECT_CRTC, "ACTIVE", 0);
    g_drm.prop_crtc_mode_id = drm_find_prop(fd, chosen_crtc, SHIM_DRM_MODE_OBJECT_CRTC, "MODE_ID", 0);
    g_drm.prop_crtc_out_fence_ptr = drm_find_prop(fd, chosen_crtc, SHIM_DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR", 0);
    g_drm.mode_blob_id = blob.blob_id;
    g_drm.display_w = chosen_mode.hdisplay ? chosen_mode.hdisplay : 1080;
    g_drm.display_h = chosen_mode.vdisplay ? chosen_mode.vdisplay : 1920;

    struct drm_mode_get_plane_res pres;
    shim_memset(&pres, 0, sizeof(pres));
    if (sys_ioctl(fd, SHIM_DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) == 0 && pres.count_planes > 0) {
        if (pres.count_planes > 32) pres.count_planes = 32;
        pres.plane_id_ptr = (uint64_t)(uintptr_t)plane_ids;
        if (sys_ioctl(fd, SHIM_DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) == 0) {
            uint32_t n = 0;
            for (uint32_t i = 0; i < pres.count_planes && n < SHIM_MAX_DRM_PLANES; i++) {
                struct drm_mode_get_plane gp;
                shim_memset(&gp, 0, sizeof(gp));
                gp.plane_id = plane_ids[i];
                if (sys_ioctl(fd, SHIM_DRM_IOCTL_MODE_GETPLANE, &gp) < 0) continue;
                if (!(gp.possible_crtcs & (1U << crtc_idx))) continue;

                uint64_t ptype = SHIM_DRM_PLANE_TYPE_OVERLAY;
                drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "type", &ptype);
                if (ptype == SHIM_DRM_PLANE_TYPE_CURSOR) continue;

                struct shim_drm_plane *p = &g_drm.planes[n];
                shim_memset(p, 0, sizeof(*p));
                p->id = gp.plane_id;
                p->type = (uint32_t)ptype;
                p->prop_fb_id = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "FB_ID", 0);
                p->prop_crtc_id = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0);
                p->prop_src_x = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "SRC_X", 0);
                p->prop_src_y = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
                p->prop_src_w = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "SRC_W", 0);
                p->prop_src_h = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "SRC_H", 0);
                p->prop_crtc_x = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "CRTC_X", 0);
                p->prop_crtc_y = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
                p->prop_crtc_w = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "CRTC_W", 0);
                p->prop_crtc_h = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "CRTC_H", 0);
                p->prop_zpos = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "zpos", 0);
                p->prop_alpha = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "alpha", 0);
                p->prop_blend_mode = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "pixel blend mode", 0);
                p->prop_rotation = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "rotation", 0);
                p->prop_in_fence_fd = drm_find_prop(fd, gp.plane_id, SHIM_DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD", 0);
                n++;
            }
            /* Keep PRIMARY plane first at slot 0 */
            for (uint32_t i = 1; i < n; i++) {
                if (g_drm.planes[i].type == SHIM_DRM_PLANE_TYPE_PRIMARY &&
                    g_drm.planes[0].type != SHIM_DRM_PLANE_TYPE_PRIMARY) {
                    struct shim_drm_plane tmp;
                    shim_memcpy(&tmp, &g_drm.planes[0], sizeof(tmp));
                    shim_memcpy(&g_drm.planes[0], &g_drm.planes[i], sizeof(tmp));
                    shim_memcpy(&g_drm.planes[i], &tmp, sizeof(tmp));
                    break;
                }
            }
            g_drm.num_planes = n;
        }
    }

    if (g_drm.num_planes == 0 || !g_drm.prop_crtc_out_fence_ptr || !g_drm.prop_crtc_active) {
        sys_close(fd);
        g_drm.init_failed = 1;
        log_str("DRM missing planes or atomic properties");
        return -1;
    }

    g_drm.card_fd = fd;
    g_drm.prev_used_planes = g_drm.num_planes;

    char msg[128];
    int p = 0;
    p = fmt_append_str(msg, p, sizeof(msg), "DRM initialized crtc=");
    p = fmt_append_uint(msg, p, sizeof(msg), g_drm.crtc_id);
    p = fmt_append_str(msg, p, sizeof(msg), " planes=");
    p = fmt_append_uint(msg, p, sizeof(msg), g_drm.num_planes);
    p = fmt_append_str(msg, p, sizeof(msg), " res=");
    p = fmt_append_uint(msg, p, sizeof(msg), g_drm.display_w);
    p = fmt_append_str(msg, p, sizeof(msg), "x");
    p = fmt_append_uint(msg, p, sizeof(msg), g_drm.display_h);
    log_str(msg);
    return 0;
}

static int drm_create_signaled_fence_locked(int fd) {
    struct drm_syncobj_create sc = { 0, SHIM_DRM_SYNCOBJ_CREATE_SIGNALED };
    if (sys_ioctl(fd, SHIM_DRM_IOCTL_SYNCOBJ_CREATE, &sc) < 0) return -1;
    struct drm_syncobj_handle sh = { sc.handle, SHIM_DRM_SYNCOBJ_EXPORT_SYNC_FILE, -1, 0, 0 };
    int ret_fd = -1;
    if (sys_ioctl(fd, SHIM_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &sh) == 0) {
        ret_fd = sh.fd;
    }
    struct drm_syncobj_destroy sd = { sc.handle, 0 };
    sys_ioctl(fd, SHIM_DRM_IOCTL_SYNCOBJ_DESTROY, &sd);
    return ret_fd;
}

/*
 * MSMFB_ATOMIC_COMMIT ABI verified from libsdmcore.so (0x2c1e8, 0x2d13c, 0x2d2ac):
 *   mdp_layer_commit +0x08: flags (bit 0 = validate_only)
 *   mdp_layer_commit +0x0c: int32 release_fence (out)
 *   mdp_layer_commit +0x30: mdp_input_layer *input_layers
 *   mdp_layer_commit +0x38: uint32 input_layer_cnt
 *   mdp_layer_commit +0x48: int32 retire_fence (out)
 *   mdp_input_layer stride = 0xc0 (192 bytes)
 */
#define MDP_COMMIT_VALIDATE_FLAG 0x00000001U
#define MDP_LAYER_STRIDE         0xc0U
#define MDP_LAYER_SOLID_FILL     0x00000020U
#define MDP_LAYER_SECURE         0x00000010U
#define MDP_LAYER_SECURE_DISP    0x00000400U

struct shim_mdp_rect { uint32_t x, y, w, h; };
struct shim_mdp_plane { int fd; uint32_t offset; uint32_t stride; };

struct shim_atomic_req_buf {
    uint32_t objs[24];
    uint32_t cnts[24];
    uint32_t prop_ids[320];
    uint64_t prop_vals[320];
    uint32_t n_objs;
    uint32_t n_props;
};

static inline void atomic_begin_obj(struct shim_atomic_req_buf *ab, uint32_t obj_id) {
    ab->objs[ab->n_objs] = obj_id;
    ab->cnts[ab->n_objs] = 0;
}

static inline void atomic_add_prop(struct shim_atomic_req_buf *ab, uint32_t prop_id, uint64_t val) {
    if (!prop_id) return;
    ab->prop_ids[ab->n_props] = prop_id;
    ab->prop_vals[ab->n_props] = val;
    ab->n_props++;
    ab->cnts[ab->n_objs]++;
}

static inline void atomic_end_obj(struct shim_atomic_req_buf *ab) {
    if (ab->cnts[ab->n_objs] > 0) ab->n_objs++;
}

static int msmfb_atomic_commit_validate(const uint8_t *commit_ptr) {
    const uint8_t *layers = *(const uint8_t *const *)(commit_ptr + 0x30);
    uint32_t cnt = *(const uint32_t *)(commit_ptr + 0x38);
    uint32_t max_w = g_drm.display_w;
    uint32_t max_h = g_drm.display_h;

    if (cnt > 7 || (cnt > 0 && !layers)) return -22; /* -EINVAL */

    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *l = layers + i * MDP_LAYER_STRIDE;
        uint32_t lflags = *(const uint32_t *)(l + 0x00);
        const struct shim_mdp_rect *src = (const struct shim_mdp_rect *)(l + 0x20);
        const struct shim_mdp_rect *dst = (const struct shim_mdp_rect *)(l + 0x30);
        uint32_t bw = *(const uint32_t *)(l + 0x48);
        uint32_t bh = *(const uint32_t *)(l + 0x4c);
        uint32_t bfmt = *(const uint32_t *)(l + 0x50);

        if (lflags & (MDP_LAYER_SOLID_FILL | MDP_LAYER_SECURE | MDP_LAYER_SECURE_DISP)) return -22;
        if (!mdp_fmt_to_drm_fourcc(bfmt, 0)) return -22;
        if (src->w == 0 || src->h == 0 || dst->w == 0 || dst->h == 0) return -22;
        if (src->x + src->w > bw || src->y + src->h > bh) return -22;
        if (dst->x + dst->w > max_w || dst->y + dst->h > max_h) return -22;
        if (src->w * 8U < dst->w || dst->w * 8U < src->w) return -34;
        if (src->h * 8U < dst->h || dst->h * 8U < src->h) return -34;
    }
    return 0;
}

static int drm_gem_handle_in_cache_locked(uint32_t handle) {
    if (!handle) return 0;
    for (int i = 0; i < SHIM_FB_CACHE_SIZE; i++) {
        if (g_drm.fb_cache[i].in_use && g_drm.fb_cache[i].gem_handle == handle) {
            return 1;
        }
    }
    return 0;
}

static void drm_fb_cache_evict_slot_locked(int idx) {
    struct shim_fb_cache_entry *e = &g_drm.fb_cache[idx];
    if (!e->in_use) return;
    uint32_t fb_id = e->fb_id;
    uint32_t gem_handle = e->gem_handle;
    e->in_use = 0;
    e->fb_id = 0;
    e->gem_handle = 0;
    if (g_drm.card_fd >= 0 && fb_id) {
        struct drm_mode_closefb cf;
        cf.fb_id = fb_id;
        cf.pad = 0;
        sys_ioctl(g_drm.card_fd, SHIM_DRM_IOCTL_MODE_CLOSEFB, &cf);
    }
    if (g_drm.card_fd >= 0 && gem_handle && !drm_gem_handle_in_cache_locked(gem_handle)) {
        struct drm_gem_close gc;
        gc.handle = gem_handle;
        gc.pad = 0;
        sys_ioctl(g_drm.card_fd, SHIM_DRM_IOCTL_GEM_CLOSE, &gc);
    }
}

static void drm_fb_cache_flush_all_locked(void) {
    for (int i = 0; i < SHIM_FB_CACHE_SIZE; i++) {
        drm_fb_cache_evict_slot_locked(i);
    }
}

static void drm_fb_cache_prune_stale_locked(uint64_t now_seq) {
    for (int i = 0; i < SHIM_FB_CACHE_SIZE; i++) {
        if (g_drm.fb_cache[i].in_use &&
            now_seq > g_drm.fb_cache[i].last_seq &&
            (now_seq - g_drm.fb_cache[i].last_seq) > SHIM_FB_CACHE_TTL) {
            drm_fb_cache_evict_slot_locked(i);
        }
    }
}

static int drm_fb_cache_get_or_import_locked(int dmabuf_fd, uint32_t bw, uint32_t bh,
                                           uint32_t fourcc, uint32_t pitch, uint32_t offset,
                                           uint64_t now_seq, uint32_t *out_fb_id,
                                           uint32_t *out_tmp_fb_id, uint32_t *out_tmp_handle) {
    *out_fb_id = 0;
    *out_tmp_fb_id = 0;
    *out_tmp_handle = 0;

    struct shim_stat st;
    shim_memset(&st, 0, sizeof(st));
    long fr = sys_fstat(dmabuf_fd, &st);
    int can_cache = (fr == 0 && st.st_ino != 0);

    if (can_cache) {
        for (int i = 0; i < SHIM_FB_CACHE_SIZE; i++) {
            struct shim_fb_cache_entry *e = &g_drm.fb_cache[i];
            if (e->in_use &&
                e->dev == st.st_dev &&
                e->ino == st.st_ino &&
                e->w == bw &&
                e->h == bh &&
                e->fourcc == fourcc &&
                e->pitch == pitch &&
                e->offset == offset) {
                e->last_seq = now_seq;
                *out_fb_id = e->fb_id;
                return 0;
            }
        }
    }

    struct drm_prime_handle ph;
    ph.handle = 0;
    ph.flags = 0;
    ph.fd = (uint32_t)dmabuf_fd;
    long ioctl_ret = sys_ioctl(g_drm.card_fd, SHIM_DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph);
    if (ioctl_ret < 0 || ph.handle == 0) {
        log_err_code("DRM PRIME_FD_TO_HANDLE", ioctl_ret);
        return (int)(ioctl_ret < 0 ? ioctl_ret : -22);
    }

    struct drm_mode_fb_cmd2 fb;
    shim_memset(&fb, 0, sizeof(fb));
    fb.width = bw;
    fb.height = bh;
    fb.pixel_format = fourcc;
    fb.handles[0] = ph.handle;
    fb.pitches[0] = pitch;
    fb.offsets[0] = offset;
    ioctl_ret = sys_ioctl(g_drm.card_fd, SHIM_DRM_IOCTL_MODE_ADDFB2, &fb);
    if (ioctl_ret < 0 || fb.fb_id == 0) {
        if (!drm_gem_handle_in_cache_locked(ph.handle)) {
            struct drm_gem_close gc;
            gc.handle = ph.handle;
            gc.pad = 0;
            sys_ioctl(g_drm.card_fd, SHIM_DRM_IOCTL_GEM_CLOSE, &gc);
        }
        log_err_code("DRM ADDFB2", ioctl_ret);
        return (int)(ioctl_ret < 0 ? ioctl_ret : -22);
    }

    if (can_cache) {
        int slot = -1;
        uint64_t oldest_seq = ~0ULL;
        for (int i = 0; i < SHIM_FB_CACHE_SIZE; i++) {
            if (!g_drm.fb_cache[i].in_use) {
                slot = i;
                break;
            }
            if (g_drm.fb_cache[i].last_seq < now_seq && g_drm.fb_cache[i].last_seq < oldest_seq) {
                oldest_seq = g_drm.fb_cache[i].last_seq;
                slot = i;
            }
        }
        if (slot >= 0) {
            struct shim_fb_cache_entry *e = &g_drm.fb_cache[slot];
            int old_in_use = e->in_use;
            uint32_t old_fb = e->fb_id;
            uint32_t old_handle = e->gem_handle;

            /* Install new entry first so GEM handle ref-check preserves shared dmabuf handles */
            e->in_use = 1;
            e->dev = st.st_dev;
            e->ino = st.st_ino;
            e->w = bw;
            e->h = bh;
            e->fourcc = fourcc;
            e->pitch = pitch;
            e->offset = offset;
            e->gem_handle = ph.handle;
            e->fb_id = fb.fb_id;
            e->last_seq = now_seq;

            if (old_in_use) {
                if (old_fb) {
                    struct drm_mode_closefb cf;
                    cf.fb_id = old_fb;
                    cf.pad = 0;
                    sys_ioctl(g_drm.card_fd, SHIM_DRM_IOCTL_MODE_CLOSEFB, &cf);
                }
                if (old_handle && !drm_gem_handle_in_cache_locked(old_handle)) {
                    struct drm_gem_close gc;
                    gc.handle = old_handle;
                    gc.pad = 0;
                    sys_ioctl(g_drm.card_fd, SHIM_DRM_IOCTL_GEM_CLOSE, &gc);
                }
            }

            *out_fb_id = fb.fb_id;
            return 0;
        }
    }

    *out_fb_id = fb.fb_id;
    *out_tmp_fb_id = fb.fb_id;
    *out_tmp_handle = ph.handle;
    return 0;
}

static int drm_commit_set_active_locked(int active) {
    if (drm_ensure_init_locked() < 0) return -19;
    struct shim_atomic_req_buf ab;
    ab.n_objs = 0;
    ab.n_props = 0;

    if (active && g_drm.prop_conn_crtc_id) {
        atomic_begin_obj(&ab, g_drm.conn_id);
        atomic_add_prop(&ab, g_drm.prop_conn_crtc_id, g_drm.crtc_id);
        atomic_end_obj(&ab);
    }
    atomic_begin_obj(&ab, g_drm.crtc_id);
    atomic_add_prop(&ab, g_drm.prop_crtc_active, active ? 1ULL : 0ULL);
    if (active) atomic_add_prop(&ab, g_drm.prop_crtc_mode_id, g_drm.mode_blob_id);
    atomic_end_obj(&ab);

    if (!active) {
        for (uint32_t i = 0; i < g_drm.num_planes; i++) {
            atomic_begin_obj(&ab, g_drm.planes[i].id);
            atomic_add_prop(&ab, g_drm.planes[i].prop_fb_id, 0);
            atomic_add_prop(&ab, g_drm.planes[i].prop_crtc_id, 0);
            atomic_end_obj(&ab);
        }
        g_drm.prev_used_planes = 0;
    }

    struct drm_mode_atomic ma;
    shim_memset(&ma, 0, sizeof(ma));
    ma.flags = SHIM_DRM_MODE_ATOMIC_ALLOW_MODESET;
    ma.count_objs = ab.n_objs;
    ma.objs_ptr = (uint64_t)(uintptr_t)ab.objs;
    ma.count_props_ptr = (uint64_t)(uintptr_t)ab.cnts;
    ma.props_ptr = (uint64_t)(uintptr_t)ab.prop_ids;
    ma.prop_values_ptr = (uint64_t)(uintptr_t)ab.prop_vals;
    long r = sys_ioctl(g_drm.card_fd, SHIM_DRM_IOCTL_MODE_ATOMIC, &ma);
    if (r == 0) {
        g_drm.crtc_active = active;
        if (active) {
            g_drm.modeset_done = 1;
        } else {
            drm_fb_cache_flush_all_locked();
            if (g_drm.prev_out_fence >= 0) {
                sys_close(g_drm.prev_out_fence);
                g_drm.prev_out_fence = -1;
            }
        }
    }
    return (int)r;
}

static int msmfb_atomic_commit(void *arg) {
    if (!arg) return (int)sys_ret(-22);
    uint8_t *commit_ptr = (uint8_t *)arg;
    uint32_t cflags = *(uint32_t *)(commit_ptr + 0x08);

    *(int *)(commit_ptr + 0x0c) = -1;
    *(int *)(commit_ptr + 0x48) = -1;

    if (cflags & MDP_COMMIT_VALIDATE_FLAG) {
        int vr = msmfb_atomic_commit_validate(commit_ptr);
        return (int)sys_ret(vr);
    }

    const uint8_t *layers = *(const uint8_t *const *)(commit_ptr + 0x30);
    uint32_t cnt = *(const uint32_t *)(commit_ptr + 0x38);
    if (cnt > 0 && !layers) return (int)sys_ret(-22);

    shim_spin_lock(&g_drm_lock);
    if (drm_ensure_init_locked() < 0) {
        shim_spin_unlock(&g_drm_lock);
        return (int)sys_ret(-19);
    }
    if (cnt > g_drm.num_planes) {
        shim_spin_unlock(&g_drm_lock);
        return (int)sys_ret(-22);
    }

    uint64_t seq = ++g_drm.commit_seq;
    uint32_t tmp_fb_ids[SHIM_MAX_DRM_PLANES];
    uint32_t tmp_gem_handles[SHIM_MAX_DRM_PLANES];
    uint32_t n_tmp = 0;
    int out_fence = -1;
    long ioctl_ret = 0;

    struct shim_atomic_req_buf ab;
    ab.n_objs = 0;
    ab.n_props = 0;

    int need_modeset = (!g_drm.modeset_done || !g_drm.crtc_active);
    if (need_modeset && g_drm.prop_conn_crtc_id) {
        atomic_begin_obj(&ab, g_drm.conn_id);
        atomic_add_prop(&ab, g_drm.prop_conn_crtc_id, g_drm.crtc_id);
        atomic_end_obj(&ab);
    }

    atomic_begin_obj(&ab, g_drm.crtc_id);
    if (need_modeset) {
        atomic_add_prop(&ab, g_drm.prop_crtc_active, 1ULL);
        atomic_add_prop(&ab, g_drm.prop_crtc_mode_id, g_drm.mode_blob_id);
    }
    atomic_add_prop(&ab, g_drm.prop_crtc_out_fence_ptr, (uint64_t)(uintptr_t)&out_fence);
    atomic_end_obj(&ab);

    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *l = layers + i * MDP_LAYER_STRIDE;
        uint32_t lflags = *(const uint32_t *)(l + 0x00);
        uint8_t alpha = *(const uint8_t *)(l + 0x0a);
        uint16_t z_order = *(const uint16_t *)(l + 0x0c);
        uint32_t blend_op = *(const uint32_t *)(l + 0x18);
        const struct shim_mdp_rect *src = (const struct shim_mdp_rect *)(l + 0x20);
        const struct shim_mdp_rect *dst = (const struct shim_mdp_rect *)(l + 0x30);
        uint32_t bw = *(const uint32_t *)(l + 0x48);
        uint32_t bh = *(const uint32_t *)(l + 0x4c);
        uint32_t bfmt = *(const uint32_t *)(l + 0x50);
        const struct shim_mdp_plane *pl0 = (const struct shim_mdp_plane *)(l + 0x54);
        int in_fence = *(const int *)(l + 0x90);

        uint32_t cpp = 4;
        uint32_t fourcc = mdp_fmt_to_drm_fourcc(bfmt, &cpp);
        if (!fourcc || pl0->fd < 0 || bw == 0 || bh == 0) {
            ioctl_ret = -22;
            goto cleanup_fbs;
        }
        uint32_t pitch = pl0->stride;
        if (pitch < bw * cpp && pitch >= bw) pitch *= cpp;

        if (g_commit_log_budget > 0) {
            g_commit_log_budget--;
            char msg[192];
            int p = 0;
            p = fmt_append_str(msg, p, sizeof(msg), "Commit L");
            p = fmt_append_uint(msg, p, sizeof(msg), i);
            p = fmt_append_str(msg, p, sizeof(msg), " fd=");
            p = fmt_append_uint(msg, p, sizeof(msg), (uint64_t)(unsigned int)pl0->fd);
            p = fmt_append_str(msg, p, sizeof(msg), " ");
            p = fmt_append_uint(msg, p, sizeof(msg), bw);
            p = fmt_append_str(msg, p, sizeof(msg), "x");
            p = fmt_append_uint(msg, p, sizeof(msg), bh);
            p = fmt_append_str(msg, p, sizeof(msg), " fmt=");
            p = fmt_append_uint(msg, p, sizeof(msg), bfmt);
            p = fmt_append_str(msg, p, sizeof(msg), " stride=");
            p = fmt_append_uint(msg, p, sizeof(msg), pl0->stride);
            p = fmt_append_str(msg, p, sizeof(msg), " pitch=");
            p = fmt_append_uint(msg, p, sizeof(msg), pitch);
            p = fmt_append_str(msg, p, sizeof(msg), " fence=");
            if (in_fence < 0) p = fmt_append_str(msg, p, sizeof(msg), "-1");
            else p = fmt_append_uint(msg, p, sizeof(msg), (uint64_t)(unsigned int)in_fence);
            log_str(msg);
        }

        uint32_t fb_id = 0, tmp_fb = 0, tmp_handle = 0;
        int imp_rc = drm_fb_cache_get_or_import_locked(
            pl0->fd, bw, bh, fourcc, pitch, pl0->offset, seq,
            &fb_id, &tmp_fb, &tmp_handle);
        if (tmp_fb) {
            tmp_fb_ids[n_tmp] = tmp_fb;
            tmp_gem_handles[n_tmp] = tmp_handle;
            n_tmp++;
        }
        if (imp_rc < 0 || fb_id == 0) {
            ioctl_ret = imp_rc < 0 ? imp_rc : -22;
            goto cleanup_fbs;
        }

        uint64_t drm_blend = SHIM_DRM_MODE_BLEND_PREMULTI;
        if (blend_op == 3) drm_blend = SHIM_DRM_MODE_BLEND_COVERAGE;
        else if (blend_op == 1) drm_blend = SHIM_DRM_MODE_BLEND_PIXEL_NONE;

        uint64_t drm_rot = SHIM_DRM_MODE_ROTATE_0;
        if (lflags & 1U) drm_rot |= SHIM_DRM_MODE_REFLECT_X;
        if (lflags & 2U) drm_rot |= SHIM_DRM_MODE_REFLECT_Y;

        const struct shim_drm_plane *dp = &g_drm.planes[i];
        atomic_begin_obj(&ab, dp->id);
        atomic_add_prop(&ab, dp->prop_fb_id, fb_id);
        atomic_add_prop(&ab, dp->prop_crtc_id, g_drm.crtc_id);
        atomic_add_prop(&ab, dp->prop_src_x, (uint64_t)src->x << 16);
        atomic_add_prop(&ab, dp->prop_src_y, (uint64_t)src->y << 16);
        atomic_add_prop(&ab, dp->prop_src_w, (uint64_t)src->w << 16);
        atomic_add_prop(&ab, dp->prop_src_h, (uint64_t)src->h << 16);
        atomic_add_prop(&ab, dp->prop_crtc_x, (uint64_t)(int64_t)(int32_t)dst->x);
        atomic_add_prop(&ab, dp->prop_crtc_y, (uint64_t)(int64_t)(int32_t)dst->y);
        atomic_add_prop(&ab, dp->prop_crtc_w, dst->w);
        atomic_add_prop(&ab, dp->prop_crtc_h, dst->h);
        atomic_add_prop(&ab, dp->prop_zpos, (uint64_t)z_order + 1ULL);
        atomic_add_prop(&ab, dp->prop_alpha, (uint64_t)alpha * 257ULL);
        atomic_add_prop(&ab, dp->prop_blend_mode, drm_blend);
        atomic_add_prop(&ab, dp->prop_rotation, drm_rot);
        atomic_add_prop(&ab, dp->prop_in_fence_fd, (uint64_t)(int64_t)in_fence);
        atomic_end_obj(&ab);
    }

    uint32_t disable_end = g_drm.prev_used_planes > cnt ? g_drm.prev_used_planes : cnt;
    if (disable_end > g_drm.num_planes) disable_end = g_drm.num_planes;
    for (uint32_t i = cnt; i < disable_end; i++) {
        const struct shim_drm_plane *dp = &g_drm.planes[i];
        atomic_begin_obj(&ab, dp->id);
        atomic_add_prop(&ab, dp->prop_fb_id, 0);
        atomic_add_prop(&ab, dp->prop_crtc_id, 0);
        atomic_end_obj(&ab);
    }

    struct drm_mode_atomic ma;
    shim_memset(&ma, 0, sizeof(ma));
    ma.flags = SHIM_DRM_MODE_ATOMIC_NONBLOCK | (need_modeset ? SHIM_DRM_MODE_ATOMIC_ALLOW_MODESET : 0U);
    ma.count_objs = ab.n_objs;
    ma.objs_ptr = (uint64_t)(uintptr_t)ab.objs;
    ma.count_props_ptr = (uint64_t)(uintptr_t)ab.cnts;
    ma.props_ptr = (uint64_t)(uintptr_t)ab.prop_ids;
    ma.prop_values_ptr = (uint64_t)(uintptr_t)ab.prop_vals;

    ioctl_ret = sys_ioctl(g_drm.card_fd, SHIM_DRM_IOCTL_MODE_ATOMIC, &ma);
    for (int retry = 0; retry < 25 && ioctl_ret == -16 /* -EBUSY */; retry++) {
        if (out_fence >= 0) {
            sys_close(out_fence);
            out_fence = -1;
        }
        if (g_drm.prev_out_fence >= 0) {
            struct pollfd pfd;
            pfd.fd = g_drm.prev_out_fence;
            pfd.events = 0x0001; /* POLLIN */
            pfd.revents = 0;
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 40000000L; /* 40 ms */
            sys_ppoll(&pfd, 1, &ts, 0, 8);
            sys_close(g_drm.prev_out_fence);
            g_drm.prev_out_fence = -1;
        } else {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 1000000L; /* 1 ms */
            sys_nanosleep(&ts, 0);
        }
        ioctl_ret = sys_ioctl(g_drm.card_fd, SHIM_DRM_IOCTL_MODE_ATOMIC, &ma);
    }
    if (ioctl_ret == -16 /* -EBUSY */) {
        if (out_fence >= 0) {
            sys_close(out_fence);
            out_fence = -1;
        }
        ma.flags &= ~SHIM_DRM_MODE_ATOMIC_NONBLOCK;
        ioctl_ret = sys_ioctl(g_drm.card_fd, SHIM_DRM_IOCTL_MODE_ATOMIC, &ma);
    }

    if (ioctl_ret == 0) {
        g_drm.modeset_done = 1;
        g_drm.crtc_active = 1;
        g_drm.prev_used_planes = cnt;
        if (g_drm.prev_out_fence >= 0) {
            sys_close(g_drm.prev_out_fence);
            g_drm.prev_out_fence = -1;
        }
        if (out_fence >= 0) {
            g_drm.prev_out_fence = (int)sys_fcntl(out_fence, SHIM_F_DUPFD_CLOEXEC, 0);
        }
        drm_fb_cache_prune_stale_locked(seq);
    } else {
        log_err_code("DRM MODE_ATOMIC", ioctl_ret);
    }

cleanup_fbs:
    for (uint32_t i = 0; i < n_tmp; i++) {
        if (tmp_fb_ids[i]) {
            struct drm_mode_closefb cf;
            cf.fb_id = tmp_fb_ids[i];
            cf.pad = 0;
            sys_ioctl(g_drm.card_fd, SHIM_DRM_IOCTL_MODE_CLOSEFB, &cf);
        }
        if (tmp_gem_handles[i] && !drm_gem_handle_in_cache_locked(tmp_gem_handles[i])) {
            struct drm_gem_close gc;
            gc.handle = tmp_gem_handles[i];
            gc.pad = 0;
            sys_ioctl(g_drm.card_fd, SHIM_DRM_IOCTL_GEM_CLOSE, &gc);
        }
    }

    if (ioctl_ret < 0) {
        if (out_fence >= 0) sys_close(out_fence);
        shim_spin_unlock(&g_drm_lock);
        return (int)sys_ret(ioctl_ret);
    }

    if (out_fence < 0) {
        out_fence = drm_create_signaled_fence_locked(g_drm.card_fd);
    }
    shim_spin_unlock(&g_drm_lock);

    /*
     * Keep real DRM out_fence (PP_DONE) as release_fence (+0x0c) so buffers
     * are not overwritten while MDP5 reads them, and return -1 (NO_FENCE) for
     * retire_fence (+0x48 / HWC2 presentFence) so jittery non-TE PP_DONE
     * timestamps do not poison SurfaceFlinger's VsyncReactor.
     */
    *(int *)(commit_ptr + 0x0c) = out_fence;
    *(int *)(commit_ptr + 0x48) = -1;
    return 0;
}

int ioctl(int fd, unsigned long req, void *arg) {
    unsigned int nr = (unsigned int)(req & 0xff);
    unsigned int type = (unsigned int)((req >> 8) & 0xff);
    unsigned int ioc_size = (unsigned int)((req >> 16) & 0x3fff);

    if (type == 'I' && fd < 0) {
        if (nr == 5 /* ION_IOC_IMPORT */) {
            if (arg) {
                int *p = (int *)arg;
                int buffer_fd = p[1];
                if (buffer_fd <= 0 && p[2] > 0) buffer_fd = p[2];
                if (buffer_fd <= 0) buffer_fd = 1;
                p[0] = buffer_fd;
            }
            return 0;
        }
        return 0;
    }
    if (fd < 0) {
        return (int)sys_ret(-9); /* -EBADF */
    }
    if ((type == 0x6d || (type == 0x53 && nr == 0x80) || req == 0x4611) && shim_is_composer()) {
        if (!g_hwc_debug_base) {
            patch_sdm_loaded_libs();
        }
        if (type == 0x53 && nr == 0x80) {
            return msmfb_atomic_commit(arg);
        }
        if (req == 0x4611 /* FBIOBLANK */) {
            int blank = (int)(uintptr_t)arg;
            if (g_drm.card_fd >= 0) {
                shim_spin_lock(&g_drm_lock);
                drm_commit_set_active_locked(blank == 0);
                shim_spin_unlock(&g_drm_lock);
            }
            return 0;
        }
        if (g_hwc_debug_all_fn && !g_hwc_debug_refreshed && sdm_debug_requested()) {
            g_hwc_debug_all_fn(1, 3);
            g_hwc_debug_refreshed = 1;
        }
        if (g_msmfb_log_budget > 0) {
            g_msmfb_log_budget--;
            char msg[160];
            int p = 0;
            p = fmt_append_str(msg, p, sizeof(msg), "MSMFB ioctl fd=");
            p = fmt_append_uint(msg, p, sizeof(msg), (uint64_t)(unsigned int)fd);
            p = fmt_append_str(msg, p, sizeof(msg), " req=");
            p = fmt_append_hex(msg, p, sizeof(msg), (uint64_t)req);
            p = fmt_append_str(msg, p, sizeof(msg), " nr=");
            p = fmt_append_uint(msg, p, sizeof(msg), nr);
            p = fmt_append_str(msg, p, sizeof(msg), " size=");
            p = fmt_append_uint(msg, p, sizeof(msg), ioc_size);
            if (((req & 0xffff) == 0x6da6 || (req & 0xffff) == 0x6da5) && arg) {
                p = fmt_append_str(msg, p, sizeof(msg), " op=");
                p = fmt_append_uint(msg, p, sizeof(msg), *(uint32_t *)arg);
            }
            log_str(msg);
        }
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
                        sys_futex(&g_vsync_enabled, 129 /* FUTEX_WAKE_PRIVATE */, 1, 0);
                    }
                    if (g_vsync_log_budget > 0 || g_hwc_debug_refreshed) {
                        if (g_vsync_log_budget > 0) g_vsync_log_budget--;
                        log_str(en ? "VSYNC enabled" : "VSYNC disabled");
                    }
                }
            }
            return 0; // MSMFB_OVERLAY_VSYNC_CTRL
        }
        return 0; // Handle MSMFB_METADATA_SET, MSMFB_LPM_ENABLE, etc.
    }
    return (int)sys_ret(sys_ioctl(fd, req, arg));
}

int openat(int dfd, const char *filename, int flags, int mode) {
    if (shim_is_composer() && filename && !g_hwc_debug_base &&
        (str_contains(filename, "/fb0") || str_contains(filename, "mdp/caps") || str_contains(filename, "msm_fb_panel_info"))) {
        patch_sdm_loaded_libs();
    }
    long ret = sys_openat(dfd, filename, flags, mode);
    if (ret >= 0 && filename && shim_is_composer() && str_contains(filename, "vsync_event")) {
        g_vsync_fd = (int)ret;
        if (ret < 1024) g_is_vsync_fd[ret] = 1;
        log_str("Tracked vsync_event fd");
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

int __open_2(const char *filename, int flags) {
    return openat(-100, filename, flags, 0);
}

void *dlopen(const char *filename, int flag) {
    typedef void *(*fn_dlopen_t)(const char *, int);
    static fn_dlopen_t real_dlopen = 0;
    void *h = 0;
    if (!real_dlopen && dlsym) {
        real_dlopen = (fn_dlopen_t)dlsym((void *)-1L /* RTLD_NEXT */, "dlopen");
    }
    if (real_dlopen) {
        h = real_dlopen(filename, flag);
    } else if (__loader_dlopen) {
        h = __loader_dlopen(filename, flag, __builtin_return_address(0));
    }
    if (h && shim_is_composer() && filename &&
        (str_contains(filename, "composer") || str_contains(filename, "sdm") || str_contains(filename, "hwc"))) {
        patch_sdm_loaded_libs();
    }
    return h;
}

void *android_dlopen_ext(const char *filename, int flag, const void *extinfo) {
    typedef void *(*fn_ext_t)(const char *, int, const void *);
    static fn_ext_t real_ext = 0;
    void *h = 0;
    if (!real_ext && dlsym) {
        real_ext = (fn_ext_t)dlsym((void *)-1L /* RTLD_NEXT */, "android_dlopen_ext");
    }
    if (real_ext) {
        h = real_ext(filename, flag, extinfo);
    } else if (__loader_android_dlopen_ext) {
        h = __loader_android_dlopen_ext(filename, flag, extinfo, __builtin_return_address(0));
    }
    if (h && shim_is_composer() && filename &&
        (str_contains(filename, "composer") || str_contains(filename, "sdm") || str_contains(filename, "hwc"))) {
        patch_sdm_loaded_libs();
    }
    return h;
}

int close(int fd) {
    if (fd >= 0 && fd < 1024 && g_is_vsync_fd[fd]) {
        g_is_vsync_fd[fd] = 0;
        if (fd == g_vsync_fd) g_vsync_fd = -1;
    }
    return (int)sys_ret(sys_close(fd));
}

long pread(int fd, void *buf, unsigned long count, unsigned long offset) {
    if (shim_is_composer() && is_vsync(fd) && buf && count > 0) {
        uint64_t ts_ns = g_last_vsync_ns;
        if (ts_ns == 0) {
            struct timespec now;
            sys_clock_gettime(1 /* CLOCK_MONOTONIC */, &now);
            ts_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
        }

        char str[64];
        int len = 0;
        str[len++] = 'V'; str[len++] = 'S'; str[len++] = 'Y';
        str[len++] = 'N'; str[len++] = 'C'; str[len++] = '=';

        char digits[24];
        int dlen = 0;
        uint64_t val = ts_ns;
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
    if (shim_is_composer() && is_vsync(fd) && buf && count > 0) {
        return pread(fd, buf, count, 0);
    }
    return sys_ret(sys_read(fd, buf, count));
}

static int find_vsync_pollfd(const struct pollfd *fds, unsigned long nfds, int bind_fallback) {
    if (!shim_is_composer() || !fds) return -1;
    for (unsigned long i = 0; i < nfds; i++) {
        int fd = fds[i].fd;
        if (fd >= 0 && (is_vsync(fd) || (g_vsync_fd < 0 && (fds[i].events & 0x0002)))) {
            if (bind_fallback && g_vsync_fd < 0) {
                g_vsync_fd = fd;
                if (fd < 1024) g_is_vsync_fd[fd] = 1;
            }
            return (int)i;
        }
    }
    return -1;
}

int poll(struct pollfd *fds, unsigned long nfds, int timeout) {
    int vsync_idx = find_vsync_pollfd(fds, nfds, 1);

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
            if (timeout == 0) return 0;
            struct timespec ts = {0, 50000000L}; /* 50ms fallback; futex wakes immediately on enable */
            sys_futex(&g_vsync_enabled, 128 /* FUTEX_WAIT_PRIVATE */, 0, &ts);
            continue;
        }

        struct timespec now;
        sys_clock_gettime(1 /* CLOCK_MONOTONIC */, &now);
        uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;

        const uint64_t period_ns = 16666667ULL;
        uint64_t target_ns;
        if (g_last_vsync_ns == 0) {
            target_ns = now_ns + period_ns;
        } else if (now_ns > g_last_vsync_ns + period_ns) {
            uint64_t skipped = (now_ns - g_last_vsync_ns) / period_ns;
            target_ns = g_last_vsync_ns + (skipped + 1ULL) * period_ns;
        } else {
            target_ns = g_last_vsync_ns + period_ns;
        }

        if (now_ns < target_ns) {
            uint64_t diff_ns = target_ns - now_ns;
            struct timespec wait_ts;
            wait_ts.tv_sec = (long)(diff_ns / 1000000000ULL);
            wait_ts.tv_nsec = (long)(diff_ns % 1000000000ULL);
            sys_nanosleep(&wait_ts, 0);
        }

        g_last_vsync_ns = target_ns;

        for (unsigned long i = 0; i < nfds; i++) {
            fds[i].revents = 0;
        }
        fds[vsync_idx].revents = 0x0002; // POLLPRI
        return 1;
    }
}

int ppoll(struct pollfd *fds, unsigned long nfds, const struct timespec *timeout, const void *sigmask) {
    if (find_vsync_pollfd(fds, nfds, 0) < 0) {
        return (int)sys_ret(sys_ppoll(fds, nfds, timeout, sigmask, 8));
    }
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

int _ZN8gralloc19Allocator12ImportBufferEi(void *this, int fd) {
    (void)this;
    sys_write(2, "[+] libsdm_shim: Allocator::ImportBuffer -> OK\n", 47);
    return (fd > 0) ? fd : 1;
}

int _ZN8gralloc18IonAlloc12ImportBufferEi(void *this, int fd) {
    (void)this;
    sys_write(2, "[+] libsdm_shim: IonAlloc::ImportBuffer -> OK\n", 46);
    return (fd > 0) ? fd : 1;
}

/* Android 10 libbase Trim & WriteStringToFd compatibility for Health HAL */
void _ZN7android4base4TrimIRKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEEES8_OT_(void *ret, const void *src);

__attribute__((visibility("default")))
void _ZN7android4base4TrimERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE(void *ret, const void *src) {
    _ZN7android4base4TrimIRKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEEES8_OT_(ret, src);
}

__attribute__((weak)) unsigned long _ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4copyEPcmm(
    const void *str, char *dst, unsigned long n, unsigned long pos);

__attribute__((visibility("default")))
int _ZN7android4base15WriteStringToFdERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEEi(
    const void *str, int fd) {
    if (!str || !_ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4copyEPcmm) {
        return 0;
    }
    char buf[1024];
    unsigned long pos = 0;
    while (1) {
        unsigned long got = _ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4copyEPcmm(
            str, buf, sizeof(buf), pos);
        if (got == 0) break;
        unsigned long off = 0;
        while (off < got) {
            long w = sys_write(fd, buf + off, got - off);
            if (w < 0) {
                if (w == -4) continue; /* EINTR */
                sys_ret(w);
                return 0;
            }
            if (w == 0) return 0;
            off += (unsigned long)w;
        }
        pos += got;
        if (got < sizeof(buf)) break;
    }
    return 1;
}

/* Android 10 libutils String8(StaticLinkage) compatibility for Health HAL */
__attribute__((weak)) void _ZN7android7String8C1Ev(void *self);
__attribute__((weak)) void _ZN7android7String8C2Ev(void *self);

__attribute__((visibility("default")))
void _ZN7android7String8C1ENS0_13StaticLinkageE(void *self, int linkage) {
    (void)linkage;
    if (_ZN7android7String8C1Ev) _ZN7android7String8C1Ev(self);
}

__attribute__((visibility("default")))
void _ZN7android7String8C2ENS0_13StaticLinkageE(void *self, int linkage) {
    (void)linkage;
    if (_ZN7android7String8C2Ev) _ZN7android7String8C2Ev(self);
}

/* Pure-software KeyMint fallback for Keystore2 when vendor keymaster@3.0 is disabled */
typedef struct {
    void *ptr;
    void *cntrl;
} km_sp_t;

__attribute__((weak)) void *AStatus_newOk(void);
__attribute__((weak)) void *AStatus_fromServiceSpecificError(int errorCode);
__attribute__((weak)) void _ZN4aidl7android8hardware8security7keymint19CreateKeyMintDeviceENS3_13SecurityLevelE(void);
__attribute__((weak)) void _ZNSt3__119__shared_weak_count12__add_sharedEv(void *cntrl);
__attribute__((weak)) void _ZNSt3__119__shared_weak_count16__release_sharedEv(void *cntrl);

static volatile int g_km_lock = 0;
static km_sp_t g_km_tee = {0, 0};
static km_sp_t g_km_sw = {0, 0};

static inline void km_call_x8_i32(void (*fn)(void), int arg0, void *out_buf) {
    register void (*r_fn)(void) __asm__("x9") = fn;
    register int r_w0 __asm__("w0") = arg0;
    register void *r_x8 __asm__("x8") = out_buf;
    __asm__ volatile(
        "blr x9"
        : "+r"(r_w0)
        : "r"(r_fn), "r"(r_x8)
        : "x1", "x2", "x3", "x4", "x5", "x6", "x7",
          "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x30", "memory", "cc");
}

static void km_release_sp(km_sp_t *sp) {
    if (sp && sp->cntrl && _ZNSt3__119__shared_weak_count16__release_sharedEv) {
        _ZNSt3__119__shared_weak_count16__release_sharedEv(sp->cntrl);
    }
    if (sp) {
        sp->ptr = 0;
        sp->cntrl = 0;
    }
}

static void km_return_unavailable(km_sp_t *out, void **out_status) {
    km_release_sp(out);
    if (out_status) {
        /* ErrorCode::HARDWARE_TYPE_UNAVAILABLE = -68 */
        *out_status = AStatus_fromServiceSpecificError ? AStatus_fromServiceSpecificError(-68) : 0;
    }
}

__attribute__((visibility("hidden")))
void shim_km_getKeyMintDevice_impl(void *self, int sec_level, km_sp_t *out, void **out_status) {
    (void)self;
    if (!out || !out_status) return;

    /* SecurityLevel: SOFTWARE = 0, TRUSTED_ENVIRONMENT = 1 */
    if (sec_level != 0 && sec_level != 1) {
        km_return_unavailable(out, out_status);
        return;
    }

    shim_spin_lock(&g_km_lock);
    km_sp_t *cache = (sec_level == 1) ? &g_km_tee : &g_km_sw;
    if (!cache->ptr && _ZN4aidl7android8hardware8security7keymint19CreateKeyMintDeviceENS3_13SecurityLevelE) {
        km_sp_t created = {0, 0};
        km_call_x8_i32(_ZN4aidl7android8hardware8security7keymint19CreateKeyMintDeviceENS3_13SecurityLevelE, sec_level, &created);
        *cache = created;
        if (created.ptr && sec_level == 1) {
            klog("[+] libsdm_shim: Created software KeyMint TEE fallback device\n", 61);
        }
    }
    km_sp_t chosen = *cache;
    if (chosen.cntrl && _ZNSt3__119__shared_weak_count12__add_sharedEv) {
        _ZNSt3__119__shared_weak_count12__add_sharedEv(chosen.cntrl);
    }
    shim_spin_unlock(&g_km_lock);

    km_release_sp(out);
    *out = chosen;
    if (chosen.ptr && AStatus_newOk) {
        *out_status = AStatus_newOk();
    } else {
        *out_status = AStatus_fromServiceSpecificError ? AStatus_fromServiceSpecificError(-68) : 0;
    }
}

__attribute__((visibility("hidden")))
void shim_km_getSecureClock_impl(void *self, km_sp_t *out, void **out_status) {
    (void)self;
    km_return_unavailable(out, out_status);
}

__attribute__((visibility("hidden")))
void shim_km_getSharedSecret_impl(void *self, int sec_level, km_sp_t *out, void **out_status) {
    (void)self;
    (void)sec_level;
    km_return_unavailable(out, out_status);
}

__asm__(
    ".text\n"
    ".globl _ZN21KeystoreCompatService16getKeyMintDeviceEN4aidl7android8hardware8security7keymint13SecurityLevelEPNSt3__110shared_ptrINS4_14IKeyMintDeviceEEE\n"
    ".type _ZN21KeystoreCompatService16getKeyMintDeviceEN4aidl7android8hardware8security7keymint13SecurityLevelEPNSt3__110shared_ptrINS4_14IKeyMintDeviceEEE, %function\n"
    "_ZN21KeystoreCompatService16getKeyMintDeviceEN4aidl7android8hardware8security7keymint13SecurityLevelEPNSt3__110shared_ptrINS4_14IKeyMintDeviceEEE:\n"
    "  mov x3, x8\n"
    "  b shim_km_getKeyMintDevice_impl\n"
    ".globl _ZN21KeystoreCompatService14getSecureClockEPNSt3__110shared_ptrIN4aidl7android8hardware8security11secureclock12ISecureClockEEE\n"
    ".type _ZN21KeystoreCompatService14getSecureClockEPNSt3__110shared_ptrIN4aidl7android8hardware8security11secureclock12ISecureClockEEE, %function\n"
    "_ZN21KeystoreCompatService14getSecureClockEPNSt3__110shared_ptrIN4aidl7android8hardware8security11secureclock12ISecureClockEEE:\n"
    "  mov x2, x8\n"
    "  b shim_km_getSecureClock_impl\n"
    ".globl _ZN21KeystoreCompatService15getSharedSecretEN4aidl7android8hardware8security7keymint13SecurityLevelEPNSt3__110shared_ptrINS3_12sharedsecret13ISharedSecretEEE\n"
    ".type _ZN21KeystoreCompatService15getSharedSecretEN4aidl7android8hardware8security7keymint13SecurityLevelEPNSt3__110shared_ptrINS3_12sharedsecret13ISharedSecretEEE, %function\n"
    "_ZN21KeystoreCompatService15getSharedSecretEN4aidl7android8hardware8security7keymint13SecurityLevelEPNSt3__110shared_ptrINS3_12sharedsecret13ISharedSecretEEE:\n"
    "  mov x3, x8\n"
    "  b shim_km_getSharedSecret_impl\n"
);




