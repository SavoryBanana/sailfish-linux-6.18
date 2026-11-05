#define RTLD_NOW 2

void *dlopen(const char *filename, int flag);
void *dlsym(void *handle, const char *symbol);
char *dlerror(void);
int pause(void);
long write(int fd, const void *buf, unsigned long count);
void exit(int status);

static void print_str(const char *s) {
    unsigned long len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

void _start(void) {
    print_str("[*] qservice_daemon: Initializing /dev/vndbinder...\n");
    void *lb = dlopen("/system/lib64/libbinder.so", RTLD_NOW);
    if (!lb) {
        print_str("[-] dlopen libbinder.so failed: ");
        const char *e = dlerror();
        if (e) print_str(e);
        print_str("\n");
        exit(1);
    }
    void *(*init_with_driver)(const char *) = (void *(*)(const char *))dlsym(lb, "_ZN7android12ProcessState14initWithDriverEPKc");
    void (*start_thread_pool)(void *) = (void (*)(void *))dlsym(lb, "_ZN7android12ProcessState15startThreadPoolEv");
    void (*join_thread_pool)(_Bool) = (void (*)(_Bool))dlsym(lb, "_ZN7android14IPCThreadState14joinThreadPoolEb");

    void *ps = 0;
    if (init_with_driver) {
        ps = init_with_driver("/dev/vndbinder");
        print_str("[+] Initialized ProcessState with /dev/vndbinder\n");
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
    if (join_thread_pool) {
        join_thread_pool(1);
    } else {
        while (1) pause();
    }
    exit(0);
}
