#define SYS_execve 221
#define SYS_exit 94

static const char preload_str[] = "LD_PRELOAD=/system/lib64/libsdm_shim.so";
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

/* Build <prefix><name>.real in a bounded stack buffer and execve with LD_PRELOAD. */
static void try_exec_real(const char *prefix, const char *name, char **argv, char **new_env) {
    char target[256];
    unsigned long pos = 0;
    const unsigned long cap = sizeof(target) - 1;
    const char *parts[3] = { prefix, name, real_suffix };

    for (int i = 0; i < 3; i++) {
        for (const char *p = parts[i]; p && *p; p++) {
            if (pos >= cap) return;
            target[pos++] = *p;
        }
    }
    target[pos] = 0;

    argv[0] = target;
    my_execve(target, argv, new_env);
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

    const char *orig_argv0 = argv[0];
    const char *slash = 0;
    for (const char *p = orig_argv0; *p; p++) {
        if (*p == '/') slash = p;
    }
    const char *bname = slash ? slash + 1 : orig_argv0;

    /* Strategy 1: /system/bin/<basename>.real */
    try_exec_real(sys_bin, bname, argv, new_env);

    /* Strategy 2: <original_argv0>.real */
    try_exec_real("", orig_argv0, argv, new_env);

    my_exit(127);
}

__asm__(
".global _start\n"
"_start:\n"
"    mov x0, sp\n"
"    b c_start\n"
);
