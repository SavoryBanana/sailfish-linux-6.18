#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <linux/reboot.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <errno.h>

static int g_ttygs_fd = -1;

static void write_str(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, val, strlen(val));
        close(fd);
    }
}

static bool read_first_line(const char *path, char *buf, size_t len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(buf, len, "n/a");
        return false;
    }
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0) {
        snprintf(buf, len, "empty");
        return false;
    }
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = '\0';
    return true;
}

static bool find_udc(char *buf, size_t len) {
    DIR *d = opendir("/sys/class/udc");
    if (!d)
        return false;
    struct dirent *dir;
    bool found = false;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] != '.') {
            snprintf(buf, len, "%s", dir->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static void get_udc_state(char *buf, size_t len) {
    char udc[96];
    if (!find_udc(udc, sizeof(udc))) {
        snprintf(buf, len, "no-udc");
        return;
    }
    char path[160];
    snprintf(path, sizeof(path), "/sys/class/udc/%s/state", udc);
    read_first_line(path, buf, len);
}

static bool is_udc_configured(void) {
    char state[64];
    get_udc_state(state, sizeof(state));
    return strcmp(state, "configured") == 0;
}

static void close_ttygs(void) {
    if (g_ttygs_fd >= 0) {
        close(g_ttygs_fd);
        g_ttygs_fd = -1;
    }
}

static void open_ttygs_if_ready(void) {
    if (g_ttygs_fd >= 0)
        return;
    if (!is_udc_configured())
        return;

    int fd = open("/dev/ttyGS0", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd >= 0) {
        struct termios tio;
        if (tcgetattr(fd, &tio) == 0) {
            cfmakeraw(&tio);
            tio.c_cflag |= (CLOCAL | CREAD | CS8);
            tio.c_oflag |= (OPOST | ONLCR);
            tcsetattr(fd, TCSANOW, &tio);
        }
        g_ttygs_fd = fd;
        const char *banner =
            "\r\n======================================================\r\n"
            "  Google Pixel (sailfish) - Linux 6.18 USB Shell\r\n"
            "  Type 'help' for commands, 'bootloader' for fastboot\r\n"
            "======================================================\r\n"
            "sailfish# ";
        write(g_ttygs_fd, banner, strlen(banner));
    }
}

static void usb_write_ex(const char *msg, bool wait_drain) {
    if (g_ttygs_fd < 0 || !msg)
        return;

    size_t off = 0;
    size_t len = strlen(msg);
    int retries = 0;

    while (off < len) {
        ssize_t rc = write(g_ttygs_fd, msg + off, len - off);
        if (rc > 0) {
            off += (size_t)rc;
            retries = 0;
            continue;
        }
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_drain || retries++ >= 20)
                break;
            struct pollfd pfd = {
                .fd = g_ttygs_fd,
                .events = POLLOUT,
                .revents = 0,
            };
            int prc = poll(&pfd, 1, 50);
            if (prc > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
                close_ttygs();
                break;
            }
            continue;
        }
        if (rc < 0 && errno == EIO)
            close_ttygs();
        break;
    }
}

static void usb_write(const char *msg) {
    usb_write_ex(msg, true);
}

static void print_msg(const char *msg) {
    const char *devs[] = { "/dev/tty0", "/dev/kmsg", "/dev/pmsg0" };
    for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        int fd = open(devs[i], O_WRONLY | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) {
            write(fd, msg, strlen(msg));
            close(fd);
        }
    }
    usb_write_ex(msg, false);
}

static bool has_non_loop_block(void) {
    DIR *d = opendir("/sys/class/block");
    if (!d)
        return false;
    struct dirent *dir;
    bool found = false;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] == '.')
            continue;
        if (strncmp(dir->d_name, "loop", 4) != 0 &&
            strncmp(dir->d_name, "ram", 3) != 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static void dump_sysfs_dir(const char *label, const char *path, bool usb_only) {
    char line[192];
    DIR *d = opendir(path);
    if (!d) {
        snprintf(line, sizeof(line), "[-] Could not open %s: %s\n", path, strerror(errno));
        if (usb_only) usb_write(line); else print_msg(line);
        return;
    }

    snprintf(line, sizeof(line), "[+] %s (%s):\n", label, path);
    if (usb_only) usb_write(line); else print_msg(line);

    struct dirent *dir;
    int count = 0;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] == '.')
            continue;
        if (strncmp(dir->d_name, "loop", 4) == 0)
            continue;
        snprintf(line, sizeof(line), "    -> %s\n", dir->d_name);
        if (usb_only) usb_write(line); else print_msg(line);
        count++;
    }
    if (count == 0) {
        if (usb_only) usb_write("    (none)\n"); else print_msg("    (none)\n");
    }
    closedir(d);
}

static void dump_file_lines(const char *path, int max_lines, const char *filter, bool usb_only) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        char err[192];
        snprintf(err, sizeof(err), "[-] Cannot open %s: %s\n", path, strerror(errno));
        if (usb_only) usb_write(err); else print_msg(err);
        return;
    }
    char line[384];
    int count = 0;
    while (fgets(line, sizeof(line), fp) && count < max_lines) {
        if (filter && *filter && !strcasestr(line, filter))
            continue;
        if (usb_only) usb_write(line); else print_msg(line);
        count++;
    }
    fclose(fp);
}

static void dump_dmesg_filtered(int max_lines, const char *filter) {
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        usb_write("[-] Cannot open /dev/kmsg\n");
        return;
    }
    char buf[4096];
    char out[4160];
    int count = 0;
    while (count < max_lines) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) {
            if (errno == EPIPE || errno == EINVAL)
                continue; /* ring buffer wrapped or line too long; skip to next */
            break;
        }
        if (n == 0)
            break;
        buf[n] = '\0';
        char *semi = strchr(buf, ';');
        const char *msg = semi ? semi + 1 : buf;
        if (filter && *filter && !strcasestr(msg, filter))
            continue;

        unsigned long long usec = 0;
        unsigned int prio = 0;
        unsigned long long seq = 0;
        if (semi && sscanf(buf, "%u,%llu,%llu", &prio, &seq, &usec) == 3) {
            snprintf(out, sizeof(out), "[%5llu.%06llu] %s",
                     usec / 1000000ULL, usec % 1000000ULL, msg);
            usb_write(out);
        } else {
            usb_write(msg);
        }
        count++;
    }
    close(fd);
}

static bool setup_usb_gadget(void) {
    static bool gadget_created = false;
    char udc_name[96] = {0};

    if (!find_udc(udc_name, sizeof(udc_name)))
        return false;

    if (!gadget_created) {
        mkdir("/sys/kernel/config", 0755);
        mount("none", "/sys/kernel/config", "configfs", 0, NULL);

        mkdir("/sys/kernel/config/usb_gadget/g1", 0755);
        write_str("/sys/kernel/config/usb_gadget/g1/idVendor", "0x18d1\n");
        write_str("/sys/kernel/config/usb_gadget/g1/idProduct", "0xd001\n");
        mkdir("/sys/kernel/config/usb_gadget/g1/strings/0x409", 0755);
        write_str("/sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber", "sailfish-6.18\n");
        write_str("/sys/kernel/config/usb_gadget/g1/strings/0x409/manufacturer", "Google\n");
        write_str("/sys/kernel/config/usb_gadget/g1/strings/0x409/product", "Pixel Sailfish Linux 6.18\n");

        mkdir("/sys/kernel/config/usb_gadget/g1/configs/c.1", 0755);
        mkdir("/sys/kernel/config/usb_gadget/g1/configs/c.1/strings/0x409", 0755);
        write_str("/sys/kernel/config/usb_gadget/g1/configs/c.1/strings/0x409/configuration", "CDC-ACM\n");
        write_str("/sys/kernel/config/usb_gadget/g1/configs/c.1/MaxPower", "500\n");

        mkdir("/sys/kernel/config/usb_gadget/g1/functions/acm.gs0", 0755);
        symlink("/sys/kernel/config/usb_gadget/g1/functions/acm.gs0",
                "/sys/kernel/config/usb_gadget/g1/configs/c.1/acm.gs0");
        gadget_created = true;
    }

    char udc_nl[112];
    snprintf(udc_nl, sizeof(udc_nl), "%s\n", udc_name);
    write_str("/sys/kernel/config/usb_gadget/g1/UDC", udc_nl);

    char msg[192];
    snprintf(msg, sizeof(msg), "[+] Bound USB CDC-ACM gadget (18d1:d001) to UDC: %s\n", udc_name);
    print_msg(msg);
    return true;
}

static void handle_usb_cmd(char *cmd) {
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    if (*cmd == '\0') {
        usb_write("sailfish# ");
        return;
    }

    char log_line[192];
    snprintf(log_line, sizeof(log_line), "[sailfish-shell] cmd: %s\n", cmd);
    print_msg(log_line);

    if (strcmp(cmd, "help") == 0) {
        usb_write("Commands:\n"
                  "  help              - Show this help\n"
                  "  uname             - Kernel version & machine info\n"
                  "  uptime            - System uptime (/proc/uptime)\n"
                  "  dmesg [filter]    - Dump kernel log (optional substring filter)\n"
                  "  ramoops [filter]  - Dump /sys/fs/pstore/console-ramoops-0\n"
                  "  usb               - Show UDC status & speed\n"
                  "  parts             - Show /proc/partitions\n"
                  "  mounts            - Show /proc/mounts\n"
                  "  ls [dir]          - List directory entries\n"
                  "  cat <file>        - Print file contents (up to 2000 lines)\n"
                  "  mount_sys         - Mount sda33 (/mnt/system_a) & sda31 (/mnt/vendor_a) ro\n"
                  "  bootloader        - Reboot directly to fastboot\n"
                  "  recovery          - Reboot directly to recovery (TWRP)\n"
                  "  reboot            - Normal reboot\n");
    } else if (strcmp(cmd, "uname") == 0) {
        struct utsname u;
        if (uname(&u) == 0) {
            char out[320];
            snprintf(out, sizeof(out), "%s %s %s %s %s\n",
                     u.sysname, u.nodename, u.release, u.version, u.machine);
            usb_write(out);
        }
    } else if (strcmp(cmd, "uptime") == 0) {
        dump_file_lines("/proc/uptime", 5, NULL, true);
    } else if (strcmp(cmd, "dmesg") == 0 || strncmp(cmd, "dmesg ", 6) == 0) {
        const char *flt = (strlen(cmd) > 6) ? cmd + 6 : NULL;
        while (flt && *flt == ' ') flt++;
        dump_dmesg_filtered(4000, flt);
    } else if (strcmp(cmd, "ramoops") == 0 || strncmp(cmd, "ramoops ", 8) == 0) {
        const char *flt = (strlen(cmd) > 8) ? cmd + 8 : NULL;
        while (flt && *flt == ' ') flt++;
        dump_sysfs_dir("pstore", "/sys/fs/pstore", true);
        dump_file_lines("/sys/fs/pstore/console-ramoops-0", 4000, flt, true);
    } else if (strcmp(cmd, "usb") == 0) {
        char state[64], speed[64];
        get_udc_state(state, sizeof(state));
        read_first_line("/sys/class/udc/6a00000.usb/current_speed", speed, sizeof(speed));
        char out[192];
        snprintf(out, sizeof(out), "UDC=6a00000.usb state=%s speed=%s\n", state, speed);
        usb_write(out);
    } else if (strcmp(cmd, "parts") == 0) {
        dump_file_lines("/proc/partitions", 120, NULL, true);
    } else if (strcmp(cmd, "mounts") == 0) {
        dump_file_lines("/proc/mounts", 120, NULL, true);
    } else if (strcmp(cmd, "ls") == 0 || strncmp(cmd, "ls ", 3) == 0) {
        const char *dir = (strlen(cmd) > 3) ? cmd + 3 : "/";
        while (*dir == ' ') dir++;
        if (*dir == '\0') dir = "/";
        dump_sysfs_dir("Dir", dir, true);
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        const char *path = cmd + 4;
        while (*path == ' ') path++;
        dump_file_lines(path, 2000, NULL, true);
    } else if (strcmp(cmd, "mount_sys") == 0) {
        mkdir("/mnt", 0755);
        mkdir("/mnt/system_a", 0755);
        mkdir("/mnt/vendor_a", 0755);
        int r1 = mount("/dev/sda33", "/mnt/system_a", "ext4", MS_RDONLY, NULL);
        int r2 = mount("/dev/sda31", "/mnt/vendor_a", "ext4", MS_RDONLY, NULL);
        char out[220];
        snprintf(out, sizeof(out), "mount sda33->/mnt/system_a: %d (%s)\nmount sda31->/mnt/vendor_a: %d (%s)\n",
                 r1, r1 == 0 ? "ok" : strerror(errno),
                 r2, r2 == 0 ? "ok" : strerror(errno));
        print_msg(out);
    } else if (strcmp(cmd, "bootloader") == 0) {
        print_msg("[sailfish-shell] Rebooting to bootloader (fastboot)...\n");
        sync();
        usleep(100000);
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                LINUX_REBOOT_CMD_RESTART2, "bootloader");
    } else if (strcmp(cmd, "recovery") == 0) {
        print_msg("[sailfish-shell] Rebooting to recovery...\n");
        sync();
        usleep(100000);
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                LINUX_REBOOT_CMD_RESTART2, "recovery");
    } else if (strcmp(cmd, "reboot") == 0) {
        print_msg("[sailfish-shell] Rebooting...\n");
        sync();
        usleep(100000);
        reboot(RB_AUTOBOOT);
    } else {
        usb_write("Unknown command. Type 'help' for available commands.\n");
    }

    usb_write("sailfish# ");
}

static long monotonic_sec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return ts.tv_sec;
    return 0;
}

int main(void) {
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);

    mount("none", "/dev", "devtmpfs", 0, NULL);
    mount("none", "/proc", "proc", 0, NULL);
    mount("none", "/sys", "sysfs", 0, NULL);
    mkdir("/sys/fs/pstore", 0755);
    mount("pstore", "/sys/fs/pstore", "pstore", 0, NULL);

    print_msg("\n======================================================\n");
    print_msg("  Google Pixel (sailfish) Linux 6.18 Bring-up         \n");
    print_msg("  Android 17 Common Kernel (ACK) - MSM8996 Pro        \n");
    print_msg("======================================================\n");
    print_msg("[+] Kernel 6.18 booted to userspace /init!\n");
    print_msg("[+] Waiting for RPM, UFS (624000.ufshc), and USB (6a00000.usb)...\n");

    char udc_buf[96];
    for (int i = 0; i < 20; i++) {
        if (has_non_loop_block() && find_udc(udc_buf, sizeof(udc_buf)))
            break;
        usleep(250000);
    }

    bool usb_bound = setup_usb_gadget();
    if (!usb_bound)
        print_msg("[-] UDC not ready yet; will retry in loop\n");

    /* Try mounting stock/installed system_a & vendor_a read-only as a sanity test */
    mkdir("/mnt", 0755);
    mkdir("/mnt/system_a", 0755);
    mkdir("/mnt/vendor_a", 0755);
    int sys_rc = mount("/dev/sda33", "/mnt/system_a", "ext4", MS_RDONLY, NULL);
    int ven_rc = mount("/dev/sda31", "/mnt/vendor_a", "ext4", MS_RDONLY, NULL);
    {
        char mmsg[200];
        snprintf(mmsg, sizeof(mmsg),
                 "[+] UFS ext4 probe: sda33(system_a)=%s, sda31(vendor_a)=%s\n",
                 sys_rc == 0 ? "MOUNTED(ro)" : strerror(errno),
                 ven_rc == 0 ? "MOUNTED(ro)" : strerror(errno));
        print_msg(mmsg);
        if (ven_rc == 0)
            dump_sysfs_dir("vendor_a root", "/mnt/vendor_a", false);
    }

    print_msg("[+] USB serial shell waiting for host on /dev/ttyGS0 (/dev/ttyACM0)\n");
    print_msg("======================================================\n");

    char cmd_buf[256];
    size_t cmd_len = 0;
    long start_sec = monotonic_sec();
    long last_hb_sec = start_sec;

    while (1) {
        if (!usb_bound)
            usb_bound = setup_usb_gadget();

        open_ttygs_if_ready();

        if (g_ttygs_fd >= 0) {
            struct pollfd pfd;
            pfd.fd = g_ttygs_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;

            int prc = poll(&pfd, 1, 1000);
            if (prc > 0) {
                if (pfd.revents & POLLIN) {
                    char rx[64];
                    ssize_t n = read(g_ttygs_fd, rx, sizeof(rx));
                    if (n > 0) {
                        for (ssize_t i = 0; i < n; i++) {
                            char c = rx[i];
                            if (c == '\r' || c == '\n') {
                                usb_write("\r\n");
                                cmd_buf[cmd_len] = '\0';
                                handle_usb_cmd(cmd_buf);
                                cmd_len = 0;
                            } else if (c == 0x7f || c == '\b') {
                                if (cmd_len > 0) {
                                    cmd_len--;
                                    usb_write("\b \b");
                                }
                            } else if (c >= 32 && c < 127 && cmd_len + 1 < sizeof(cmd_buf)) {
                                cmd_buf[cmd_len++] = c;
                                char echo[2] = { c, '\0' };
                                usb_write(echo);
                            }
                        }
                    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        close_ttygs();
                        usleep(200000);
                    }
                }
                if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    /* Host reset USB or closed port; reopen cleanly */
                    close_ttygs();
                    usleep(200000);
                }
            }
        } else {
            usleep(250000);
        }

        long now_sec = monotonic_sec();
        if (now_sec - last_hb_sec >= 15) {
            last_hb_sec = now_sec;
            char udc_state[64];
            get_udc_state(udc_state, sizeof(udc_state));
            char hb[160];
            snprintf(hb, sizeof(hb),
                     "[sailfish-init] +%lds udc=%s ufs=%d sys_a=%s ven_a=%s\n",
                     now_sec - start_sec, udc_state, has_non_loop_block() ? 1 : 0,
                     sys_rc == 0 ? "ok" : "err", ven_rc == 0 ? "ok" : "err");
            print_msg(hb);
        }
    }
    return 0;
}
