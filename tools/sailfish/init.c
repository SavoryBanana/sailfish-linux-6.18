#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>

void print_msg(const char *msg) {
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        write(fd, msg, strlen(msg));
        close(fd);
    }
    int kfd = open("/dev/kmsg", O_WRONLY);
    if (kfd >= 0) {
        write(kfd, msg, strlen(msg));
        close(kfd);
    }
}

int main() {
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);

    mount("none", "/dev", "devtmpfs", 0, NULL);
    mount("none", "/proc", "proc", 0, NULL);
    mount("none", "/sys", "sysfs", 0, NULL);

    print_msg("\n\n");
    print_msg("======================================================\n");
    print_msg("  Google Pixel (sailfish) Linux 6.18 Early Bringup    \n");
    print_msg("  Android 17 Common Kernel (ACK) - MSM8996 Pro        \n");
    print_msg("======================================================\n");
    print_msg("[+] Kernel 6.18 booted successfully!\n");
    print_msg("[+] Userland /init started (aarch64 static binary)\n");
    print_msg("[+] Mounted /dev, /proc, /sys\n");

    DIR *d = opendir("/sys/class/block");
    if (d) {
        struct dirent *dir;
        print_msg("[+] Detected Block Devices in /sys/class/block:\n");
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_name[0] != '.') {
                char bmsg[64];
                snprintf(bmsg, sizeof(bmsg), "    -> %s\n", dir->d_name);
                print_msg(bmsg);
            }
        }
        closedir(d);
    } else {
        print_msg("[-] Could not read /sys/class/block\n");
    }

    print_msg("======================================================\n");
    print_msg("System alive. Entering sleep loop.\n\n");

    while (1) {
        sleep(5);
    }
    return 0;
}
