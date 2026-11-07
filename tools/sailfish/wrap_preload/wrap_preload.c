#define SYS_execve 221
#define SYS_exit 94

static const char preload_str[] = "LD_PRELOAD=/system/lib64/libsdm_shim.so:/data/libsdm_shim.so";
static const char sys_bin[] = "/system/bin/";
static const char real_suffix[] = ".real";

static inline long my_execve(const char *filename, char *const argv[], char *const envp[]) {
    register long x0 __asm__("x0") = (long)filename;
    register long x1 __asm__("x1") = (long)argv;
    register long x2 __asm__("x2") = (long)envp;
    register long x8 __asm__("x8") = SYS_execve;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static inline void my_exit(int code) {
    register long x0 __asm__("x0") = code;
    register long x8 __asm__("x8") = SYS_exit;
    __asm__ __volatile__("svc #0" : : "r"(x0), "r"(x8) : "memory");
    while(1);
}

void c_start(long *sp) {
    long argc = *sp;
    char **argv = (char **)(sp + 1);
    char **envp = (char **)(sp + 1 + argc + 1);

    int envc = 0;
    while (envp && envp[envc]) envc++;

    char *new_env[256];
    int idx = 0;
    new_env[idx++] = (char *)preload_str;
    for (int i = 0; i < envc && idx < 254; i++) {
        new_env[idx++] = envp[i];
    }
    new_env[idx] = 0;

    /* Strategy 1: /system/bin/<basename>.real */
    char target1[256];
    char *d = target1;
    const char *p = sys_bin;
    while (*p) *d++ = *p++;

    const char *slash = 0;
    for (p = argv[0]; *p; p++) {
        if (*p == '/') slash = p;
    }
    const char *bname = slash ? slash + 1 : argv[0];
    while (*bname) *d++ = *bname++;

    p = real_suffix;
    while (*p) *d++ = *p++;
    *d = 0;

    argv[0] = target1;
    my_execve(target1, argv, new_env);

    /* Strategy 2: <original_argv0>.real */
    char target2[256];
    d = target2;
    p = argv[0];
    while (*p) *d++ = *p++;
    p = real_suffix;
    while (*p) *d++ = *p++;
    *d = 0;

    argv[0] = target2;
    my_execve(target2, argv, new_env);

    my_exit(127);
}

__asm__(
".global _start\n"
"_start:\n"
"    mov x0, sp\n"
"    b c_start\n"
);
