/*
 * Minimal Android 10 -> Android 17 LD_PRELOAD compatibility shim for Sailfish:
 *  1. android::String8::String8(StaticLinkage) compat for stock health@2.0 / power@1.1
 *  2. Pure-software KeyMint fallback for keystore2 when hardware keymaster@3.0 is absent
 *
 * All legacy 3.18 msmfb / libsdmcore / DRM / ioctl hooks have been removed now that
 * display and graphics run natively on mainline Linux 6.18 DRM/KMS + AIDL IComposer3.
 */

#include <stddef.h>
#include <stdint.h>

static inline long sys_openat(int dfd, const char *filename, int flags, int mode) {
    register long x0 __asm__("x0") = dfd;
    register long x1 __asm__("x1") = (long)filename;
    register long x2 __asm__("x2") = flags;
    register long x3 __asm__("x3") = mode;
    register long x8 __asm__("x8") = 56; /* SYS_openat */
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

static inline long sys_write(int fd, const void *buf, size_t count) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)count;
    register long x8 __asm__("x8") = 64; /* SYS_write */
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static inline long sys_close(int fd) {
    register long x0 __asm__("x0") = fd;
    register long x8 __asm__("x8") = 57; /* SYS_close */
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static void klog(const char *msg, size_t len) {
    int fd = (int)sys_openat(-100, "/dev/kmsg", 1 /* O_WRONLY */, 0);
    if (fd >= 0) {
        sys_write(fd, msg, len);
        sys_close(fd);
    }
}

static inline void shim_spin_lock(volatile int *lock) {
    while (__sync_lock_test_and_set(lock, 1))
        __asm__ __volatile__("yield" ::: "memory");
}

static inline void shim_spin_unlock(volatile int *lock) {
    __sync_lock_release(lock);
}

/* Android 10 libbase Trim & WriteStringToFd compatibility for Health HAL */
__attribute__((weak))
void _ZN7android4base4TrimIRKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEEES8_OT_(void *ret, const void *src);

__attribute__((visibility("default")))
void _ZN7android4base4TrimERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE(void *ret, const void *src) {
    if (_ZN7android4base4TrimIRKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEEES8_OT_)
        _ZN7android4base4TrimIRKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEEES8_OT_(ret, src);
}

__attribute__((weak)) unsigned long _ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4copyEPcmm(
    const void *str, char *dst, unsigned long n, unsigned long pos);

__attribute__((visibility("default")))
int _ZN7android4base15WriteStringToFdERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEEi(
    const void *str, int fd) {
    if (!str || !_ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4copyEPcmm)
        return 0;
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

/* Android 10 libutils String8(StaticLinkage) compatibility for Health/Power HALs */
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
    if (sp && sp->cntrl && _ZNSt3__119__shared_weak_count16__release_sharedEv)
        _ZNSt3__119__shared_weak_count16__release_sharedEv(sp->cntrl);
    if (sp) {
        sp->ptr = 0;
        sp->cntrl = 0;
    }
}

static void km_return_unavailable(km_sp_t *out, void **out_status) {
    km_release_sp(out);
    if (out_status)
        *out_status = AStatus_fromServiceSpecificError ? AStatus_fromServiceSpecificError(-68) : 0;
}

__attribute__((visibility("hidden")))
void shim_km_getKeyMintDevice_impl(void *self, int sec_level, km_sp_t *out, void **out_status) {
    (void)self;
    if (!out || !out_status) return;

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
        if (created.ptr && sec_level == 1)
            klog("[+] libsdm_shim: Created software KeyMint TEE fallback device\n", 61);
    }
    km_sp_t chosen = *cache;
    if (chosen.cntrl && _ZNSt3__119__shared_weak_count12__add_sharedEv)
        _ZNSt3__119__shared_weak_count12__add_sharedEv(chosen.cntrl);
    shim_spin_unlock(&g_km_lock);

    km_release_sp(out);
    *out = chosen;
    if (chosen.ptr && AStatus_newOk)
        *out_status = AStatus_newOk();
    else
        *out_status = AStatus_fromServiceSpecificError ? AStatus_fromServiceSpecificError(-68) : 0;
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
