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
#include <linux/input.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
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

static void print_msg_local(const char *msg) {
    const char *devs[] = { "/dev/tty0", "/dev/kmsg", "/dev/pmsg0" };
    for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        int fd = open(devs[i], O_WRONLY | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) {
            write(fd, msg, strlen(msg));
            close(fd);
        }
    }
}

static void print_msg(const char *msg) {
    print_msg_local(msg);
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

static void dump_file_raw(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        char err[192];
        snprintf(err, sizeof(err), "[-] Cannot open %s: %s\n", path, strerror(errno));
        usb_write(err);
        return;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t rc = write(g_ttygs_fd, buf + off, n - off);
            if (rc > 0) {
                off += rc;
            } else if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                usleep(2000);
            } else {
                break;
            }
        }
    }
    close(fd);
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

static long monotonic_sec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return ts.tv_sec;
    return 0;
}

static void test_input_events(int timeout_sec) {
    char out[192];
    int fds[16];
    int num_fds = 0;

    dump_sysfs_dir("Input devices", "/dev/input", true);
    DIR *sd = opendir("/sys/class/input");
    if (sd) {
        struct dirent *dir;
        while ((dir = readdir(sd)) != NULL) {
            if (strncmp(dir->d_name, "input", 5) == 0) {
                char npath[128], name[128];
                snprintf(npath, sizeof(npath), "/sys/class/input/%s/name", dir->d_name);
                read_first_line(npath, name, sizeof(name));
                char l[192];
                snprintf(l, sizeof(l), "  %s: %s\n", dir->d_name, name);
                usb_write(l);
            }
        }
        closedir(sd);
    }

    DIR *d = opendir("/dev/input");
    if (!d) {
        usb_write("[-] /dev/input not found\n");
        return;
    }
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL && num_fds < 16) {
        if (strncmp(dir->d_name, "event", 5) == 0) {
            char path[64];
            snprintf(path, sizeof(path), "/dev/input/%s", dir->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                char name[128] = "unknown";
                ioctl(fd, EVIOCGNAME(sizeof(name)), name);
                snprintf(out, sizeof(out), "[+] Listening on %s (%s)\n", path, name);
                usb_write(out);
                fds[num_fds++] = fd;
            }
        }
    }
    closedir(d);

    if (num_fds == 0) {
        usb_write("[-] No /dev/input/event* devices found\n");
        return;
    }

    snprintf(out, sizeof(out), "[+] Touch screen or press buttons now (monitoring for %ds)...\n", timeout_sec);
    usb_write(out);

    long start = monotonic_sec();
    while (monotonic_sec() - start < timeout_sec) {
        struct pollfd pfds[16];
        for (int i = 0; i < num_fds; i++) {
            pfds[i].fd = fds[i];
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }
        int prc = poll(pfds, num_fds, 500);
        if (prc > 0) {
            for (int i = 0; i < num_fds; i++) {
                if (pfds[i].revents & POLLIN) {
                    struct input_event ev[16];
                    ssize_t n = read(fds[i], ev, sizeof(ev));
                    if (n > 0) {
                        size_t count = n / sizeof(struct input_event);
                        for (size_t j = 0; j < count; j++) {
                            snprintf(out, sizeof(out), "ev%d: type=0x%x code=0x%x val=%d\n",
                                     i, ev[j].type, ev[j].code, ev[j].value);
                            usb_write(out);
                        }
                    }
                }
            }
        }
    }
    for (int i = 0; i < num_fds; i++)
        close(fds[i]);
    usb_write("[+] Done monitoring input.\n");
}

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

#define FDT_MAGIC       0xd00dfeed
#define FDT_BEGIN_NODE  0x00000001
#define FDT_END_NODE    0x00000002
#define FDT_PROP        0x00000003
#define FDT_NOP         0x00000004
#define FDT_END         0x00000009

static bool is_fdt_string_list(const uint8_t *data, size_t len) {
    if (len == 0 || data[len - 1] != '\0') return false;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\0') continue;
        if (!isprint(data[i]) && data[i] != '\t' && data[i] != '\n')
            return false;
    }
    return true;
}

static void print_fdt_dump(const char *msg, bool to_usb) {
    if (to_usb) usb_write(msg);
    else print_msg(msg);
}

static void parse_and_dump_fdt_touch(const uint8_t *fdt, size_t fdt_size, bool to_usb) {
    if (fdt_size < sizeof(struct fdt_header)) return;
    const struct fdt_header *h = (const struct fdt_header *)fdt;
    if (__builtin_bswap32(h->magic) != FDT_MAGIC) return;

    uint32_t off_struct = __builtin_bswap32(h->off_dt_struct);
    uint32_t off_strings = __builtin_bswap32(h->off_dt_strings);
    uint32_t total = __builtin_bswap32(h->totalsize);

    if (total > fdt_size || off_struct >= fdt_size || off_strings >= fdt_size) return;

    const uint32_t *p = (const uint32_t *)(fdt + off_struct);
    const uint32_t *end = (const uint32_t *)(fdt + fdt_size);
    const char *strings = (const char *)(fdt + off_strings);

    int depth = 0;
    char node_names[16][64];

    char hdr[128];
    snprintf(hdr, sizeof(hdr), "[+] FDT Valid: total size=%u bytes\n", total);
    print_fdt_dump(hdr, to_usb);

    while (p < end) {
        uint32_t tag = __builtin_bswap32(*p++);
        if (tag == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t nlen = strlen(name);
            p += (nlen + 4) / 4;
            if (depth < 16) {
                snprintf(node_names[depth], sizeof(node_names[depth]), "%s", name[0] ? name : "/");
            }
            depth++;
        } else if (tag == FDT_END_NODE) {
            if (depth > 0) depth--;
        } else if (tag == FDT_PROP) {
            uint32_t len = __builtin_bswap32(*p++);
            uint32_t nameoff = __builtin_bswap32(*p++);
            const uint8_t *data = (const uint8_t *)p;
            p += (len + 3) / 4;
            const char *pname = strings + nameoff;

            bool match = false;
            for (int i = 0; i < depth; i++) {
                if (strcasestr(node_names[i], "syna") ||
                    strcasestr(node_names[i], "touch") ||
                    strcasestr(node_names[i], "7577000") ||
                    strcasestr(node_names[i], "i2c") ||
                    strcasestr(node_names[i], "pmx_ts") ||
                    strcasestr(node_names[i], "pmx_i2c") ||
                    strcasestr(node_names[i], "ts_mux") ||
                    strcasestr(node_names[i], "ts_int") ||
                    strcasestr(node_names[i], "ts_reset") ||
                    strcasestr(node_names[i], "ts_rst") ||
                    strcasestr(node_names[i], "ts_active") ||
                    strcasestr(node_names[i], "rpm-pm8994") ||
                    strcasestr(node_names[i], "rpm-pmi8994")) {
                    match = true;
                }
            }
            if (!match && len == 4 && (strcmp(pname, "phandle") == 0 || strcmp(pname, "linux,phandle") == 0)) {
                uint32_t ph = __builtin_bswap32(*(const uint32_t *)data);
                if (ph == 0x20d || ph == 0x20e || ph == 0x20f || ph == 0x210 || ph == 0x211 || ph == 0x212 || ph == 0x213)
                    match = true;
            }
            if (match || strcasestr(pname, "syna") || strcasestr(pname, "touch")) {
                char fullpath[256] = "";
                for (int i = 0; i < depth; i++) {
                    if (i > 0 && strcmp(node_names[i], "/") != 0) strcat(fullpath, "/");
                    strcat(fullpath, node_names[i]);
                }
                char lbuf[512];
                int lpos = snprintf(lbuf, sizeof(lbuf), "  %s -> %s = ", fullpath, pname);
                bool is_cells = strstr(pname, "gpio") || strstr(pname, "interrupt") ||
                                strcmp(pname, "reg") == 0 || strstr(pname, "pinctrl-") ||
                                strstr(pname, "clocks") || strstr(pname, "dmas");
                if (len == 0) {
                    lpos += snprintf(lbuf + lpos, sizeof(lbuf) - lpos, "(empty/boolean)\n");
                } else if (!is_cells && is_fdt_string_list(data, len)) {
                    lpos += snprintf(lbuf + lpos, sizeof(lbuf) - lpos, "\"");
                    for (size_t k = 0; k < len; k++) {
                        if (data[k] == '\0') {
                            if (k + 1 < len) lpos += snprintf(lbuf + lpos, sizeof(lbuf) - lpos, "\", \"");
                        } else {
                            if (lpos + 2 < (int)sizeof(lbuf)) lbuf[lpos++] = (char)data[k];
                        }
                    }
                    lpos += snprintf(lbuf + lpos, sizeof(lbuf) - lpos, "\"\n");
                } else if (len % 4 == 0) {
                    lpos += snprintf(lbuf + lpos, sizeof(lbuf) - lpos, "<");
                    for (size_t k = 0; k < len && lpos + 16 < (int)sizeof(lbuf); k += 4) {
                        uint32_t val = __builtin_bswap32(*(const uint32_t *)(data + k));
                        lpos += snprintf(lbuf + lpos, sizeof(lbuf) - lpos, "0x%x%s",
                                         val, (k + 4 < len) ? " " : "");
                    }
                    lpos += snprintf(lbuf + lpos, sizeof(lbuf) - lpos, ">\n");
                } else {
                    lpos += snprintf(lbuf + lpos, sizeof(lbuf) - lpos, "[");
                    for (size_t k = 0; k < len && lpos + 8 < (int)sizeof(lbuf); k++) {
                        lpos += snprintf(lbuf + lpos, sizeof(lbuf) - lpos, "%02x%s",
                                         data[k], (k + 1 < len) ? " " : "");
                    }
                    lpos += snprintf(lbuf + lpos, sizeof(lbuf) - lpos, "]\n");
                }
                print_fdt_dump(lbuf, to_usb);
            }
        } else if (tag == FDT_END) {
            break;
        }
    }
}

static void dump_base64_dtb(const uint8_t *dtb, size_t total, bool to_usb) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    print_fdt_dump("\n---START_DTB_B64---\n", to_usb);
    char line[128];
    int line_len = 0;
    for (size_t i = 0; i < total; i += 3) {
        uint32_t octet_a = dtb[i];
        uint32_t octet_b = (i + 1 < total) ? dtb[i + 1] : 0;
        uint32_t octet_c = (i + 2 < total) ? dtb[i + 2] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        line[line_len++] = b64[(triple >> 18) & 0x3f];
        line[line_len++] = b64[(triple >> 12) & 0x3f];
        line[line_len++] = (i + 1 < total) ? b64[(triple >> 6) & 0x3f] : '=';
        line[line_len++] = (i + 2 < total) ? b64[triple & 0x3f] : '=';
        if (line_len >= 76) {
            line[line_len++] = '\n';
            line[line_len] = '\0';
            print_fdt_dump(line, to_usb);
            line_len = 0;
        }
    }
    if (line_len > 0) {
        line[line_len++] = '\n';
        line[line_len] = '\0';
        print_fdt_dump(line, to_usb);
    }
    print_fdt_dump("---END_DTB_B64---\n", to_usb);
}

static void scan_sda_for_dtb(bool to_usb, bool dump_b64) {
    DIR *d = opendir("/sys/block/sda");
    if (!d) {
        print_fdt_dump("[-] /sys/block/sda not found\n", to_usb);
        return;
    }
    char boot_dev[64] = "";
    struct dirent *de;
    print_fdt_dump("[+] Partition Table (/sys/block/sda):\n", to_usb);
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "sda", 3) == 0 && isdigit(de->d_name[3])) {
            char uev_path[128], uev[256];
            snprintf(uev_path, sizeof(uev_path), "/sys/block/sda/%s/uevent", de->d_name);
            FILE *f = fopen(uev_path, "r");
            if (f) {
                while (fgets(uev, sizeof(uev), f)) {
                    if (strncmp(uev, "PARTNAME=", 9) == 0) {
                        char *nl = strchr(uev, '\n'); if (nl) *nl = '\0';
                        char msg[128];
                        snprintf(msg, sizeof(msg), "    %s: %s\n", de->d_name, uev + 9);
                        print_fdt_dump(msg, to_usb);
                        if (strcmp(uev + 9, "boot_a") == 0 || strcmp(uev + 9, "boot_b") == 0) {
                            if (!boot_dev[0] || strcmp(uev + 9, "boot_a") == 0) {
                                snprintf(boot_dev, sizeof(boot_dev), "/dev/%s", de->d_name);
                            }
                        }
                    }
                }
                fclose(f);
            }
        }
    }
    closedir(d);

    if (!boot_dev[0]) {
        strcpy(boot_dev, "/dev/sda35");
    }

    char out[128];
    snprintf(out, sizeof(out), "[+] Scanning %s for stock DTB...\n", boot_dev);
    print_fdt_dump(out, to_usb);

    int fd = open(boot_dev, O_RDONLY);
    if (fd < 0) {
        snprintf(out, sizeof(out), "[-] Failed to open %s: %s\n", boot_dev, strerror(errno));
        print_fdt_dump(out, to_usb);
        return;
    }

    size_t chunk_sz = 256 * 1024;
    uint8_t *buf = malloc(chunk_sz + 4096);
    if (!buf) { close(fd); return; }

    off_t pos = 0;
    int dtb_count = 0;

    while (pos < 32 * 1024 * 1024) {
        if (lseek(fd, pos, SEEK_SET) < 0) break;
        ssize_t n = read(fd, buf, chunk_sz);
        if (n <= 16) break;

        for (ssize_t i = 0; i < n - 4; i += 4) {
            if (*(uint32_t *)(buf + i) == __builtin_bswap32(FDT_MAGIC)) {
                uint32_t total = __builtin_bswap32(*(uint32_t *)(buf + i + 4));
                if (total >= 4096 && total <= 2000000) {
                    off_t dtb_pos = pos + i;
                    snprintf(out, sizeof(out), "[+] Found DTB #%d at offset 0x%lx (size %u bytes)\n",
                             ++dtb_count, (long)dtb_pos, total);
                    print_fdt_dump(out, to_usb);

                    uint8_t *dtb = malloc(total);
                    if (dtb) {
                        lseek(fd, dtb_pos, SEEK_SET);
                        if (read(fd, dtb, total) == (ssize_t)total) {
                            if (dump_b64)
                                dump_base64_dtb(dtb, total, to_usb);
                            else
                                parse_and_dump_fdt_touch(dtb, total, to_usb);
                        }
                        free(dtb);
                    }
                    goto scan_done;
                }
            }
        }
        pos += chunk_sz - 4096;
    }

scan_done:
    free(buf);
    close(fd);
    print_fdt_dump("[+] Done scanning for DTB.\n", to_usb);
}

static void scan_i2c_bus(void) {
    int fd = open("/dev/i2c-0", O_RDWR);
    if (fd < 0) {
        char err[128];
        snprintf(err, sizeof(err), "[-] Cannot open /dev/i2c-0: %s\n", strerror(errno));
        usb_write(err);
        return;
    }
    usb_write("[+] Scanning /dev/i2c-0 (0x03..0x77):\n");
    int found = 0;
    for (int addr = 0x03; addr <= 0x77; addr++) {
        if (ioctl(fd, 0x0703 /* I2C_SLAVE */, addr) < 0)
            continue;
        uint8_t byte = 0;
        if (read(fd, &byte, 1) == 1) {
            char out[128];
            snprintf(out, sizeof(out), "  [ACK] Device responded at 0x%02x (byte=0x%02x)\n", addr, byte);
            usb_write(out);
            found++;
        }
    }
    char done[64];
    snprintf(done, sizeof(done), "[+] Done I2C scan: %d device(s) found.\n", found);
    usb_write(done);
    close(fd);
}

static void test_touch_hardware(void) {
    char out[192];
    usb_write("[+] Interactive Touch Hardware Diagnostics\n");

    int i2c_fd = open("/dev/i2c-0", O_RDWR);
    if (i2c_fd < 0) {
        snprintf(out, sizeof(out), "[-] Cannot open /dev/i2c-0: %s\n", strerror(errno));
        usb_write(out);
        return;
    }

    int chip_fd = open("/dev/gpiochip0", O_RDWR);
    if (chip_fd < 0) {
        snprintf(out, sizeof(out), "[-] Cannot open /dev/gpiochip0: %s\n", strerror(errno));
        usb_write(out);
        close(i2c_fd);
        return;
    }

    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    req.offsets[0] = 80; /* switch: Low=AP, High=SLPI */
    req.offsets[1] = 89; /* reset: Low=Reset, High=Run */
    req.num_lines = 2;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    strcpy(req.consumer, "touch_test");

    int rc = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req);
    if (rc < 0) {
        snprintf(out, sizeof(out), "[-] Failed to request GPIO 80/89: %s\n", strerror(errno));
        usb_write(out);
        close(chip_fd);
        close(i2c_fd);
        return;
    }

    usb_write("[+] Acquired GPIO 80 (switch) and GPIO 89 (reset) via /dev/gpiochip0\n");

    /* Check physical state of SDA (gpio47) and SCL (gpio48) */
    struct gpio_v2_line_request bus_req;
    memset(&bus_req, 0, sizeof(bus_req));
    bus_req.offsets[0] = 47;
    bus_req.offsets[1] = 48;
    bus_req.num_lines = 2;
    bus_req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
    strcpy(bus_req.consumer, "touch_bus_test");
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &bus_req) == 0) {
        struct gpio_v2_line_values bvals;
        memset(&bvals, 0, sizeof(bvals));
        bvals.mask = 0x3;
        ioctl(bus_req.fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &bvals);
        snprintf(out, sizeof(out), "[+] Physical I2C3 bus lines: SDA(gpio47)=%d, SCL(gpio48)=%d\n",
                 (int)(bvals.bits & 1), (int)((bvals.bits >> 1) & 1));
        usb_write(out);
        close(bus_req.fd);
    } else {
        snprintf(out, sizeof(out), "[-] Cannot read SDA/SCL via gpiochip0: %s\n", strerror(errno));
        usb_write(out);
    }

    int test_switches[2] = { 0, 1 };

    for (int t = 0; t < 2; t++) {
        int sw = test_switches[t];

        snprintf(out, sizeof(out), "\n[Test %d] switch(gpio80)=%d: Performing hardware reset pulse (reset 1->0->1)...\n",
                 t + 1, sw);
        usb_write(out);

        /* Set switch and assert reset low */
        struct gpio_v2_line_values vals;
        memset(&vals, 0, sizeof(vals));
        vals.mask = 0x3;
        vals.bits = ((uint64_t)sw & 1) | (0ULL << 1); /* rst=0 */
        ioctl(req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals);
        usleep(10000); /* 10ms reset pulse */

        /* Release reset high */
        vals.bits = ((uint64_t)sw & 1) | (1ULL << 1); /* rst=1 */
        ioctl(req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals);
        usleep(100000); /* 100ms for Synaptics boot */

        ioctl(i2c_fd, 0x0703, 0x20);
        uint8_t tx[2] = { 0xff, 0x00 };
        ssize_t nw = write(i2c_fd, tx, 2);
        uint8_t rx = 0;
        ssize_t nr = read(i2c_fd, &rx, 1);

        snprintf(out, sizeof(out), "  Addr 0x20: write_2b=%zd (%s), read_1b=%zd (val=0x%02x, %s)\n",
                 nw, nw == 2 ? "ACK" : strerror(errno),
                 nr, rx, nr == 1 ? "ACK" : strerror(errno));
        usb_write(out);

        int found = 0;
        for (int a = 0x03; a <= 0x77; a++) {
            ioctl(i2c_fd, 0x0703, a);
            uint8_t b = 0;
            if (read(i2c_fd, &b, 1) == 1) {
                snprintf(out, sizeof(out), "  >>> Found ACK at address 0x%02x (byte=0x%02x)!\n", a, b);
                usb_write(out);
                found++;
            }
        }
        if (found == 0)
            usb_write("  Bus scan: 0 responsive devices\n");
    }

    struct gpio_v2_line_values final_vals;
    memset(&final_vals, 0, sizeof(final_vals));
    final_vals.mask = 0x3;
    final_vals.bits = (1 << 1) | (0 << 0);
    ioctl(req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &final_vals);

    close(req.fd);
    close(chip_fd);
    close(i2c_fd);
    usb_write("\n[+] Done touch hardware test.\n");
}

static void dump_battery_info(void) {
    char out[256];
    usb_write("[+] Power Supply Subsystem (/sys/class/power_supply):\n");
    dump_sysfs_dir("power_supply", "/sys/class/power_supply", true);

    const char *bat = (access("/sys/class/power_supply/smbb-bif", F_OK) == 0) ?
                      "/sys/class/power_supply/smbb-bif" : "/sys/class/power_supply/battery";
    const char *usb = (access("/sys/class/power_supply/smbb-usbin", F_OK) == 0) ?
                      "/sys/class/power_supply/smbb-usbin" : "/sys/class/power_supply/usb";

    char val[64], path[128];
    snprintf(path, sizeof(path), "%s/status", bat);
    if (read_first_line(path, val, sizeof(val))) {
        snprintf(out, sizeof(out), "  Battery Status: %s\n", val);
        usb_write(out);
    }
    snprintf(path, sizeof(path), "%s/capacity", bat);
    if (read_first_line(path, val, sizeof(val))) {
        snprintf(out, sizeof(out), "  Battery Capacity: %s%%\n", val);
        usb_write(out);
    }
    snprintf(path, sizeof(path), "%s/voltage_now", bat);
    if (read_first_line(path, val, sizeof(val))) {
        snprintf(out, sizeof(out), "  Battery Voltage: %s uV\n", val);
        usb_write(out);
    }
    snprintf(path, sizeof(path), "%s/health", bat);
    if (read_first_line(path, val, sizeof(val))) {
        snprintf(out, sizeof(out), "  Battery Health: %s\n", val);
        usb_write(out);
    }
    snprintf(path, sizeof(path), "%s/online", usb);
    if (read_first_line(path, val, sizeof(val))) {
        snprintf(out, sizeof(out), "  USB Charger Online: %s\n", val);
        usb_write(out);
    }
    snprintf(path, sizeof(path), "%s/charge_control_limit", usb);
    if (read_first_line(path, val, sizeof(val))) {
        snprintf(out, sizeof(out), "  USB Input Current Limit: %s uA\n", val);
        usb_write(out);
    }
}

static void dump_display_info(void) {
    char out[256];
    usb_write("[+] DRM / KMS Subsystem (/sys/class/drm):\n");
    dump_sysfs_dir("drm", "/sys/class/drm", true);
    usb_write("[+] Backlight Subsystem (/sys/class/backlight):\n");
    dump_sysfs_dir("backlight", "/sys/class/backlight", true);

    char val[64];
    DIR *d = opendir("/sys/class/backlight");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char path[128];
            snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness", de->d_name);
            if (read_first_line(path, val, sizeof(val))) {
                snprintf(out, sizeof(out), "  Backlight %s brightness: %s", de->d_name, val);
                snprintf(path, sizeof(path), "/sys/class/backlight/%s/max_brightness", de->d_name);
                char max_v[64];
                if (read_first_line(path, max_v, sizeof(max_v))) {
                    snprintf(out + strlen(out), sizeof(out) - strlen(out), " / %s\n", max_v);
                } else {
                    strcat(out, "\n");
                }
                usb_write(out);
            }
        }
        closedir(d);
    }
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
    print_msg_local(log_line);

    if (strcmp(cmd, "help") == 0) {
        usb_write("Commands:\n"
                  "  help              - Show this help\n"
                  "  uname             - Kernel version & machine info\n"
                  "  uptime            - System uptime (/proc/uptime)\n"
                  "  dmesg [filter]    - Dump kernel log (optional substring filter)\n"
                  "  ramoops [filter]  - Dump /sys/fs/pstore/console-ramoops-0\n"
                  "  input / touch     - List input devices & monitor events for 10s\n"
                  "  battery           - Show battery capacity, voltage, and charger state\n"
                  "  display / drm     - Show DRM/KMS connectors, CRTCs, and backlight\n"
                  "  touch_test        - Sweep switch & reset GPIOs and test I2C ACK\n"
                  "  i2c / i2c_scan    - Scan /dev/i2c-0 for responsive slave addresses\n"
                  "  usb               - Show UDC status & speed\n"
                  "  parts             - Show /proc/partitions\n"
                  "  dtb / stock_dtb   - Scan stock boot_a for factory DTB & touch node\n"
                  "  dump_dtb          - Dump stock factory DTB as base64\n"
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
        if (flt && *flt)
            dump_file_lines("/sys/fs/pstore/console-ramoops-0", 4000, flt, true);
        else
            dump_file_raw("/sys/fs/pstore/console-ramoops-0");
    } else if (strcmp(cmd, "input") == 0 || strcmp(cmd, "touch") == 0) {
        test_input_events(10);
    } else if (strcmp(cmd, "battery") == 0 || strcmp(cmd, "power") == 0) {
        dump_battery_info();
    } else if (strcmp(cmd, "display") == 0 || strcmp(cmd, "drm") == 0) {
        dump_display_info();
    } else if (strcmp(cmd, "usb") == 0) {
        char state[64], speed[64];
        get_udc_state(state, sizeof(state));
        read_first_line("/sys/class/udc/6a00000.usb/current_speed", speed, sizeof(speed));
        char out[192];
        snprintf(out, sizeof(out), "UDC=6a00000.usb state=%s speed=%s\n", state, speed);
        usb_write(out);
    } else if (strcmp(cmd, "parts") == 0) {
        dump_file_lines("/proc/partitions", 120, NULL, true);
    } else if (strcmp(cmd, "dtb") == 0 || strcmp(cmd, "stock_dtb") == 0) {
        scan_sda_for_dtb(true, false);
    } else if (strcmp(cmd, "dump_dtb") == 0) {
        scan_sda_for_dtb(true, true);
    } else if (strcmp(cmd, "touch_test") == 0 || strcmp(cmd, "touch_probe") == 0) {
        test_touch_hardware();
    } else if (strcmp(cmd, "i2c") == 0 || strcmp(cmd, "i2c_scan") == 0) {
        scan_i2c_bus();
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
        dump_file_raw(path);
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
                    static char last_c = 0;
                    ssize_t n = read(g_ttygs_fd, rx, sizeof(rx));
                    if (n > 0) {
                        for (ssize_t i = 0; i < n; i++) {
                            char c = rx[i];
                            if (c == '\n' && last_c == '\r') {
                                last_c = c;
                                continue;
                            }
                            last_c = c;
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
