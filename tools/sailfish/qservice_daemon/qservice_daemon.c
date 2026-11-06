#define RTLD_NOW 2

void *dlopen(const char *filename, int flag);
void *dlsym(void *handle, const char *symbol);
char *dlerror(void);
int pause(void);
long write(int fd, const void *buf, unsigned long count);
void exit(int status);
int fork(void);

static void print_str(const char *s) {
    unsigned long len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

void _start(void) {
    int fpid = fork();
    if (fpid > 0) {
        print_str("[+] Daemonized qservice_daemon to background!\n");
        exit(0);
    }

    print_str("[*] qservice_daemon: Initializing /dev/vndbinder...\n");
    void *lb = dlopen("/system/lib64/libbinder.so", RTLD_NOW);
    if (!lb) {
        print_str("[-] dlopen libbinder.so failed: ");
        const char *e = dlerror();
        if (e) print_str(e);
        print_str("\n");
        exit(1);
    }

    void (*init_with_driver)(void) = (void (*)(void))dlsym(lb, "_ZN7android12ProcessState14initWithDriverEPKc");
    void (*get_context_object)(void) = (void (*)(void))dlsym(lb, "_ZN7android12ProcessState16getContextObjectERKNS_2spINS_7IBinderEEE");
    void (*as_interface)(void) = (void (*)(void))dlsym(lb, "_ZN7android2os15IServiceManager11asInterfaceERKNS_2spINS_7IBinderEEE");
    void (*get_shim)(void) = (void (*)(void))dlsym(lb, "_ZN7android51getServiceManagerShimFromAidlServiceManagerForTestsERKNS_2spINS_2os15IServiceManagerEEE");
    void (*set_default_sm)(void) = (void (*)(void))dlsym(lb, "_ZN7android24setDefaultServiceManagerERKNS_2spINS_15IServiceManagerEEE");
    void (*start_thread_pool)(void *) = (void (*)(void *))dlsym(lb, "_ZN7android12ProcessState15startThreadPoolEv");
    void *(*ipc_self)(void) = (void *(*)(void))dlsym(lb, "_ZN7android14IPCThreadState4selfEv");
    void (*join_thread_pool)(void *, _Bool) = (void (*)(void *, _Bool))dlsym(lb, "_ZN7android14IPCThreadState14joinThreadPoolEb");

    void *ps_sp[2] = {0, 0};
    if (init_with_driver) {
        const char *driver = "/dev/vndbinder";
        register void *ret_buf asm("x8") = ps_sp;
        register const char *arg0 asm("x0") = driver;
        register void *fn asm("x16") = (void *)init_with_driver;
        asm volatile(
            "blr x16"
            : "+r"(ret_buf), "+r"(arg0)
            : "r"(fn)
            : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "x30", "memory"
        );
        print_str("[+] Initialized ProcessState with /dev/vndbinder\n");
    }

    void *ps = ps_sp[0];
    if (ps && get_context_object && as_interface && get_shim && set_default_sm) {
        print_str("[*] Binding vndservicemanager as defaultServiceManager()...\n");
        void *binder_sp[2] = {0, 0};
        void *null_sp[2] = {0, 0};
        register void *ret_buf8 asm("x8") = binder_sp;
        register void *arg0 asm("x0") = ps;
        register void *arg1 asm("x1") = null_sp;
        register void *fn asm("x16") = (void *)get_context_object;
        asm volatile(
            "blr x16"
            : "+r"(ret_buf8), "+r"(arg0), "+r"(arg1)
            : "r"(fn)
            : "x2", "x3", "x4", "x5", "x6", "x7", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "x30", "memory"
        );

        if (binder_sp[0]) {
            void *aidl_sm_sp[2] = {0, 0};
            ret_buf8 = aidl_sm_sp;
            arg0 = binder_sp;
            fn = (void *)as_interface;
            asm volatile(
                "blr x16"
                : "+r"(ret_buf8), "+r"(arg0)
                : "r"(fn)
                : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "x30", "memory"
            );

            if (aidl_sm_sp[0]) {
                void *legacy_sm_sp[2] = {0, 0};
                ret_buf8 = legacy_sm_sp;
                arg0 = aidl_sm_sp;
                fn = (void *)get_shim;
                asm volatile(
                    "blr x16"
                    : "+r"(ret_buf8), "+r"(arg0)
                    : "r"(fn)
                    : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "x30", "memory"
                );

                if (legacy_sm_sp[0]) {
                    arg0 = legacy_sm_sp;
                    fn = (void *)set_default_sm;
                    asm volatile(
                        "blr x16"
                        : "+r"(arg0)
                        : "r"(fn)
                        : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "x30", "memory"
                    );
                    print_str("[+] Successfully bound vndservicemanager as defaultServiceManager()!\n");
                }
            }
        }
    }

    print_str("[*] Loading /vendor/lib64/libqservice.so...\n");
    void *lq = dlopen("/vendor/lib64/libqservice.so", RTLD_NOW);
    if (!lq) {
        print_str("[-] dlopen libqservice.so failed: ");
        const char *e = dlerror();
        if (e) print_str(e);
        print_str("\n");
        exit(2);
    }

    int (*qinit)(void) = (int (*)(void))dlsym(lq, "_ZN8qService8QService4initEv");
    if (!qinit) {
        print_str("[-] dlsym _ZN8qService8QService4initEv failed\n");
        exit(3);
    }

    print_str("[*] Calling qService::QService::init()...\n");
    int rc = qinit();
    print_str("[+] qService::QService::init() returned!\n");

    if (start_thread_pool && ps) {
        start_thread_pool(ps);
        print_str("[+] Started binder threadpool\n");
    }

    print_str("[+] qservice_daemon is ready and running!\n");
    void *ipc = ipc_self ? ipc_self() : 0;
    if (join_thread_pool && ipc) {
        join_thread_pool(ipc, 1);
    } else {
        while (1) pause();
    }
    exit(0);
}
