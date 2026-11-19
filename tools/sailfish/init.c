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
#include <drm/drm.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <sched.h>
#include <linux/fscrypt.h>

static int g_ttygs_fd = -1;
static bool g_usb_bound = false;
static bool g_data_persistent = false;

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
            "======================================================\r\n";
        write(g_ttygs_fd, banner, strlen(banner));
        if (access("/sys/fs/pstore/console-ramoops-0", F_OK) == 0 ||
            access("/sys/fs/pstore/dmesg-ramoops-0", F_OK) == 0) {
            const char *pmsg = "[!] Previous crash log found in /sys/fs/pstore (run 'ramoops')\r\n";
            write(g_ttygs_fd, pmsg, strlen(pmsg));
        }
        write(g_ttygs_fd, "sailfish# ", 10);
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

static void print_msg_local(const char *msg) {
    if (!msg) return;
    const char *devs[] = { "/dev/kmsg", "/dev/pmsg0" };
    for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        int fd = open(devs[i], O_WRONLY | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) {
            write(fd, msg, strlen(msg));
            close(fd);
        }
    }
}

static void usb_write(const char *msg) {
    usb_write_ex(msg, true);
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

static void dump_file_tail(const char *path, off_t bytes) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        char err[192];
        snprintf(err, sizeof(err), "[-] Cannot open %s: %s\n", path, strerror(errno));
        usb_write(err);
        return;
    }
    char *buf = malloc(bytes);
    if (!buf) {
        close(fd);
        return;
    }
    size_t total = 0;
    char chunk[2048];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        if (total + (size_t)n <= (size_t)bytes) {
            memcpy(buf + total, chunk, n);
            total += n;
        } else {
            size_t shift = (total + n) - bytes;
            memmove(buf, buf + shift, bytes - n);
            memcpy(buf + bytes - n, chunk, n);
            total = bytes;
        }
    }
    close(fd);

    size_t off = 0;
    while (off < total) {
        ssize_t rc = write(g_ttygs_fd, buf + off, total - off);
        if (rc > 0) {
            off += rc;
        } else if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(2000);
        } else {
            break;
        }
    }
    free(buf);
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

static void dump_dmesg_any(int max_lines, const char *const *filters, int nfilters) {
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
        if (filters && nfilters > 0) {
            bool matched = false;
            for (int i = 0; i < nfilters; i++) {
                if (filters[i] && filters[i][0] && strcasestr(msg, filters[i])) {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                continue;
        }

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

static void dump_dmesg_filtered(int max_lines, const char *filter) {
    if (filter && *filter)
        dump_dmesg_any(max_lines, &filter, 1);
    else
        dump_dmesg_any(max_lines, NULL, 0);
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

static void dump_base64_buf(const char *label, const uint8_t *data, size_t total, bool to_usb) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char hdr[160];
    snprintf(hdr, sizeof(hdr), "\n---START_%s_B64 (%zu bytes)---\n", label, total);
    print_fdt_dump(hdr, to_usb);
    char line[128];
    int line_len = 0;
    for (size_t i = 0; i < total; i += 3) {
        uint32_t octet_a = data[i];
        uint32_t octet_b = (i + 1 < total) ? data[i + 1] : 0;
        uint32_t octet_c = (i + 2 < total) ? data[i + 2] : 0;
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
    snprintf(hdr, sizeof(hdr), "---END_%s_B64---\n", label);
    print_fdt_dump(hdr, to_usb);
}

static void dump_base64_dtb(const uint8_t *dtb, size_t total, bool to_usb) {
    dump_base64_buf("DTB", dtb, total, to_usb);
}

static void dump_firmware_file_b64(const char *name) {
    char path[160];
    snprintf(path, sizeof(path), "/mnt/vendor_a/firmware/%s", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        char err[160];
        snprintf(err, sizeof(err), "[-] Cannot open %s: %s\n", path, strerror(errno));
        usb_write(err);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        close(fd);
        usb_write("[-] Bad file size\n");
        return;
    }
    uint8_t *buf = malloc(st.st_size);
    if (!buf) {
        close(fd);
        usb_write("[-] Out of memory\n");
        return;
    }
    ssize_t rd = read(fd, buf, st.st_size);
    close(fd);
    if (rd != st.st_size) {
        free(buf);
        usb_write("[-] Read error\n");
        return;
    }
    dump_base64_buf(name, buf, st.st_size, true);
    free(buf);
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
    snprintf(path, sizeof(path), "%s/present", bat);
    if (read_first_line(path, val, sizeof(val))) {
        snprintf(out, sizeof(out), "  Battery Present: %s\n", val);
        usb_write(out);
    }
    snprintf(path, sizeof(path), "%s/charge_type", bat);
    if (read_first_line(path, val, sizeof(val))) {
        snprintf(out, sizeof(out), "  Battery Charge Type: %s\n", val);
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
    snprintf(path, sizeof(path), "%s/device/smbchg_regs", usb);
    if (access(path, F_OK) == 0) {
        usb_write("  SMBCHG Regs: ");
        dump_file_lines(path, 10, NULL, true);
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

static void dump_thermal_perf(void) {
    char out[512];

    usb_write("[+] Thermal Zones (/sys/class/thermal):\n");
    DIR *td = opendir("/sys/class/thermal");
    if (td) {
        struct dirent *de;
        while ((de = readdir(td)) != NULL) {
            if (strncmp(de->d_name, "thermal_zone", 12) != 0)
                continue;
            char tp[300], vp[300], type[64] = {0}, temp[32] = {0};
            snprintf(tp, sizeof(tp), "/sys/class/thermal/%s/type", de->d_name);
            snprintf(vp, sizeof(vp), "/sys/class/thermal/%s/temp", de->d_name);
            read_first_line(tp, type, sizeof(type));
            read_first_line(vp, temp, sizeof(temp));
            snprintf(out, sizeof(out), "  %-15s %-22s %s mC\n",
                     de->d_name, type[0] ? type : "?", temp[0] ? temp : "?");
            usb_write(out);
        }
        closedir(td);
    }

    usb_write("[+] CPUfreq, Cooling & CPUidle:\n");
    for (int pol = 0; pol <= 3; pol += 2) {
        char gp[160], cp[160], minp[160], maxp[160];
        char gov[48] = {0}, cur[32] = {0}, mn[32] = {0}, mx[32] = {0};
        snprintf(gp, sizeof(gp), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor", pol);
        snprintf(cp, sizeof(cp), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", pol);
        snprintf(minp, sizeof(minp), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq", pol);
        snprintf(maxp, sizeof(maxp), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq", pol);
        if (access(gp, F_OK) == 0) {
            read_first_line(gp, gov, sizeof(gov));
            read_first_line(cp, cur, sizeof(cur));
            read_first_line(minp, mn, sizeof(mn));
            read_first_line(maxp, mx, sizeof(mx));
            snprintf(out, sizeof(out), "  policy%d: gov=%-12s cur=%s min=%s max=%s kHz\n",
                     pol, gov[0] ? gov : "?", cur[0] ? cur : "?",
                     mn[0] ? mn : "?", mx[0] ? mx : "?");
            usb_write(out);
        }
    }
    DIR *cd = opendir("/sys/class/thermal");
    if (cd) {
        struct dirent *de;
        while ((de = readdir(cd)) != NULL) {
            if (strncmp(de->d_name, "cooling_device", 14) != 0)
                continue;
            char tp[300], cp[300], mp[300], type[64] = {0}, cur[16] = {0}, maxs[16] = {0};
            snprintf(tp, sizeof(tp), "/sys/class/thermal/%s/type", de->d_name);
            snprintf(cp, sizeof(cp), "/sys/class/thermal/%s/cur_state", de->d_name);
            snprintf(mp, sizeof(mp), "/sys/class/thermal/%s/max_state", de->d_name);
            read_first_line(tp, type, sizeof(type));
            read_first_line(cp, cur, sizeof(cur));
            read_first_line(mp, maxs, sizeof(maxs));
            snprintf(out, sizeof(out), "  %-16s %-22s state=%s/%s\n",
                     de->d_name, type[0] ? type : "?", cur[0] ? cur : "?", maxs[0] ? maxs : "?");
            usb_write(out);
        }
        closedir(cd);
    }
    for (int cpu = 0; cpu < 4; cpu++) {
        char p0n[160], p1u[160], p1n[160], u0[32] = {0}, u1[32] = {0}, s1[48] = {0};
        snprintf(p0n, sizeof(p0n), "/sys/devices/system/cpu/cpu%d/cpuidle/state0/usage", cpu);
        snprintf(p1u, sizeof(p1u), "/sys/devices/system/cpu/cpu%d/cpuidle/state1/usage", cpu);
        snprintf(p1n, sizeof(p1n), "/sys/devices/system/cpu/cpu%d/cpuidle/state1/name", cpu);
        if (access(p0n, F_OK) == 0) {
            read_first_line(p0n, u0, sizeof(u0));
            if (access(p1u, F_OK) == 0) {
                read_first_line(p1u, u1, sizeof(u1));
                read_first_line(p1n, s1, sizeof(s1));
                snprintf(out, sizeof(out), "  cpu%d idle: WFI=%s %s=%s\n", cpu, u0, s1[0] ? s1 : "state1", u1);
            } else {
                snprintf(out, sizeof(out), "  cpu%d idle: WFI=%s\n", cpu, u0);
            }
            usb_write(out);
        }
    }
    DIR *rg = opendir("/sys/class/regulator");
    if (rg) {
        struct dirent *de;
        while ((de = readdir(rg)) != NULL) {
            if (strncmp(de->d_name, "regulator.", 10) != 0)
                continue;
            char np[300], vp[300], rname[64] = {0}, ruv[32] = {0};
            snprintf(np, sizeof(np), "/sys/class/regulator/%s/name", de->d_name);
            read_first_line(np, rname, sizeof(rname));
            if (strcmp(rname, "pm8994_s11") == 0 || strcmp(rname, "vdd_gfx") == 0) {
                snprintf(vp, sizeof(vp), "/sys/class/regulator/%s/microvolts", de->d_name);
                read_first_line(vp, ruv, sizeof(ruv));
                snprintf(out, sizeof(out), "  reg %-12s uV=%s\n", rname, ruv[0] ? ruv : "?");
                usb_write(out);
            }
        }
        closedir(rg);
    }
    if (access("/sys/class/devfreq/b00000.gpu/cur_freq", F_OK) == 0) {
        char gcur[32] = {0}, gmin[32] = {0}, gmax[32] = {0}, ggov[48] = {0};
        read_first_line("/sys/class/devfreq/b00000.gpu/cur_freq", gcur, sizeof(gcur));
        read_first_line("/sys/class/devfreq/b00000.gpu/min_freq", gmin, sizeof(gmin));
        read_first_line("/sys/class/devfreq/b00000.gpu/max_freq", gmax, sizeof(gmax));
        read_first_line("/sys/class/devfreq/b00000.gpu/governor", ggov, sizeof(ggov));
        snprintf(out, sizeof(out), "  gpu:     gov=%-12s cur=%s min=%s max=%s Hz\n",
                 ggov, gcur, gmin, gmax);
        usb_write(out);
    }

    usb_write("[+] Memory, ZRAM & Pstore:\n");
    FILE *mf = fopen("/proc/meminfo", "r");
    if (mf) {
        char line[160];
        int shown = 0;
        while (fgets(line, sizeof(line), mf) && shown < 6) {
            if (strncmp(line, "MemTotal:", 9) == 0 ||
                strncmp(line, "MemAvailable:", 13) == 0 ||
                strncmp(line, "SwapTotal:", 10) == 0 ||
                strncmp(line, "SwapFree:", 9) == 0) {
                usb_write("  ");
                usb_write(line);
                shown++;
            }
        }
        fclose(mf);
    }
    dump_sysfs_dir("pstore", "/sys/fs/pstore", true);
    dump_battery_info();
}

static void dump_wifi_info(void) {
    char out[768];
    usb_write("[+] PCIe / Wi-Fi Hardware & Driver State:\n");

    usb_write("  [1] PCI devices (/sys/bus/pci/devices):\n");
    DIR *pd = opendir("/sys/bus/pci/devices");
    if (!pd) {
        usb_write("      (no /sys/bus/pci/devices)\n");
    } else {
        struct dirent *de;
        int count = 0;
        while ((de = readdir(pd)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            count++;
            char path[384], ven[32], dev[32], drv[256];
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", de->d_name);
            read_first_line(path, ven, sizeof(ven));
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", de->d_name);
            read_first_line(path, dev, sizeof(dev));
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver", de->d_name);
            ssize_t dlen = readlink(path, drv, sizeof(drv) - 1);
            if (dlen > 0) {
                drv[dlen] = '\0';
                const char *base = strrchr(drv, '/');
                snprintf(out, sizeof(out), "      %s  vendor=%s device=%s driver=%s\n",
                         de->d_name, ven, dev, base ? base + 1 : drv);
            } else {
                snprintf(out, sizeof(out), "      %s  vendor=%s device=%s driver=(none)\n",
                         de->d_name, ven, dev);
            }
            usb_write(out);
        }
        closedir(pd);
        if (count == 0)
            usb_write("      (empty)\n");
    }

    usb_write("  [2] Network interface (/sys/class/net/wlan0):\n");
    if (access("/sys/class/net/wlan0", F_OK) == 0) {
        char state[64], addr[64];
        read_first_line("/sys/class/net/wlan0/operstate", state, sizeof(state));
        read_first_line("/sys/class/net/wlan0/address", addr, sizeof(addr));
        snprintf(out, sizeof(out), "      wlan0: operstate=%s address=%s\n", state, addr);
        usb_write(out);
    } else {
        usb_write("      (wlan0 not present)\n");
    }
    dump_sysfs_dir("net", "/sys/class/net", true);

    usb_write("  [3] RFKill (/sys/class/rfkill):\n");
    dump_sysfs_dir("rfkill", "/sys/class/rfkill", true);
    DIR *rd = opendir("/sys/class/rfkill");
    if (rd) {
        struct dirent *de;
        while ((de = readdir(rd)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            char path[384], name[64], type[32], state[16], soft[16], hard[16];
            snprintf(path, sizeof(path), "/sys/class/rfkill/%s/name", de->d_name);
            read_first_line(path, name, sizeof(name));
            snprintf(path, sizeof(path), "/sys/class/rfkill/%s/type", de->d_name);
            read_first_line(path, type, sizeof(type));
            snprintf(path, sizeof(path), "/sys/class/rfkill/%s/state", de->d_name);
            read_first_line(path, state, sizeof(state));
            snprintf(path, sizeof(path), "/sys/class/rfkill/%s/soft", de->d_name);
            read_first_line(path, soft, sizeof(soft));
            snprintf(path, sizeof(path), "/sys/class/rfkill/%s/hard", de->d_name);
            read_first_line(path, hard, sizeof(hard));
            snprintf(out, sizeof(out), "      %s (%s/%s): state=%s soft=%s hard=%s\n",
                     de->d_name, name, type, state, soft, hard);
            usb_write(out);
        }
        closedir(rd);
    }

    usb_write("  [4] Staged Wi-Fi Firmware & RRO:\n");
    snprintf(out, sizeof(out),
             "      firmware-6.bin(tools): %s  firmware-6.bin(lib): %s\n"
             "      board.bin(tools):      %s  regulatory.db(tools): %s\n"
             "      SailfishWifiOverlay:   %s\n",
             access("/tools/firmware/ath10k/QCA6174/hw3.0/firmware-6.bin", F_OK) == 0 ? "yes" : "no",
             access("/lib/firmware/ath10k/QCA6174/hw3.0/firmware-6.bin", F_OK) == 0 ? "yes" : "no",
             access("/tools/firmware/ath10k/QCA6174/hw3.0/board.bin", F_OK) == 0 ? "yes" : "no",
             access("/tools/firmware/regulatory.db", F_OK) == 0 ? "yes" : "no",
             access("/tools/SailfishWifiOverlay.apk", F_OK) == 0 ? "yes" : "no");
    usb_write(out);

    usb_write("  [5] Regulators (WLAN/PCIe/PM8994 rails):\n");
    DIR *rdir = opendir("/sys/class/regulator");
    if (rdir) {
        struct dirent *rde;
        static const char *const reg_keys[] = {
            "rome", "wlan", "l28", "l12", "l30", "s3", "s4", "s11"
        };
        while ((rde = readdir(rdir)) != NULL) {
            if (rde->d_name[0] == '.')
                continue;
            char name[96] = {0}, state[32] = {0}, uv[32] = {0}, users[32] = {0}, path[320];
            snprintf(path, sizeof(path), "/sys/class/regulator/%s/name", rde->d_name);
            read_first_line(path, name, sizeof(name));
            if (!name[0])
                continue;
            bool match = false;
            for (int k = 0; k < (int)(sizeof(reg_keys) / sizeof(reg_keys[0])); k++) {
                if (strcasestr(name, reg_keys[k])) {
                    match = true;
                    break;
                }
            }
            if (!match)
                continue;
            snprintf(path, sizeof(path), "/sys/class/regulator/%s/state", rde->d_name);
            read_first_line(path, state, sizeof(state));
            snprintf(path, sizeof(path), "/sys/class/regulator/%s/microvolts", rde->d_name);
            read_first_line(path, uv, sizeof(uv));
            snprintf(path, sizeof(path), "/sys/class/regulator/%s/num_users", rde->d_name);
            read_first_line(path, users, sizeof(users));
            char rline[448];
            snprintf(rline, sizeof(rline), "      %-16s name=%-18s state=%-10s uV=%-8s users=%s\n",
                     rde->d_name, name, state[0] ? state : "?", uv[0] ? uv : "?",
                     users[0] ? users : "?");
            usb_write(rline);
        }
        closedir(rdir);
    } else {
        usb_write("      (/sys/class/regulator unavailable)\n");
    }

    usb_write("  [6] Pinmux & PMIC pinconf (RAM/SPMI safe):\n");
    if (access("/sys/kernel/debug/pinctrl", F_OK) != 0) {
        mkdir("/sys/kernel/debug", 0755);
        mount("none", "/sys/kernel/debug", "debugfs", 0, NULL);
    }
    DIR *pdir = opendir("/sys/kernel/debug/pinctrl");
    if (pdir) {
        struct dirent *pde;
        static const char *const pin_keys[] = {
            "(gpio8)", "(gpio9)", "(gpio18)", "(gpio35)", "(gpio36)", "(gpio37)", "(gpio46)",
            "pcie", "wlan", "rome", "cnss"
        };
        while ((pde = readdir(pdir)) != NULL) {
            if (pde->d_name[0] == '.')
                continue;
            char pm_path[320];
            snprintf(pm_path, sizeof(pm_path), "/sys/kernel/debug/pinctrl/%s/pinmux-pins", pde->d_name);
            FILE *pf = fopen(pm_path, "r");
            if (!pf)
                continue;
            char hdr[320];
            snprintf(hdr, sizeof(hdr), "      [%s/pinmux-pins]\n", pde->d_name);
            bool hdr_printed = false;
            char pline[256];
            while (fgets(pline, sizeof(pline), pf)) {
                for (int k = 0; k < (int)(sizeof(pin_keys) / sizeof(pin_keys[0])); k++) {
                    if (strcasestr(pline, pin_keys[k])) {
                        if (!hdr_printed) {
                            usb_write(hdr);
                            hdr_printed = true;
                        }
                        usb_write("        ");
                        usb_write(pline);
                        break;
                    }
                }
            }
            fclose(pf);
        }
        closedir(pdir);
    } else {
        usb_write("      (/sys/kernel/debug/pinctrl unavailable)\n");
    }

    /* Safe to inspect PM8994 SPMI GPIOs via pinconf-groups (never read 1010000.pinctrl/pinconf-pins!) */
    FILE *pcf = fopen("/sys/kernel/debug/pinctrl/400f000.spmi:pmic@0:gpio@c000/pinconf-groups", "r");
    if (pcf) {
        static const char *const pmic_keys[] = { "gpio8)", "gpio9)", "gpio18)", "gpio19)" };
        bool pc_hdr = false;
        char pcline[256];
        while (fgets(pcline, sizeof(pcline), pcf)) {
            for (int k = 0; k < (int)(sizeof(pmic_keys) / sizeof(pmic_keys[0])); k++) {
                if (strstr(pcline, pmic_keys[k])) {
                    if (!pc_hdr) {
                        usb_write("      [400f000.spmi:pmic@0:gpio@c000/pinconf-groups]\n");
                        pc_hdr = true;
                    }
                    usb_write("        ");
                    usb_write(pcline);
                    break;
                }
            }
        }
        fclose(pcf);
    }

    usb_write("  [7] Regulator summary (/sys/kernel/debug/regulator/regulator_summary):\n");
    FILE *rsf = fopen("/sys/kernel/debug/regulator/regulator_summary", "r");
    if (rsf) {
        static const char *const rs_keys[] = {
            "rome", "wlan", "l28", "l12", "l30", "s3", "s4", "vph", "pcie", "34000"
        };
        char rline[256];
        while (fgets(rline, sizeof(rline), rsf)) {
            for (int k = 0; k < (int)(sizeof(rs_keys) / sizeof(rs_keys[0])); k++) {
                if (strcasestr(rline, rs_keys[k])) {
                    usb_write("      ");
                    usb_write(rline);
                    break;
                }
            }
        }
        fclose(rsf);
    } else {
        usb_write("      (regulator_summary unavailable)\n");
    }

    usb_write("  [7b] Clocks (/sys/kernel/debug/clk/clk_summary):\n");
    FILE *csf = fopen("/sys/kernel/debug/clk/clk_summary", "r");
    if (csf) {
        static const char *const clk_keys[] = {
            "pcie", "clkref", "sleep_clk", "xo_board"
        };
        char cline[256];
        while (fgets(cline, sizeof(cline), csf)) {
            for (int k = 0; k < (int)(sizeof(clk_keys) / sizeof(clk_keys[0])); k++) {
                if (strcasestr(cline, clk_keys[k])) {
                    usb_write("      ");
                    usb_write(cline);
                    break;
                }
            }
        }
        fclose(csf);
    } else {
        usb_write("      (clk_summary unavailable)\n");
    }

    usb_write("  [8] Kernel dmesg (PCIe/QMP/PM8994/ath10k/USB/CPUfreq):\n");
    static const char *const wifi_keys[] = {
        "pcie", "qcom-pcie", "qcom_pcie", "qmp", "34000", "phy-wrapper",
        "ath10k", "cfg80211", "wlan", "rome", "pm8994", "pinctrl",
        "regulator", "spmi", "qcom-cpufreq", "s11", "cbf",
        "dwc3", "f_fs", "ffs", "regulatory"
    };
    dump_dmesg_any(400, wifi_keys, (int)(sizeof(wifi_keys) / sizeof(wifi_keys[0])));
}

static void test_android_primitives(void) {
    char out[256];
    usb_write("[+] Android 17 Kernel Infrastructure Status:\n");

    /* 1. Binder */
    usb_write("  [1] Binder IPC nodes (/dev/binderfs):\n");
    dump_sysfs_dir("binderfs", "/dev/binderfs", true);

    /* 2. DMA-BUF Heaps */
    usb_write("  [2] DMA-BUF Heaps (/dev/dma_heap):\n");
    dump_sysfs_dir("dma_heap", "/dev/dma_heap", true);

    /* 3. PSI (Pressure Stall Information) */
    usb_write("  [3] Pressure Stall Information (/proc/pressure):\n");
    dump_sysfs_dir("pressure", "/proc/pressure", true);
    char psi[128];
    if (read_first_line("/proc/pressure/memory", psi, sizeof(psi))) {
        snprintf(out, sizeof(out), "      PSI Memory: %s\n", psi);
        usb_write(out);
    }
    if (read_first_line("/proc/pressure/cpu", psi, sizeof(psi))) {
        snprintf(out, sizeof(out), "      PSI CPU:    %s\n", psi);
        usb_write(out);
    }

    /* 4. SELinux */
    char se_enforce[32];
    if (read_first_line("/sys/fs/selinux/enforce", se_enforce, sizeof(se_enforce))) {
        snprintf(out, sizeof(out), "  [4] SELinux Enforce: %s (%s)\n",
                 se_enforce, se_enforce[0] == '1' ? "enforcing" : "permissive");
        usb_write(out);
    } else {
        usb_write("  [4] SELinux: ready\n");
    }

    /* 5. Cgroups */
    usb_write("  [5] Cgroups (/proc/cgroups):\n");
    dump_file_lines("/proc/cgroups", 20, NULL, true);
}

static void set_selinux_label(const char *path, const char *label) {
    if (path && label) {
        setxattr(path, "security.selinux", label, strlen(label) + 1, 0);
    }
}

static bool copy_file(const char *src, const char *dst) {
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) return false;
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (dfd < 0) { close(sfd); return false; }
    char buf[4096];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        if (write(dfd, buf, n) != n) {
            close(sfd); close(dfd); return false;
        }
    }
    close(sfd);
    close(dfd);
    return true;
}

static void copy_dir_labeled(const char *src_dir, const char *dst_dir,
                             const char *label, int depth) {
    if (depth > 4)
        return;
    mkdir(dst_dir, 0755);
    chmod(dst_dir, 0755);
    set_selinux_label(dst_dir, label);

    DIR *d = opendir(src_dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char sp[512], dp[512];
        int sn = snprintf(sp, sizeof(sp), "%s/%s", src_dir, de->d_name);
        int dn = snprintf(dp, sizeof(dp), "%s/%s", dst_dir, de->d_name);
        if (sn < 0 || (size_t)sn >= sizeof(sp) || dn < 0 || (size_t)dn >= sizeof(dp))
            continue;
        struct stat st;
        if (lstat(sp, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            copy_dir_labeled(sp, dp, label, depth + 1);
        } else if (S_ISLNK(st.st_mode)) {
            char target[512];
            ssize_t len = readlink(sp, target, sizeof(target) - 1);
            if (len > 0) {
                target[len] = '\0';
                unlink(dp);
                if (symlink(target, dp) == 0 && label)
                    lsetxattr(dp, "security.selinux", label, strlen(label) + 1, 0);
            }
        } else if (S_ISREG(st.st_mode)) {
            if (copy_file(sp, dp)) {
                chmod(dp, 0644);
                set_selinux_label(dp, label);
            }
        }
    }
    closedir(d);
}

static void write_file_str(const char *path, const char *val) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, val, strlen(val));
        close(fd);
    }
}

/* Remove 0-byte regular placeholder files from a lib dir without touching symlinks. */
static void clean_zero_byte_libs(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char sp[256];
        snprintf(sp, sizeof(sp), "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (lstat(sp, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0) {
            unlink(sp);
        }
    }
    closedir(d);
}

/* Non-destructively overlay an Android ueventd.rc file with 0666 ION/DMA-BUF/DRM rules. */
static void overlay_ueventd_file(const char *src_rc, const char *tmp_rc, const char *label) {
    if (access(src_rc, F_OK) != 0) return;

    mkdir("/tmp", 0755);
    FILE *in = fopen(src_rc, "r");
    FILE *out = fopen(tmp_rc, "w");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), in)) {
        if (strncmp(line, "/dev/ion", 8) == 0 ||
            strncmp(line, "/dev/dma_heap/system ", 21) == 0 ||
            strncmp(line, "/dev/dma_heap/system\t", 21) == 0 ||
            strncmp(line, "/dev/dma_heap/system-uncached", 29) == 0 ||
            strncmp(line, "/dev/dri/*", 10) == 0) {
            continue;
        }
        fputs(line, out);
    }
    fprintf(out,
        "\n# Sailfish mainline DMA-BUF/ION/DRM permissions\n"
        "/dev/ion                                  0666   root       root\n"
        "/dev/dma_heap/system                      0666   system     system\n"
        "/dev/dma_heap/system-uncached             0666   system     system\n"
        "/dev/dri/*                                0666   root       graphics\n");
    fclose(in);
    fclose(out);

    chmod(tmp_rc, 0644);
    set_selinux_label(tmp_rc, label);
    if (mount(tmp_rc, src_rc, NULL, MS_BIND, NULL) == 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "[+] Automated: Overlaid %s with 0666 ION/DMA-BUF/DRM rules\n", src_rc);
        usb_write(msg);
    }
}

/* Write a permissive multi-arch (${LIB}) linkerconfig enabling all APEX & system namespaces. */
static void umount_all(const char *path) {
    for (int tries = 0; tries < 32; tries++) {
        if (umount2(path, MNT_DETACH) != 0) break;
    }
}

static int bind_mount_clean(const char *src, const char *dst) {
    umount_all(dst);
    return mount(src, dst, NULL, MS_BIND, NULL);
}

static bool is_path_fscrypt_encrypted(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return false;

    struct fscrypt_policy_v1 p1;
    memset(&p1, 0, sizeof(p1));
    if (ioctl(fd, FS_IOC_GET_ENCRYPTION_POLICY, &p1) == 0) {
        close(fd);
        return true;
    }

    struct fscrypt_get_policy_ex_arg ex;
    memset(&ex, 0, sizeof(ex));
    ex.policy_size = sizeof(ex.policy);
    if (ioctl(fd, FS_IOC_GET_ENCRYPTION_POLICY_EX, &ex) == 0) {
        close(fd);
        return true;
    }

    close(fd);
    return false;
}

/* Refuse stock FBE/non-Sailfish trees; accept only empty or marked unencrypted userdata. */
static bool is_userdata_safe_unencrypted(const char *mnt) {
    char check_path[256];
    const char *fbe_paths[] = { "unencrypted", "misc/vold/user_keys", NULL };
    for (int i = 0; fbe_paths[i]; i++) {
        snprintf(check_path, sizeof(check_path), "%s/%s", mnt, fbe_paths[i]);
        if (access(check_path, F_OK) == 0)
            return false;
    }

    if (is_path_fscrypt_encrypted(mnt))
        return false;

    const char *subdirs[] = { "data", "system", "user", "user/0", "app", "media", "misc", NULL };
    for (int i = 0; subdirs[i]; i++) {
        snprintf(check_path, sizeof(check_path), "%s/%s", mnt, subdirs[i]);
        if (is_path_fscrypt_encrypted(check_path))
            return false;
    }

    snprintf(check_path, sizeof(check_path), "%s/.sailfish_unencrypted", mnt);
    bool has_marker = (access(check_path, F_OK) == 0);

    DIR *d = opendir(mnt);
    if (!d)
        return false;

    bool has_other_entries = false;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0 ||
            strcmp(de->d_name, "lost+found") == 0 || strcmp(de->d_name, ".sailfish_unencrypted") == 0) {
            continue;
        }
        snprintf(check_path, sizeof(check_path), "%s/%s", mnt, de->d_name);
        if (is_path_fscrypt_encrypted(check_path)) {
            closedir(d);
            return false;
        }
        has_other_entries = true;
    }
    closedir(d);

    return has_marker || !has_other_entries;
}

static bool dev_in_proc_mounts(const char *target_dev) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return false;
    char line[1024], dev[256];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%255s", dev) == 1 && strcmp(dev, target_dev) == 0) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static void mount_tmpfs_data(const char *target) {
    umount_all(target);
    mkdir(target, 0771);
    mount("tmpfs", target, "tmpfs", 0, "mode=0771,uid=1000,gid=1000");
    chown(target, 1000, 1000);
    chmod(target, 0771);
    set_selinux_label(target, "u:object_r:system_data_root_file:s0");
    g_data_persistent = false;
}

static void mount_userdata_partition(const char *sys_dev) {
    const char *target = "/mnt/system_a/data";
    const char *probe = "/mnt/userdata_probe";
    g_data_persistent = false;
    umount_all(target);
    umount_all(probe);
    mkdir(target, 0771);
    mkdir(probe, 0755);

    if (strcmp(sys_dev, "/dev/sda35") == 0) {
        usb_write("[!] /dev/sda35 is system root; using tmpfs /data\n");
        mount_tmpfs_data(target);
        return;
    }

    const char *fstypes[] = { "ext4", "f2fs", NULL };
    const char *mounted_fs = NULL;
    int last_err = ENOENT;
    for (int i = 0; fstypes[i]; i++) {
        if (mount("/dev/sda35", probe, fstypes[i], MS_NOATIME, NULL) == 0) {
            mounted_fs = fstypes[i];
            break;
        }
        last_err = errno;
    }

    if (!mounted_fs) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[!] Could not mount /dev/sda35 as ext4/f2fs (%s); using tmpfs /data\n"
                 "[!] Run 'format_userdata confirm' in sailfish# or 'fastboot format:ext4 userdata' for persistent /data\n",
                 strerror(last_err));
        usb_write(msg);
        mount_tmpfs_data(target);
        return;
    }

    bool safe = is_userdata_safe_unencrypted(probe);
    if (umount2(probe, 0) != 0)
        umount_all(probe);

    if (!safe) {
        usb_write("[!] /dev/sda35 has FBE-encrypted or unrecognized stock data; using tmpfs /data\n"
                  "[!] To enable persistent /data safely, run 'format_userdata confirm' or 'fastboot format:ext4 userdata'\n");
        mount_tmpfs_data(target);
        return;
    }

    if (mount("/dev/sda35", target, mounted_fs, MS_NOATIME, NULL) != 0) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "[!] Re-mount /dev/sda35 (%s) -> /data failed (%s); using tmpfs /data\n",
                 mounted_fs, strerror(errno));
        usb_write(msg);
        mount_tmpfs_data(target);
        return;
    }

    int mfd = open("/mnt/system_a/data/.sailfish_unencrypted", O_WRONLY | O_CREAT, 0600);
    if (mfd >= 0) {
        write(mfd, "sailfish-unencrypted-data\n", 26);
        close(mfd);
    }
    chown(target, 1000, 1000);
    chmod(target, 0771);
    set_selinux_label(target, "u:object_r:system_data_root_file:s0");
    g_data_persistent = true;

    char okmsg[192];
    snprintf(okmsg, sizeof(okmsg),
             "[+] Mounted persistent unencrypted /dev/sda35 (%s) -> /data (ART dexopt enabled)\n",
             mounted_fs);
    usb_write(okmsg);
}

static void install_data_file(const char *src, const char *dst, mode_t mode,
                              const char *label, const char *usb_msg) {
    if (access(src, F_OK) != 0)
        return;
    unlink(dst);
    if (copy_file(src, dst)) {
        chmod(dst, mode);
        set_selinux_label(dst, label);
        if (usb_msg)
            usb_write(usb_msg);
    }
}

static void copy_selinux_label_or(const char *src, char *buf, size_t len, const char *fallback) {
    ssize_t llen = getxattr(src, "security.selinux", buf, len - 1);
    if (llen <= 0) {
        snprintf(buf, len, "%s", fallback);
    } else {
        buf[llen] = '\0';
    }
}

/* Non-destructively overlay an Android build.prop so adbd runs passwordless alongside rescue shell. */
static void overlay_prop_file(const char *src, const char *tmp, const char *fallback_lbl,
                              bool append_defaults, const char *log_msg) {
    umount_all(src);
    if (access(src, F_OK) != 0)
        return;

    char label[128] = {0};
    copy_selinux_label_or(src, label, sizeof(label), fallback_lbl);

    mkdir("/tmp", 0755);
    FILE *in = fopen(src, "r");
    FILE *out = fopen(tmp, "w");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return;
    }

    char *line = NULL;
    size_t cap = 0;
    bool saw_adb_secure = false;
    bool saw_debuggable = false;
    bool saw_wifi_iface = false;
    bool saw_sf_blur = false;
    bool saw_disable_blurs = false;
    while (getline(&line, &cap, in) >= 0) {
        if (strncmp(line, "ro.adb.secure=", 14) == 0) {
            fputs("ro.adb.secure=0\n", out);
            saw_adb_secure = true;
        } else if (strncmp(line, "ro.debuggable=", 14) == 0) {
            fputs("ro.debuggable=1\n", out);
            saw_debuggable = true;
        } else if (append_defaults && strncmp(line, "wifi.interface=", 15) == 0) {
            fputs("wifi.interface=wlan0\n", out);
            saw_wifi_iface = true;
        } else if (append_defaults &&
                   strncmp(line, "ro.surface_flinger.supports_background_blur=",
                           sizeof("ro.surface_flinger.supports_background_blur=") - 1) == 0) {
            fputs("ro.surface_flinger.supports_background_blur=0\n", out);
            saw_sf_blur = true;
        } else if (append_defaults &&
                   strncmp(line, "persist.sys.sf.disable_blurs=",
                           sizeof("persist.sys.sf.disable_blurs=") - 1) == 0) {
            fputs("persist.sys.sf.disable_blurs=1\n", out);
            saw_disable_blurs = true;
        } else {
            fputs(line, out);
        }
    }
    free(line);
    if (append_defaults) {
        if (!saw_adb_secure)
            fputs("ro.adb.secure=0\n", out);
        if (!saw_debuggable)
            fputs("ro.debuggable=1\n", out);
        if (!saw_wifi_iface)
            fputs("wifi.interface=wlan0\n", out);
        if (!saw_sf_blur)
            fputs("ro.surface_flinger.supports_background_blur=0\n", out);
        if (!saw_disable_blurs)
            fputs("persist.sys.sf.disable_blurs=1\n", out);
    }
    fclose(in);
    fclose(out);

    chmod(tmp, 0644);
    set_selinux_label(tmp, label);
    if (bind_mount_clean(tmp, src) == 0)
        usb_write(log_msg);
}

static void overlay_adb_build_prop(void) {
    overlay_prop_file("/mnt/system_a/system/build.prop", "/tmp/system_build.prop",
                      "u:object_r:system_file:s0", true,
                      "[+] Automated: Overlaid /system/build.prop (ro.adb.secure=0, ro.debuggable=1, wifi.interface=wlan0)\n");
    overlay_prop_file("/mnt/system_a/vendor/build.prop", "/tmp/vendor_build.prop",
                      "u:object_r:vendor_configs_file:s0", false,
                      "[+] Automated: Overlaid /vendor/build.prop (ro.adb.secure=0, ro.debuggable=1)\n");
}

/* Replace Android's stock init.usb.configfs.rc and silence vendor USB scripts so composite
 * CDC-ACM (/dev/ttyACM0) stays intact while FunctionFS ADB is bound once adbd opens ep0. */
static void overlay_usb_configfs_rc(void) {
    char udc_name[96];
    if (!find_udc(udc_name, sizeof(udc_name)))
        snprintf(udc_name, sizeof(udc_name), "6a00000.usb");

    mkdir("/tmp", 0755);
    const char *empty_rc = "/tmp/empty_usb.rc";
    write_file_str(empty_rc, "# Overridden by sailfish composite ACM+ADB USB configfs\n");
    chmod(empty_rc, 0644);
    set_selinux_label(empty_rc, "u:object_r:vendor_configs_file:s0");

    const char *vendor_usb_rcs[] = {
        "/mnt/system_a/vendor/etc/init/hw/init.sailfish.usb.rc",
        "/mnt/system_a/vendor/etc/init/hw/init.marlin.usb.rc",
        "/mnt/system_a/vendor/etc/init/init.sailfish.usb.rc",
        NULL
    };
    for (int i = 0; vendor_usb_rcs[i]; i++) {
        if (access(vendor_usb_rcs[i], F_OK) == 0) {
            if (bind_mount_clean(empty_rc, vendor_usb_rcs[i]) == 0) {
                char msg[192];
                snprintf(msg, sizeof(msg), "[+] Automated: Overlaid %s (preserving ttyGS0)\n", vendor_usb_rcs[i]);
                usb_write(msg);
            }
        }
    }

    char usb_rc[4096];
    snprintf(usb_rc, sizeof(usb_rc),
        "on early-init\n"
        "    setprop sys.usb.configfs 1\n"
        "    setprop sys.usb.controller %s\n"
        "    setprop ro.adb.secure 0\n"
        "\n"
        "on post-fs-data\n"
        "    mkdir /data/adb 0700 root root\n"
        "    mkdir /data/misc/adb 02750 system shell\n"
        "    mkdir /dev/usb-ffs 0770 shell shell\n"
        "    mkdir /dev/usb-ffs/adb 0770 shell shell\n"
        "    mkdir /config/usb_gadget/g1/functions/ffs.adb 0770 shell shell\n"
        "    setprop sys.usb.ffs.ready 0\n"
        "    setprop sys.usb.state none\n"
        "\n"
        "on boot\n"
        "    setprop sys.sailfish.adb 0\n"
        "    setprop sys.usb.config none\n"
        "    stop adbd\n"
        "    setprop sys.usb.ffs.ready 0\n"
        "    setprop sys.usb.state none\n"
        "\n"
        "on property:sys.usb.config=none && property:sys.usb.configfs=1\n"
        "    stop adbd\n"
        "    setprop sys.usb.ffs.ready 0\n"
        "    setprop sys.usb.state none\n"
        "\n"
        "on property:init.svc.adbd=stopped\n"
        "    setprop sys.usb.ffs.ready 0\n"
        "\n"
        "# Keep adbd stopped unless opted in via sys.sailfish.adb=1 (prevents 1-Hz FUNCTIONFS_BIND spin)\n"
        "on property:sys.usb.config=adb && property:sys.sailfish.adb=0 && property:sys.usb.configfs=1\n"
        "    stop adbd\n"
        "    setprop sys.usb.ffs.ready 0\n"
        "    setprop sys.usb.config none\n"
        "    setprop sys.usb.state none\n"
        "\n"
        "on property:init.svc.adbd=running && property:sys.sailfish.adb=0\n"
        "    stop adbd\n"
        "    setprop sys.usb.ffs.ready 0\n"
        "    setprop sys.usb.config none\n"
        "    setprop sys.usb.state none\n"
        "\n"
        "# Opt-in ADB only (never bounce UDC during boot so CDC-ACM /dev/ttyACM0 stays up):\n"
        "on property:sys.sailfish.adb=1 && property:sys.usb.configfs=1\n"
        "    setprop sys.usb.config adb\n"
        "    start adbd\n"
        "\n"
        "on property:sys.sailfish.adb=0 && property:sys.usb.configfs=1\n"
        "    stop adbd\n"
        "    setprop sys.usb.ffs.ready 0\n"
        "    exec - root root -- /system/bin/sh /data/setup_usb_adb.sh unbind ${sys.usb.controller}\n"
        "    setprop sys.usb.config none\n"
        "    setprop sys.usb.state none\n"
        "\n"
        "on property:sys.sailfish.adb=1 && property:sys.usb.config=adb && property:sys.usb.ffs.ready=1 && property:sys.usb.configfs=1\n"
        "    exec - root root -- /system/bin/sh /data/setup_usb_adb.sh bind ${sys.usb.controller}\n"
        "    setprop sys.usb.state adb\n",
        udc_name);

    const char *dst_rc = "/mnt/system_a/system/etc/init/hw/init.usb.configfs.rc";
    const char *tmp_rc = "/tmp/init.usb.configfs.rc";
    write_file_str(tmp_rc, usb_rc);
    chmod(tmp_rc, 0644);
    if (access(dst_rc, F_OK) == 0) {
        char label[128] = {0};
        copy_selinux_label_or(dst_rc, label, sizeof(label), "u:object_r:system_file:s0");
        set_selinux_label(tmp_rc, label);
        if (bind_mount_clean(tmp_rc, dst_rc) == 0)
            usb_write("[+] Automated: Overlaid init.usb.configfs.rc (composite ACM + FunctionFS ADB)\n");
    } else {
        const char *fb_rc = "/mnt/system_a/system/etc/init/sailfish_usb_adb.rc";
        write_file_str(fb_rc, usb_rc);
        chmod(fb_rc, 0644);
        set_selinux_label(fb_rc, "u:object_r:system_file:s0");
    }
}

struct bin_patch {
    off_t offset;
    const void *bytes;
    size_t len;
};

/* Copy a shared library to /tmp, apply binary patches, and bind-mount over the original. */
static void patch_and_bind_lib(const char *src, const char *tmp_path,
                               const char *label,
                               const struct bin_patch *patches, size_t count,
                               const char *usb_msg) {
    umount_all(src);
    if (access(src, F_OK) != 0) return;

    mkdir("/tmp", 0755);
    if (!copy_file(src, tmp_path)) return;

    int fd = open(tmp_path, O_RDWR);
    if (fd < 0) return;

    bool ok = true;
    for (size_t i = 0; i < count; i++) {
        if (pwrite(fd, patches[i].bytes, patches[i].len, patches[i].offset) !=
            (ssize_t)patches[i].len) {
            ok = false;
            break;
        }
    }
    close(fd);
    if (!ok) {
        unlink(tmp_path);
        return;
    }

    chmod(tmp_path, 0644);
    set_selinux_label(tmp_path, label);
    if (mount(tmp_path, src, NULL, MS_BIND, NULL) == 0 && usb_msg) {
        usb_write(usb_msg);
    }
}

/* Match an Android .rc directive line, ignoring leading whitespace and trailing comments/whitespace. */
static bool is_rc_command_line(const char *line, const char *cmd) {
    while (*line == ' ' || *line == '\t') line++;
    size_t len = strlen(cmd);
    if (strncmp(line, cmd, len) != 0) return false;
    char next = line[len];
    return next == '\0' || next == '\n' || next == '\r' ||
           next == ' '  || next == '\t' || next == '#';
}

static bool drop_composer_restart(const char *line) {
    return strstr(line, "onrestart restart surfaceflinger") != NULL;
}

static bool drop_start_netd(const char *line) {
    return is_rc_command_line(line, "start netd");
}

static bool drop_netd1shot_reboot(const char *line) {
    return is_rc_command_line(line, "reboot_on_failure reboot,netd1shot-fail");
}

/* Non-destructively bind-mount an Android .rc/.cil file in /tmp with matching lines removed. */
static void overlay_filtered_rc(const char *src_rc, const char *tmp_rc,
                                const char *fallback_label,
                                bool (*should_drop)(const char *),
                                const char *usb_msg) {
    umount_all(src_rc);
    if (access(src_rc, F_OK) != 0) return;

    char label[128] = {0};
    ssize_t llen = getxattr(src_rc, "security.selinux", label, sizeof(label) - 1);
    if (llen <= 0) {
        snprintf(label, sizeof(label), "%s", fallback_label);
    } else {
        label[llen] = '\0';
    }

    mkdir("/tmp", 0755);
    FILE *in = fopen(src_rc, "r");
    FILE *out = fopen(tmp_rc, "w");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return;
    }

    char *line = NULL;
    size_t cap = 0;
    bool dropped = false;
    while (getline(&line, &cap, in) >= 0) {
        if (should_drop(line)) {
            dropped = true;
            continue;
        }
        fputs(line, out);
    }
    free(line);
    fclose(in);
    fclose(out);

    if (!dropped) {
        unlink(tmp_rc);
        return;
    }

    chmod(tmp_rc, 0644);
    set_selinux_label(tmp_rc, label);
    if (bind_mount_clean(tmp_rc, src_rc) == 0 && usb_msg) {
        usb_write(usb_msg);
    }
}

static void unmount_android_mounts(void) {
    for (int pass = 0; pass < 8; pass++) {
        FILE *f = fopen("/proc/mounts", "r");
        if (!f) break;
        char mps[512][256];
        int count = 0;
        char line[1024];
        while (fgets(line, sizeof(line), f) && count < 512) {
            char dev[256], mp[256];
            if (sscanf(line, "%255s %255s", dev, mp) == 2) {
                if (strcmp(mp, "/mnt/system_a") == 0 ||
                    strncmp(mp, "/mnt/system_a/", 14) == 0 ||
                    strcmp(mp, "/mnt/vendor_a") == 0 ||
                    strncmp(mp, "/mnt/vendor_a/", 14) == 0) {
                    snprintf(mps[count], sizeof(mps[count]), "%s", mp);
                    count++;
                }
            }
        }
        fclose(f);
        if (count == 0) break;

        for (int i = 0; i < count - 1; i++) {
            for (int j = i + 1; j < count; j++) {
                if (strlen(mps[i]) < strlen(mps[j])) {
                    char tmp[256];
                    memcpy(tmp, mps[i], sizeof(tmp));
                    memcpy(mps[i], mps[j], sizeof(tmp));
                    memcpy(mps[j], tmp, sizeof(tmp));
                }
            }
        }
        int unmounted = 0;
        for (int i = 0; i < count; i++) {
            mount(NULL, mps[i], NULL, MS_PRIVATE, NULL);
            if (umount2(mps[i], MNT_DETACH) == 0) {
                unmounted++;
            }
        }
        if (unmounted == 0) break;
    }
}

static bool is_valid_wrap_target(const char *path) {
    struct stat st, wst;
    if (stat(path, &st) != 0 || st.st_size <= 4096) return false;
    if (stat("/tools/wrap_preload", &wst) == 0 && st.st_size == wst.st_size) {
        unsigned char a[512], b[512];
        int fa = open(path, O_RDONLY);
        int fb = open("/tools/wrap_preload", O_RDONLY);
        bool same = false;
        if (fa >= 0 && fb >= 0) {
            ssize_t na = read(fa, a, sizeof(a));
            ssize_t nb = read(fb, b, sizeof(b));
            if (na > 0 && na == nb && memcmp(a, b, (size_t)na) == 0) {
                same = true;
            }
        }
        if (fa >= 0) close(fa);
        if (fb >= 0) close(fb);
        if (same) return false;
    }
    return true;
}

static void write_proc_sysctl(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        ssize_t n = write(fd, val, strlen(val));
        (void)n;
        close(fd);
    }
}

static bool write_permissive_ld_config(const char *path, const char *label) {
    FILE *ldf = fopen(path, "w");
    if (!ldf) return false;

    fprintf(ldf,
        "# sailfish-permissive-ldconfig\n"
        "dir.vendor = /vendor/bin\n"
        "dir.vendor = /odm/bin\n"
        "dir.system = /system/bin\n"
        "dir.system = /system/xbin\n"
        "dir.system = /system/apex\n"
        "dir.system = /apex\n"
        "dir.system = /data\n"
        "dir.system_ext = /system_ext/bin\n"
        "dir.product = /product/bin\n\n"
    );

    struct ns_spec {
        const char *name;
        const char *search_paths;
    };
    /* Intentionally omit "system" here so libnativeloader's GetSystemNamespace()
     * falls through to "default", where zygote64 already loaded libc.so. */
    static const struct ns_spec apex_ns[] = {
        { "system_ext", "/system_ext/${LIB}:/system/system_ext/${LIB}" },
        { "product", "/product/${LIB}:/system/product/${LIB}" },
        { "com_android_runtime", NULL },
        { "com_android_art", "/apex/com.android.art/${LIB}" },
        { "com_android_i18n", "/apex/com.android.i18n/${LIB}" },
        { "com_android_conscrypt", "/apex/com.android.conscrypt/${LIB}" },
        { "com_android_media", "/apex/com.android.media/${LIB}" },
        { "com_android_media_swcodec", "/apex/com.android.media.swcodec/${LIB}" },
        { "com_android_tethering", "/apex/com.android.tethering/${LIB}" },
        { "com_android_os_statsd", "/apex/com.android.os.statsd/${LIB}" },
        { "com_android_sdkext", "/apex/com.android.sdkext/${LIB}" },
        { "com_android_adbd", "/apex/com.android.adbd/${LIB}" },
        { "com_android_resolv", "/apex/com.android.resolv/${LIB}" },
        { "com_android_neuralnetworks", "/apex/com.android.neuralnetworks/${LIB}" },
        { "com_android_bt", "/apex/com.android.bt/${LIB}" },
        { "com_android_configinfrastructure", "/apex/com.android.configinfrastructure/${LIB}" },
        { "com_android_appsearch", "/apex/com.android.appsearch/${LIB}" },
        { "com_android_uprobestats", "/apex/com.android.uprobestats/${LIB}" },
        { "com_android_uwb", "/apex/com.android.uwb/${LIB}" },
        { "com_android_virt", "/apex/com.android.virt/${LIB}" },
        { "com_android_permission", "/apex/com.android.permission/${LIB}" },
        { "com_android_wifi", "/apex/com.android.wifi/${LIB}" },
        { "com_android_mediaprovider", "/apex/com.android.mediaprovider/${LIB}" },
        { "com_android_adservices", "/apex/com.android.adservices/${LIB}" },
        { "com_android_rkpd", "/apex/com.android.rkpd/${LIB}" },
        { NULL, NULL }
    };

    const char *bionic_paths =
        "/apex/com.android.runtime/${LIB}/bionic:/bootstrap-apex/com.android.runtime/${LIB}/bionic:/system/${LIB}/bootstrap:";
    const char *sys_lib_paths =
        "/system/${LIB}:/system_ext/${LIB}:/system/system_ext/${LIB}:/product/${LIB}:/system/product/${LIB}:";
    const char *apex_lib_paths =
        "/apex/com.android.art/${LIB}:/apex/com.android.runtime/${LIB}:"
        "/apex/com.android.i18n/${LIB}:/apex/com.android.adbd/${LIB}:/apex/com.android.tethering/${LIB}:"
        "/apex/com.android.os.statsd/${LIB}:/apex/com.android.conscrypt/${LIB}:/apex/com.android.media/${LIB}:"
        "/apex/com.android.media.swcodec/${LIB}:/apex/com.android.resolv/${LIB}:/apex/com.android.neuralnetworks/${LIB}:"
        "/apex/com.android.bt/${LIB}:/apex/com.android.configinfrastructure/${LIB}:/apex/com.android.appsearch/${LIB}:"
        "/apex/com.android.uprobestats/${LIB}:/apex/com.android.uwb/${LIB}:/apex/com.android.virt/${LIB}:"
        "/apex/com.android.permission/${LIB}:/apex/com.android.wifi/${LIB}:/apex/com.android.mediaprovider/${LIB}:"
        "/apex/com.android.adservices/${LIB}:/apex/com.android.sdkext/${LIB}:/apex/com.android.rkpd/${LIB}:"
        "/bootstrap-apex/com.android.art/${LIB}:/bootstrap-apex/com.android.runtime/${LIB}:/bootstrap-apex/com.android.i18n/${LIB}:";
    const char *vndk_paths =
        "/vendor/${LIB}:/apex/com.android.vndk.v34/${LIB}:/apex/com.android.vndk.v33/${LIB}:/apex/com.android.vndk.v32/${LIB}:/apex/com.android.vndk.v31/${LIB}";

    char common_paths[2048];
    snprintf(common_paths, sizeof(common_paths), "%s%s%s%s",
             bionic_paths, sys_lib_paths, apex_lib_paths, vndk_paths);

    static const char *const sections[] = { "system", "system_ext", "product", "data", "vendor", NULL };
    for (int s = 0; sections[s]; s++) {
        fprintf(ldf, "[%s]\n", sections[s]);
        fprintf(ldf, "additional.namespaces = ");
        for (int i = 0; apex_ns[i].name; i++) {
            fprintf(ldf, "%s%s", apex_ns[i].name, apex_ns[i + 1].name ? "," : "\n");
        }
        fprintf(ldf, "namespace.default.isolated = false\n");
        fprintf(ldf, "namespace.default.visible = true\n");
        fprintf(ldf, "namespace.default.search.paths = %s\n", common_paths);
        fprintf(ldf, "namespace.default.permitted.paths = /\n");
        fprintf(ldf, "namespace.default.links = ");
        for (int i = 0; apex_ns[i].name; i++) {
            fprintf(ldf, "%s%s", apex_ns[i].name, apex_ns[i + 1].name ? "," : "\n");
        }
        for (int i = 0; apex_ns[i].name; i++) {
            fprintf(ldf, "namespace.default.link.%s.allow_all_shared_libs = true\n", apex_ns[i].name);
        }
        fprintf(ldf, "\n");

        /* Secondary namespaces never list bionic_paths or /system/${LIB} in search.paths,
         * so unbundled product/system_ext/APEX JNI loads share zygote's libc.so from default
         * instead of re-dlopening a second copy into their own namespace (IE TLS failure). */
        for (int i = 0; apex_ns[i].name; i++) {
            const char *ns = apex_ns[i].name;
            fprintf(ldf, "namespace.%s.isolated = false\n", ns);
            fprintf(ldf, "namespace.%s.visible = true\n", ns);
            if (apex_ns[i].search_paths)
                fprintf(ldf, "namespace.%s.search.paths = %s\n", ns, apex_ns[i].search_paths);
            fprintf(ldf, "namespace.%s.permitted.paths = /\n", ns);
            fprintf(ldf, "namespace.%s.links = default\n", ns);
            fprintf(ldf, "namespace.%s.link.default.allow_all_shared_libs = true\n\n", ns);
        }
    }
    fclose(ldf);
    chmod(path, 0644);
    set_selinux_label(path, label);
    return true;
}

/* Restore a single ld.config.txt if Android init/apexd truncated or overwrote it. */
static bool restore_single_ld_config(const char *lc_src, off_t src_size,
                                     const char *dir, const char *label) {
    struct stat dir_st, dst_st;
    if (stat(dir, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode)) return false;

    char lc_dst[192];
    snprintf(lc_dst, sizeof(lc_dst), "%s/ld.config.txt", dir);

    bool need_restore = (lstat(lc_dst, &dst_st) != 0 || dst_st.st_size != src_size);
    if (!need_restore) {
        char hdr[32] = {0};
        int fd = open(lc_dst, O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, hdr, sizeof(hdr) - 1);
            close(fd);
            if (n < 30 || memcmp(hdr, "# sailfish-permissive-ldconfig", 30) != 0) {
                need_restore = true;
            }
        } else {
            need_restore = true;
        }
    }

    if (!need_restore) return false;

    char lc_tmp[208];
    snprintf(lc_tmp, sizeof(lc_tmp), "%s/.ld.config.tmp", dir);
    bool restored = false;
    if (copy_file(lc_src, lc_tmp)) {
        chmod(lc_tmp, 0644);
        set_selinux_label(lc_tmp, "u:object_r:linkerconfig_file:s0");
        if (rename(lc_tmp, lc_dst) == 0) {
            restored = true;
        }
    }
    if (!restored) {
        unlink(lc_tmp);
        if (copy_file(lc_src, lc_dst)) {
            chmod(lc_dst, 0644);
            set_selinux_label(lc_dst, "u:object_r:linkerconfig_file:s0");
            restored = true;
        }
    }
    if (restored) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[+] Supervisor: Restored permissive %s/ld.config.txt\n", label);
        usb_write(msg);
    }
    return restored;
}

/* Restore /linkerconfig/{,default/,bootstrap/}ld.config.txt if init/apexd overwrote them. */
static void restore_linkerconfig_if_needed(pid_t init_pid) {
    if (init_pid <= 1) return;
    char lc_src[128];
    snprintf(lc_src, sizeof(lc_src), "/proc/%d/root/data/ld.config.sailfish.txt", (int)init_pid);

    struct stat src_st;
    if (stat(lc_src, &src_st) != 0 || src_st.st_size < 256) return;

    const char *subdirs[] = { "", "/default", "/bootstrap" };
    const char *labels[] = { "/linkerconfig", "/linkerconfig/default", "/linkerconfig/bootstrap" };
    for (int i = 0; i < 3; i++) {
        char dir[160];
        snprintf(dir, sizeof(dir), "/proc/%d/root/linkerconfig%s", (int)init_pid, subdirs[i]);
        restore_single_ld_config(lc_src, src_st.st_size, dir, labels[i]);
    }
}

static void setup_mdss_sysfs_mock(void) {
    /* Create tmpfs overlay for MDSS graphics sysfs */
    mkdir("/sys/devices/virtual/graphics", 0755);
    mount("tmpfs", "/sys/devices/virtual/graphics", "tmpfs", 0, "mode=0755,uid=0,gid=0");
    mkdir("/sys/devices/virtual/graphics/fb0", 0755);
    mkdir("/sys/devices/virtual/graphics/fb0/mdp", 0755);

    const char *caps_content =
        "hw_rev=268894210\n"
        "max_upscale_ratio=20\n"
        "max_downscale_ratio=4\n"
        "max_bandwidth_low=9600000\n"
        "max_bandwidth_high=9600000\n"
        "max_pipe_bw=4500000\n"
        "max_mdp_clk=412500000\n"
        "clk_fudge_factor=105,100\n"
        "scale_factor=1\n"
        "xtra_ff_factor=1\n"
        "fmt_linear_factor=1\n"
        "fmt_mt_factor=1\n"
        "fmt_mt_nv12_factor=8\n"
        "amortizable_threshold=25\n"
        "system_overhead_lines=20\n"
        "blending_stages=8\n"
        "max_mixer_width=2560\n"
        "max_pipe_width=2560\n"
        "max_cursor_size=512\n"
        "non_scalar_rgb=0\n"
        "features=bwc,decimation,tile_format,src_split\n"
        "pipe_count=12\n"
        "pipe_type=vig pipe_ndx=1 rects=1 fmts_supported=51,224,0,22\n"
        "pipe_type=vig pipe_ndx=2 rects=1 fmts_supported=51,224,0,22\n"
        "pipe_type=vig pipe_ndx=3 rects=1 fmts_supported=51,224,0,22\n"
        "pipe_type=vig pipe_ndx=4 rects=1 fmts_supported=51,224,0,22\n"
        "pipe_type=rgb pipe_ndx=5 rects=1 fmts_supported=51,224,0,22\n"
        "pipe_type=rgb pipe_ndx=6 rects=1 fmts_supported=51,224,0,22\n"
        "pipe_type=rgb pipe_ndx=7 rects=1 fmts_supported=51,224,0,22\n"
        "pipe_type=rgb pipe_ndx=8 rects=1 fmts_supported=51,224,0,22\n"
        "pipe_type=dma pipe_ndx=9 rects=1 fmts_supported=51,224,0,22\n"
        "pipe_type=dma pipe_ndx=10 rects=1 fmts_supported=51,224,0,22\n"
        "pipe_type=cursor pipe_ndx=11 rects=1 fmts_supported=18,96\n"
        "pipe_type=cursor pipe_ndx=12 rects=1 fmts_supported=18,96\n";
    write_file_str("/sys/devices/virtual/graphics/fb0/mdp/caps", caps_content);
    write_file_str("/sys/devices/virtual/graphics/fb0/mdp/bw_mode_bitmap", "0\n");

    const char *panel_content =
        "panel_name=s6e3fa5_cmd\n"
        "primary_panel=1\n"
        "is_pluggable=0\n"
        "xres=1080\n"
        "yres=1920\n"
        "bpp=24\n"
        "type=mipi dsi cmd panel\n"
        "min_fps=60\n"
        "max_fps=60\n"
        "fps=60\n"
        "dyn_fps_en=0\n"
        "pu_en=0\n";
    write_file_str("/sys/devices/virtual/graphics/fb0/msm_fb_panel_info", panel_content);

    write_file_str("/sys/devices/virtual/graphics/fb0/msm_fb_type", "mipi dsi cmd panel\n");
    write_file_str("/sys/devices/virtual/graphics/fb0/connected", "1\n");
    write_file_str("/sys/devices/virtual/graphics/fb0/msm_fb_split", "0 0\n");
    write_file_str("/sys/devices/virtual/graphics/fb0/msm_cmd_autorefresh_en", "0\n");
    write_file_str("/sys/devices/virtual/graphics/fb0/vsync_event", "VSYNC=0\n");
    chmod("/sys/devices/virtual/graphics/fb0/vsync_event", 0666);

    /* Also provide /sys/class/graphics/fb0 mapping */
    mkdir("/sys/class/graphics", 0755);
    mount("tmpfs", "/sys/class/graphics", "tmpfs", 0, "mode=0755,uid=0,gid=0");
    symlink("/sys/devices/virtual/graphics/fb0", "/sys/class/graphics/fb0");

    /* Ensure device node permissions */
    chmod("/dev/fb0", 0666);
    chmod("/dev/dri/card0", 0666);
    chmod("/dev/dri/card1", 0666);
    chmod("/dev/dri/renderD128", 0666);
    chmod("/dev/ion", 0666);

    usb_write("[+] Automated: MDSS sysfs mock initialized (10 pipes, SDE 5.0, panel s6e3fa5_cmd)\n");
}

static void rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                    continue;
                char child[512];
                snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
                rm_rf(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static void stage_oem_wifi_board_data(void) {
    if (access("/tmp/.ath10k_oem_bd_loaded", F_OK) == 0)
        return;

    const char *src_bd = NULL;
    char cand[128] = {0};
    char sub_hex[32] = {0};
    struct stat st;

    if (read_first_line("/sys/bus/pci/devices/0000:01:00.0/subsystem_device",
                        sub_hex, sizeof(sub_hex))) {
        unsigned long sub_id = strtoul(sub_hex, NULL, 0);
        snprintf(cand, sizeof(cand), "/mnt/vendor_a/firmware/bdwlan30.b%02lx",
                 sub_id & 0xffUL);
        if (stat(cand, &st) == 0 && st.st_size >= 4096)
            src_bd = cand;
    }

    if (!src_bd) {
        const char *fallback = "/mnt/vendor_a/firmware/bdwlan30.bin";
        if (stat(fallback, &st) == 0 && st.st_size >= 4096)
            src_bd = fallback;
    }

    if (!src_bd) {
        usb_write("[!] No Pixel OEM bdwlan30 board data found on vendor_a\n");
        return;
    }

    unlink("/tools/firmware/ath10k/QCA6174/hw3.0/board-2.bin");
    if (!copy_file(src_bd, "/tools/firmware/ath10k/QCA6174/hw3.0/board.bin")) {
        usb_write("[!] Failed to stage OEM QCA6174 board.bin into /tools/firmware\n");
        return;
    }

    bool rebound = false;
    if (access("/sys/bus/pci/drivers/ath10k_pci/0000:01:00.0", F_OK) == 0) {
        /* Wait briefly for early async register_work so unbind doesn't race init */
        for (int i = 0; i < 40; i++) {
            if (access("/sys/class/net/wlan0", F_OK) == 0)
                break;
            usleep(50000);
        }
        write_str("/sys/bus/pci/drivers/ath10k_pci/unbind", "0000:01:00.0");
        usleep(100000);
        write_str("/sys/bus/pci/drivers/ath10k_pci/bind", "0000:01:00.0");
        for (int i = 0; i < 60; i++) {
            if (access("/sys/class/net/wlan0", F_OK) == 0)
                break;
            usleep(50000);
        }
        rebound = true;
    } else if (access("/sys/bus/pci/devices/0000:01:00.0", F_OK) == 0) {
        write_str("/sys/bus/pci/drivers/ath10k_pci/bind", "0000:01:00.0");
        for (int i = 0; i < 60; i++) {
            if (access("/sys/class/net/wlan0", F_OK) == 0)
                break;
            usleep(50000);
        }
        rebound = true;
    }

    int mfd = open("/tmp/.ath10k_oem_bd_loaded", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (mfd >= 0)
        close(mfd);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "[+] Staged OEM QCA6174 board data %s (%lld bytes) -> board.bin%s\n",
             src_bd, (long long)st.st_size,
             rebound ? " (rebound ath10k_pci)" : "");
    usb_write(msg);
}

static bool prepare_android_rootfs(bool for_shell) {
    /* Keep kernel /dev/kmsg ratelimiting enabled so noisy BPF Rust loaders cannot
     * stall early init/userspace for ~57s dumping unbuffered messages to /dev/kmsg. */
    write_str("/proc/sys/kernel/printk_devkmsg", "ratelimit\n");
    rm_rf("/dev/__properties__");
    rm_rf("/dev/socket");
    mkdir("/dev/socket", 0755);
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, NULL);
    mkdir("/dev/dm-user", 0755);
    symlink("/dev/mapper/control", "/dev/device-mapper");
    set_selinux_label("/dev/mapper/control", "u:object_r:dm_device:s0");
    set_selinux_label("/dev/loop-control", "u:object_r:loop_control_device:s0");
    unlink("/dev/.coldboot_done");

    mkdir("/mnt", 0755);
    mkdir("/mnt/system_a", 0755);
    mkdir("/mnt/vendor_a", 0755);

    /* Mount system_a (sda33, fallback to sda35) rw and vendor_a (sda31) ro */
    const char *sys_dev = "/dev/sda33";
    int mrc = -1;
    if (mount("/dev/sda33", "/mnt/system_a", "ext4", MS_REMOUNT, NULL) == 0 ||
        mount("/dev/sda33", "/mnt/system_a", "ext4", 0, NULL) == 0 ||
        mount("/dev/sda33", "/mnt/system_a", "ext4", MS_RDONLY, NULL) == 0) {
        mrc = 0;
        sys_dev = "/dev/sda33";
    } else if (access("/mnt/system_a/system/bin/init", F_OK) == 0) {
        mrc = 0;
        sys_dev = "/dev/sda33";
    } else {
        if (mount("/dev/sda35", "/mnt/system_a", "ext4", MS_REMOUNT, NULL) == 0 ||
            mount("/dev/sda35", "/mnt/system_a", "ext4", 0, NULL) == 0) {
            mrc = 0;
            sys_dev = "/dev/sda35";
            usb_write("[+] Mounted system root from /dev/sda35 (userdata)\n");
        }
    }
    mount("/dev/sda31", "/mnt/vendor_a", "ext4", MS_RDONLY, NULL);
    stage_oem_wifi_board_data();

    /* Bind vendor into system_a/vendor */
    mkdir("/mnt/system_a/vendor", 0755);
    mount("/mnt/vendor_a", "/mnt/system_a/vendor", NULL, MS_BIND | MS_REC, NULL);

    /* Mount persistent unencrypted /dev/sda35 -> /data when safe, otherwise tmpfs */
    mount_userdata_partition(sys_dev);

    /* Clear volatile software-KeyMint keystore & synthetic-password state before Android
     * init and keystore2 start so LockSettingsService.unlockUserKeyIfUnsecured(0) never
     * fails with Error::Km(r#INVALID_KEY_BLOB) on subsequent boots. */
    rm_rf("/mnt/system_a/data/system_de/0/spblob");
    unlink("/mnt/system_a/data/system/locksettings.db");
    unlink("/mnt/system_a/data/system/locksettings.db-wal");
    unlink("/mnt/system_a/data/system/locksettings.db-shm");
    rm_rf("/mnt/system_a/data/misc/keystore");
    mkdir("/mnt/system_a/data/misc/keystore", 0700);
    chown("/mnt/system_a/data/misc/keystore", 1017, 1017);
    chmod("/mnt/system_a/data/misc/keystore", 0700);
    set_selinux_label("/mnt/system_a/data/misc/keystore", "u:object_r:keystore_data_file:s0");
    rm_rf("/mnt/system_a/data/misc/gatekeeper");

    /* Preferred: Mesa Freedreno (gallium/msm) real GLES driver.
     * Build a tmpfs copy of /vendor/lib64/egl, overlay the Mesa libs (named *_adreno.so so
     * ro.hardware.egl=adreno keeps working) plus libgallium_dri.so (found via RUNPATH=$ORIGIN),
     * then bind it over the read-only vendor egl directory. */
    bool mesa_installed = false;
    if (access("/tools/mesa/libgallium_dri.so", F_OK) == 0 &&
        access("/mnt/system_a/vendor/lib64/egl", F_OK) == 0) {
        mkdir("/mesa_egl", 0755);
        if (mount("tmpfs", "/mesa_egl", "tmpfs", 0, "mode=0755") == 0) {
            const char *dirs[] = { "/mnt/system_a/vendor/lib64/egl", "/tools/mesa" };
            for (int d = 0; d < 2; d++) {
                DIR *dp = opendir(dirs[d]);
                if (!dp) continue;
                struct dirent *de;
                while ((de = readdir(dp)) != NULL) {
                    if (de->d_name[0] == '.') continue;
                    char src[512], dst[512];
                    snprintf(src, sizeof(src), "%s/%s", dirs[d], de->d_name);
                    snprintf(dst, sizeof(dst), "/mesa_egl/%s", de->d_name);
                    unlink(dst);
                    if (copy_file(src, dst)) {
                        chmod(dst, 0644);
                        set_selinux_label(dst, "u:object_r:same_process_hal_file:s0");
                    }
                }
                closedir(dp);
            }
            set_selinux_label("/mesa_egl", "u:object_r:same_process_hal_file:s0");
            if (mount("/mesa_egl", "/mnt/system_a/vendor/lib64/egl", NULL, MS_BIND, NULL) == 0) {
                mesa_installed = true;
                usb_write("[+] Automated: Mesa Freedreno GLES installed over /vendor/lib64/egl\n");
            } else {
                usb_write("[-] Mesa egl bind failed, falling back to EGL shim\n");
            }
            if (access("/tools/libadreno_utils.so", F_OK) == 0 &&
                access("/mnt/system_a/vendor/lib64/libadreno_utils.so", F_OK) == 0) {
                if (copy_file("/tools/libadreno_utils.so", "/mesa_egl/libadreno_utils.so")) {
                    chmod("/mesa_egl/libadreno_utils.so", 0644);
                    set_selinux_label("/mesa_egl/libadreno_utils.so", "u:object_r:same_process_hal_file:s0");
                    if (mount("/mesa_egl/libadreno_utils.so", "/mnt/system_a/vendor/lib64/libadreno_utils.so", NULL, MS_BIND, NULL) == 0) {
                        usb_write("[+] Automated: Bound open-source Adreno 530 libadreno_utils.so -> /vendor/lib64/libadreno_utils.so\n");
                    }
                }
            }
        }
    }

    /* Fallback: Bind-mount mock Bionic EGL/GLES shim over legacy vendor Adreno blobs */
    if (!mesa_installed && access("/tools/libEGL_adreno_shim.so", F_OK) == 0) {
        if (access("/mnt/system_a/vendor/lib64/egl/libEGL_adreno.so", F_OK) == 0) {
            if (mount("/tools/libEGL_adreno_shim.so", "/mnt/system_a/vendor/lib64/egl/libEGL_adreno.so", NULL, MS_BIND, NULL) == 0) {
                usb_write("[+] Automated: Bound libEGL_adreno_shim.so -> /vendor/lib64/egl/libEGL_adreno.so\n");
            }
        }
        if (access("/mnt/system_a/vendor/lib64/egl/libGLESv2_adreno.so", F_OK) == 0) {
            if (mount("/tools/libEGL_adreno_shim.so", "/mnt/system_a/vendor/lib64/egl/libGLESv2_adreno.so", NULL, MS_BIND, NULL) == 0) {
                usb_write("[+] Automated: Bound libEGL_adreno_shim.so -> /vendor/lib64/egl/libGLESv2_adreno.so\n");
            }
        }
    }

    /* Automated: Modernize vendor vndservicemanager with Android 17 AIDL servicemanager */
    if (access("/mnt/system_a/system/bin/servicemanager", F_OK) == 0 &&
        access("/mnt/system_a/vendor/bin/vndservicemanager", F_OK) == 0) {
        if (mount("/mnt/system_a/system/bin/servicemanager", "/mnt/system_a/vendor/bin/vndservicemanager", NULL, MS_BIND, NULL) == 0) {
            usb_write("[+] Automated: Bound Android 17 servicemanager -> /vendor/bin/vndservicemanager\n");
        }
    }

    /* Automated: Strip /data (fileencryption=ice) from /vendor/etc/fstab.sailfish so mount_all --late
     * preserves our in-memory tmpfs /data and never triggers FBE keymaster waits */
    if (access("/mnt/system_a/vendor/etc/fstab.sailfish", F_OK) == 0) {
        mkdir("/tmp", 0755);
        FILE *fs_in = fopen("/mnt/system_a/vendor/etc/fstab.sailfish", "r");
        FILE *fs_out = fopen("/tmp/fstab.sailfish", "w");
        if (fs_in && fs_out) {
            char line[512];
            while (fgets(line, sizeof(line), fs_in)) {
                if (strstr(line, "/userdata") || strstr(line, "\t/data\t") || strstr(line, " /data "))
                    continue;
                fputs(line, fs_out);
            }
        }
        if (fs_in) fclose(fs_in);
        if (fs_out) {
            fclose(fs_out);
            chmod("/tmp/fstab.sailfish", 0644);
            set_selinux_label("/tmp/fstab.sailfish", "u:object_r:vendor_configs_file:s0");
            if (mount("/tmp/fstab.sailfish", "/mnt/system_a/vendor/etc/fstab.sailfish", NULL, MS_BIND, NULL) == 0) {
                usb_write(g_data_persistent ?
                          "[+] Automated: Overlaid /vendor/etc/fstab.sailfish (persistent /dev/sda35 /data)\n" :
                          "[+] Automated: Overlaid /vendor/etc/fstab.sailfish (unencrypted tmpfs /data)\n");
            }
        }
    }

    /* Keep real /system/bin/vdc active so hw/init.rc executes vdc checkpoint,
     * vdc keymaster earlyBootEnded (against keystore2), and vdc cryptfs init_user0. */

    /* Automated: Bind pause_stub over failing/un-linkable Android 10 vendor daemons & HALs
     * so Android init sees them sleeping cleanly (0% CPU) instead of crash-looping */
    if (access("/tools/pause_stub", F_OK) == 0) {
        set_selinux_label("/tools/pause_stub", "u:object_r:vendor_file:s0");
        chmod("/tools/pause_stub", 0755);
        const char *legacy_daemons[] = {
            "/mnt/system_a/vendor/bin/qseecomd",
            "/mnt/system_a/vendor/bin/time_daemon",
            "/mnt/system_a/vendor/bin/adsprpcd",
            "/mnt/system_a/vendor/bin/thermal-engine",
            "/mnt/system_a/vendor/bin/cnd",
            "/mnt/system_a/vendor/bin/cnss-daemon",
            "/mnt/system_a/vendor/bin/msm_irqbalance",
            "/mnt/system_a/vendor/bin/netmgrd",
            "/mnt/system_a/vendor/bin/imsdatadaemon",
            "/mnt/system_a/vendor/bin/hw/rild",
            "/mnt/system_a/vendor/bin/hw/android.hardware.keymaster@3.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.atrace@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.audio@2.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.boot@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.configstore@1.1-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.contexthub@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.biometrics.fingerprint@2.1-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.bluetooth@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.bluetooth@1.0-service-qti",
            "/mnt/system_a/vendor/bin/hw/android.hardware.camera.provider@2.4-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.cas@1.1-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.drm@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.drm@1.1-service.widevine",
            "/mnt/system_a/vendor/bin/hw/android.hardware.drm@1.2-service.clearkey",
            "/mnt/system_a/vendor/bin/hw/android.hardware.dumpstate@1.0-service.marlin",
            "/mnt/system_a/vendor/bin/hw/android.hardware.gatekeeper@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.gatekeeper@1.0-service-qti",
            "/mnt/system_a/vendor/bin/hw/android.hardware.gnss@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.gnss@1.0-service-qti",
            "/mnt/system_a/vendor/bin/hw/android.hardware.light@2.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.media.omx@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.memtrack@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.memtrack@1.0-service.marlin",
            "/mnt/system_a/vendor/bin/hw/android.hardware.nfc@1.1-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.oemlock@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.radio.deprecated@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.sensors@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.soundtrigger@2.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.tetheroffload.config@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.tetheroffload.control@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.thermal@2.0-service.pixel",
            "/mnt/system_a/vendor/bin/hw/android.hardware.usb@1.1-service.marlin",
            "/mnt/system_a/vendor/bin/hw/android.hardware.vibrator@1.0-service.marlin",
            "/mnt/system_a/vendor/bin/hw/android.hardware.vr@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.weaver@1.0-service",
            "/mnt/system_a/vendor/bin/hw/android.hardware.wifi@1.0-service",
            "/mnt/system_a/vendor/bin/hw/wpa_supplicant",
            "/mnt/system_a/vendor/bin/pm-service",
            "/mnt/system_a/vendor/bin/pm-proxy",
            "/mnt/system_a/vendor/bin/nanohub_slpi",
            "/mnt/system_a/vendor/bin/loc_launcher",
            NULL
        };
        for (int d = 0; legacy_daemons[d]; d++) {
            if (access(legacy_daemons[d], F_OK) == 0) {
                umount_all(legacy_daemons[d]);
                if (mount("/tools/pause_stub", legacy_daemons[d], NULL, MS_BIND, NULL) == 0) {
                    char dmsg[192];
                    snprintf(dmsg, sizeof(dmsg), "[+] Automated: Bound pause_stub -> %s\n", legacy_daemons[d]);
                    usb_write(dmsg);
                }
            }
        }

        /* Stub system daemons with their original SELinux contexts:
         * - odsign: missing keymaster/fs-verity would trigger InitFatalReboot after 4 crashes
         * - audioserver: A17 libaudiohal removed audio@2.0/4.0, causing NULL deref crash loop
         * - traced/traced_probes: exit(1) loop triggers flags_health_check UPDATABLE_CRASHING CPU storm
         * - flags_health_check: bind vdc_stub so SVC_EXEC returns 0 immediately at 0% CPU */
        mkdir("/tmp", 0755);
        struct {
            const char *src_stub;
            const char *bin_path;
            const char *stub_path;
            const char *fallback_label;
        } sys_stubs[] = {
            { "/tools/pause_stub", "/mnt/system_a/system/bin/odsign", "/tmp/odsign_stub", "u:object_r:system_file:s0" },
            { "/tools/pause_stub", "/mnt/system_a/system/bin/audioserver", "/tmp/audioserver_stub", "u:object_r:audioserver_exec:s0" },
            { "/tools/pause_stub", "/mnt/system_a/system/bin/traced", "/tmp/traced_stub", "u:object_r:traced_exec:s0" },
            { "/tools/pause_stub", "/mnt/system_a/system/bin/traced_probes", "/tmp/traced_probes_stub", "u:object_r:traced_probes_exec:s0" },
            { "/tools/vdc_stub", "/mnt/system_a/system/bin/flags_health_check", "/tmp/flags_health_check_stub", "u:object_r:system_file:s0" },
            { "/tools/vdc_stub", "/mnt/system_a/system/bin/usbd", "/tmp/usbd_stub", "u:object_r:usbd_exec:s0" },
            { NULL, NULL, NULL, NULL }
        };
        for (int s = 0; sys_stubs[s].bin_path; s++) {
            if (access(sys_stubs[s].src_stub, F_OK) == 0 &&
                access(sys_stubs[s].bin_path, F_OK) == 0 &&
                copy_file(sys_stubs[s].src_stub, sys_stubs[s].stub_path)) {
                char lbl[128] = {0};
                const char *use_lbl = sys_stubs[s].fallback_label;
                if (getxattr(sys_stubs[s].bin_path, "security.selinux", lbl, sizeof(lbl) - 1) > 0) {
                    use_lbl = lbl;
                }
                chmod(sys_stubs[s].stub_path, 0755);
                set_selinux_label(sys_stubs[s].stub_path, use_lbl);
                umount_all(sys_stubs[s].bin_path);
                if (mount(sys_stubs[s].stub_path, sys_stubs[s].bin_path, NULL, MS_BIND, NULL) == 0) {
                    char smsg[192];
                    snprintf(smsg, sizeof(smsg), "[+] Automated: Bound %s -> %s\n", sys_stubs[s].src_stub, sys_stubs[s].bin_path);
                    usb_write(smsg);
                }
            }
        }
    }

    /* Automated: Hide unused crash-looping/unneeded APKs so PackageManagerService skips them */
    mkdir("/tmp", 0755);
    mkdir("/tmp/empty_app_dir", 0755);
    umount_all("/tmp/empty_app_dir");
    if (mount("tmpfs", "/tmp/empty_app_dir", "tmpfs", 0, "mode=0755,uid=0,gid=0") == 0) {
        set_selinux_label("/tmp/empty_app_dir", "u:object_r:system_file:s0");
        const char *empty_app_dirs[] = {
            "/mnt/system_a/system/app/NfcNci",
            "/mnt/system_a/system/priv-app/Tag",
            "/mnt/system_a/vendor/app",
            NULL
        };
        for (int i = 0; empty_app_dirs[i]; i++) {
            if (access(empty_app_dirs[i], F_OK) == 0 &&
                bind_mount_clean("/tmp/empty_app_dir", empty_app_dirs[i]) == 0) {
                char amsg[192];
                snprintf(amsg, sizeof(amsg), "[+] Automated: Overlaid empty dir -> %s\n", empty_app_dirs[i]);
                usb_write(amsg);
            }
        }
    }

    /* Automated: Filter NFC and Bluetooth permissions out of /vendor/etc/permissions so
     * PackageManagerService does not respawn NfcNciApex or crash-loop com.android.bluetooth. */
    const char *vperm_dir = "/mnt/system_a/vendor/etc/permissions";
    if (access(vperm_dir, F_OK) == 0) {
        mkdir("/tmp", 0755);
        mkdir("/tmp/vendor_permissions", 0755);
        umount_all("/tmp/vendor_permissions");
        if (mount("tmpfs", "/tmp/vendor_permissions", "tmpfs", 0, "mode=0755,uid=0,gid=0") == 0) {
            set_selinux_label("/tmp/vendor_permissions", "u:object_r:vendor_configs_file:s0");
            DIR *pd = opendir(vperm_dir);
            if (pd) {
                struct dirent *de;
                while ((de = readdir(pd)) != NULL) {
                    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                        continue;
                    if (strcasestr(de->d_name, "nfc") || strcasestr(de->d_name, "mifare") ||
                        strcasestr(de->d_name, "bluetooth"))
                        continue;
                    char src_path[512], dst_path[512];
                    snprintf(src_path, sizeof(src_path), "%s/%s", vperm_dir, de->d_name);
                    snprintf(dst_path, sizeof(dst_path), "/tmp/vendor_permissions/%s", de->d_name);
                    struct stat pst;
                    if (stat(src_path, &pst) == 0 && S_ISREG(pst.st_mode)) {
                        if (copy_file(src_path, dst_path)) {
                            chmod(dst_path, 0644);
                            set_selinux_label(dst_path, "u:object_r:vendor_configs_file:s0");
                        }
                    }
                }
                closedir(pd);
            }
            const char *unavail_xml =
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<permissions>\n"
                "    <unavailable-feature name=\"android.hardware.bluetooth\" />\n"
                "    <unavailable-feature name=\"android.hardware.bluetooth_le\" />\n"
                "    <unavailable-feature name=\"android.hardware.nfc\" />\n"
                "    <unavailable-feature name=\"android.hardware.nfc.hce\" />\n"
                "    <unavailable-feature name=\"android.hardware.nfc.hcef\" />\n"
                "    <unavailable-feature name=\"android.hardware.nfc.uicc\" />\n"
                "    <unavailable-feature name=\"android.hardware.nfc.ese\" />\n"
                "    <unavailable-feature name=\"com.nxp.mifare\" />\n"
                "</permissions>\n";
            const char *unavail_path = "/tmp/vendor_permissions/sailfish_unavailable_features.xml";
            write_file_str(unavail_path, unavail_xml);
            chmod(unavail_path, 0644);
            set_selinux_label(unavail_path, "u:object_r:vendor_configs_file:s0");
            if (bind_mount_clean("/tmp/vendor_permissions", vperm_dir) == 0) {
                usb_write("[+] Automated: Filtered NFC/Mifare/Bluetooth XMLs out of /vendor/etc/permissions\n");
            }
        }
    }

    /* Automated: Silence vendor init.usb.rc so Android 10 vendor USB triggers never bounce
     * g1/UDC or launch adbd independently of overlay_usb_configfs_rc(). */
    const char *vusb_rc = "/mnt/system_a/vendor/etc/init/init.usb.rc";
    if (access(vusb_rc, F_OK) == 0) {
        const char *tmp_vusb_rc = "/tmp/vendor_init_usb.rc";
        char lbl[128] = {0};
        copy_selinux_label_or(vusb_rc, lbl, sizeof(lbl), "u:object_r:vendor_configs_file:s0");
        write_file_str(tmp_vusb_rc, "# Silenced by sailfish-init to preserve CDC-ACM gadget\n");
        chmod(tmp_vusb_rc, 0644);
        set_selinux_label(tmp_vusb_rc, lbl);
        if (bind_mount_clean(tmp_vusb_rc, vusb_rc) == 0) {
            usb_write("[+] Automated: Silenced /vendor/etc/init/init.usb.rc (preserving CDC-ACM)\n");
        }
    }

    /* Automated: Strip 'onrestart restart surfaceflinger' from composer service definition so
     * any HAL hiccup never kills SurfaceFlinger */
    overlay_filtered_rc(
        "/mnt/system_a/vendor/etc/init/android.hardware.graphics.composer@2.1-service.rc",
        "/tmp/composer.rc",
        "u:object_r:vendor_configs_file:s0",
        drop_composer_restart,
        "[+] Automated: Overlaid composer@2.1-service.rc (removed onrestart restart surfaceflinger)\n");

    /* Automated: Prevent vendor init.rc 'on post-fs-data' from starting netd prematurely
     * before 'on load-bpf-programs' sets bpf.progs_loaded=1 (stops netd crash-loop). */
    overlay_filtered_rc(
        "/mnt/system_a/vendor/etc/init/init.rc",
        "/tmp/vendor_init.rc",
        "u:object_r:vendor_configs_file:s0",
        drop_start_netd,
        "[+] Automated: Overlaid vendor/etc/init/init.rc (removed premature start netd)\n");

    /* Bring-up safety: Strip reboot_on_failure from netd1shot so a netd1shot setup glitch
     * does not trigger a hard bootloader reboot loop before we can inspect logs. */
    overlay_filtered_rc(
        "/mnt/system_a/system/etc/init/netd.rc",
        "/tmp/netd.rc",
        "u:object_r:system_file:s0",
        drop_netd1shot_reboot,
        "[+] Automated: Overlaid system/etc/init/netd.rc (removed netd1shot reboot_on_failure)\n");

    overlay_adb_build_prop();
    overlay_usb_configfs_rc();

    /* Automated: Bind wrap_preload over graphics HALs & surfaceflinger so they ALWAYS load libsdm_shim.so
     * natively on startup, preventing SIGSEGV in libsdmcore and unlocking hwcomposer + surfaceflinger */
    if (access("/tools/wrap_preload", F_OK) == 0) {
        chmod("/tools/wrap_preload", 0755);
        /* Clean up any leftover empty /system/vendor_bin from earlier test boots */
        rmdir("/mnt/system_a/system/vendor_bin");
        struct {
            const char *service_path;
            const char *real_path;
            const char *label;
        } wrap_targets[] = {
            { "/mnt/system_a/vendor/bin/hw/android.hardware.graphics.composer@2.1-service",
              "/mnt/system_a/system/bin/android.hardware.graphics.composer@2.1-service.real",
              "u:object_r:hal_graphics_composer_default_exec:s0" },
            { "/mnt/system_a/vendor/bin/hw/android.hardware.graphics.allocator@2.0-service",
              "/mnt/system_a/system/bin/android.hardware.graphics.allocator@2.0-service.real",
              "u:object_r:hal_graphics_allocator_default_exec:s0" },
            { "/mnt/system_a/system/bin/surfaceflinger",
              "/mnt/system_a/system/bin/surfaceflinger.real",
              "u:object_r:surfaceflinger_exec:s0" },
            { "/mnt/system_a/system/bin/bootanimation",
              "/mnt/system_a/system/bin/bootanimation.real",
              "u:object_r:bootanim_exec:s0" },
            { "/mnt/system_a/vendor/bin/hw/android.hardware.health@2.0-service.marlin",
              "/mnt/system_a/system/bin/android.hardware.health@2.0-service.marlin.real",
              "u:object_r:hal_health_default_exec:s0" },
            { "/mnt/system_a/vendor/bin/hw/android.hardware.power@1.1-service.marlin",
              "/mnt/system_a/system/bin/android.hardware.power@1.1-service.marlin.real",
              "u:object_r:hal_power_default_exec:s0" },
            { "/mnt/system_a/system/bin/keystore2",
              "/mnt/system_a/system/bin/keystore2.real",
              "u:object_r:keystore_exec:s0" },
            { NULL, NULL, NULL }
        };

        for (int i = 0; wrap_targets[i].service_path; i++) {
            const char *svc = wrap_targets[i].service_path;
            const char *real = wrap_targets[i].real_path;
            const char *lbl = wrap_targets[i].label;

            umount_all(svc);
            umount_all(real);

            if (access(svc, F_OK) == 0) {
                if (!is_valid_wrap_target(svc)) {
                    char msg[192];
                    snprintf(msg, sizeof(msg), "[!] Automated: Skipping invalid/wrapped target -> %s\n", svc);
                    usb_write(msg);
                    continue;
                }

                char lbl_buf[128] = {0};
                const char *use_lbl = lbl;
                if (getxattr(svc, "security.selinux", lbl_buf, sizeof(lbl_buf) - 1) > 0) {
                    use_lbl = lbl_buf;
                }

                unlink(real);
                if (access(real, F_OK) != 0) {
                    int rfd = open(real, O_WRONLY | O_CREAT | O_EXCL, 0755);
                    if (rfd >= 0) close(rfd);
                }
                chmod(real, 0755);
                set_selinux_label(real, use_lbl);

                if (bind_mount_clean(svc, real) == 0) {
                    char wrap_copy[128];
                    snprintf(wrap_copy, sizeof(wrap_copy), "/tmp/wrap_%d", i);
                    copy_file("/tools/wrap_preload", wrap_copy);
                    chmod(wrap_copy, 0755);
                    set_selinux_label(wrap_copy, use_lbl);

                    if (bind_mount_clean(wrap_copy, svc) == 0) {
                        char msg[192];
                        snprintf(msg, sizeof(msg), "[+] Automated: Bound LD_PRELOAD wrapper -> %s\n", svc);
                        usb_write(msg);
                    } else {
                        umount_all(real);
                    }
                }
            }
        }
    }

    /* Automated: Create native .rc files in /system/etc/init and /vendor/etc/init so Android init
     * automatically executes essential properties and service stops on boot */
    const char *dexopt_props = g_data_persistent ?
        "    setprop dalvik.vm.boot-dex2oat-threads 2\n"
        "    setprop dalvik.vm.dex2oat-threads 2\n"
        "    setprop dalvik.vm.background-dex2oat-threads 1\n" :
        "    setprop dalvik.vm.dexopt-flags v=n\n"
        "    setprop dalvik.vm.extra-opts -Xverify:none\n"
        "    setprop pm.dexopt.first-boot skip\n"
        "    setprop pm.dexopt.boot-after-ota skip\n";
    char rc_content[8192];
    snprintf(rc_content, sizeof(rc_content),
        "on early-init\n"
        "    setprop ro.crypto.state unencrypted\n"
        "    setprop ro.hw_timeout_multiplier 10\n"
        "    setprop sys.use_memfd true\n"
        "    setprop debug.gralloc.gfx_ubwc_disable 1\n"
        "    setprop debug.gralloc.enable_fb_ubwc 0\n"
        "    setprop service.sf.prime_shader_cache 0\n"
        "    setprop ro.surface_flinger.supports_background_blur 0\n"
        "    setprop persist.sys.sf.disable_blurs 1\n"
        "%s"
        "    copy /data/ld.config.sailfish.txt /linkerconfig/ld.config.txt\n"
        "    chmod 0644 /linkerconfig/ld.config.txt\n"
        "    chmod 0666 /dev/ion\n"
        "    chmod 0666 /dev/dri/card0\n"
        "\n"
        "on post-fs\n"
        "    stop vendor.qseecomd\n"
        "    stop vendor.time_daemon\n"
        "    stop vendor.keymaster-3-0\n"
        "    stop vendor.configstore-hal\n"
        "    setprop sys.listeners.registered true\n"
        "    setprop keystore.module_hash.sent true\n"
        "    setprop odsign.key.done 1\n"
        "    setprop odsign.verification.done 1\n"
        "    copy /data/ld.config.sailfish.txt /linkerconfig/ld.config.txt\n"
        "    chmod 0644 /linkerconfig/ld.config.txt\n"
        "    chmod 0666 /dev/ion\n"
        "    chmod 0666 /dev/dma_heap/system\n"
        "    chmod 0666 /dev/dma_heap/system-uncached\n"
        "\n"
        "on post-fs-data\n"
        "    setprop sys.time.set true\n"
        "    setprop keystore.module_hash.sent true\n"
        "    setprop odsign.key.done 1\n"
        "    setprop odsign.verification.done 1\n"
        "%s"
        "    chmod 0664 /sys/fs/cgroup/cgroup.procs\n"
        "    copy /data/ld.config.sailfish.txt /linkerconfig/ld.config.txt\n"
        "    chmod 0644 /linkerconfig/ld.config.txt\n"
        "    exec - root root -- /system/bin/sh /data/sailfish_ensure_user0.sh\n"
        "    start vndservicemanager\n"
        "\n"
        "on early-fs\n"
        "    copy /data/ld.config.sailfish.txt /linkerconfig/ld.config.txt\n"
        "    chmod 0644 /linkerconfig/ld.config.txt\n"
        "    copy /data/ld.config.sailfish.txt /linkerconfig/bootstrap/ld.config.txt\n"
        "    chmod 0644 /linkerconfig/bootstrap/ld.config.txt\n"
        "    copy /data/ld.config.sailfish.txt /linkerconfig/default/ld.config.txt\n"
        "    chmod 0644 /linkerconfig/default/ld.config.txt\n"
        "\n"
        "on property:apexd.status=activated\n"
        "    copy /data/ld.config.sailfish.txt /linkerconfig/ld.config.txt\n"
        "    chmod 0644 /linkerconfig/ld.config.txt\n"
        "\n"
        "on load-bpf-programs\n"
        "    copy /data/ld.config.sailfish.txt /linkerconfig/ld.config.txt\n"
        "    chmod 0644 /linkerconfig/ld.config.txt\n"
        "\n"
        "on zygote-start\n"
        "%s"
        "    copy /data/ld.config.sailfish.txt /linkerconfig/ld.config.txt\n"
        "    chmod 0644 /linkerconfig/ld.config.txt\n"
        "\n"
        "on boot\n"
        "    chmod 0666 /dev/ion\n"
        "    chmod 0666 /dev/dma_heap/system\n"
        "    chmod 0666 /dev/dma_heap/system-uncached\n"
        "    chmod 0666 /dev/dri/card0\n"
        "    exec - root root -- /system/bin/sh /data/start_qservice.sh\n"
        "    class_start hal\n"
        "    class_start core\n"
        "    trigger nonencrypted\n",
        dexopt_props, dexopt_props, dexopt_props);

    /* Install sailfish_boot triggers only once before hw/init.rc actions so
     * duplicate `on zygote-start` copies never truncate /linkerconfig/ld.config.txt
     * concurrently with hw/init.rc starting zygote (app_process64). */
    mkdir("/mnt/system_a/system/etc/init", 0755);
    unlink("/mnt/system_a/system/etc/init/sailfish_boot.rc");
    unlink("/mnt/system_a/vendor/etc/init/sailfish_boot.rc");

    mkdir("/tmp", 0755);
    if (access("/mnt/system_a/vendor/etc/init/sailfish_boot.rc", F_OK) == 0) {
        write_file_str("/tmp/empty.rc", "# neutralized stale vendor sailfish_boot.rc\n");
        chmod("/tmp/empty.rc", 0644);
        set_selinux_label("/tmp/empty.rc", "u:object_r:vendor_configs_file:s0");
        umount_all("/mnt/system_a/vendor/etc/init/sailfish_boot.rc");
        if (mount("/tmp/empty.rc", "/mnt/system_a/vendor/etc/init/sailfish_boot.rc",
                  NULL, MS_BIND, NULL) == 0) {
            usb_write("[+] Automated: Neutralized stale /vendor/etc/init/sailfish_boot.rc\n");
        }
    }

    mkdir("/tmp", 0755);
    write_file_str("/tmp/init.sailfish.rc", rc_content);
    chmod("/tmp/init.sailfish.rc", 0644);
    set_selinux_label("/tmp/init.sailfish.rc", "u:object_r:vendor_configs_file:s0");

    if (access("/mnt/system_a/vendor/etc/init/hw/init.sailfish.rc", F_OK) == 0 &&
        mount("/tmp/init.sailfish.rc", "/mnt/system_a/vendor/etc/init/hw/init.sailfish.rc",
              NULL, MS_BIND, NULL) == 0) {
        unlink("/mnt/system_a/init.sailfish.rc");
    } else {
        write_file_str("/mnt/system_a/init.sailfish.rc", rc_content);
        chmod("/mnt/system_a/init.sailfish.rc", 0644);
        set_selinux_label("/mnt/system_a/init.sailfish.rc", "u:object_r:rootfs:s0");
    }

    /* Idempotent fallback after hw/init.rc init_user0 so User 0 DE/CE dirs and
     * /data/data -> /data/user/0 bind mount always exist on unencrypted tmpfs /data. */
    const char *ensure_user0_sh =
        "#!/system/bin/sh\n"
        "ensure_dir() {\n"
        "    if [ ! -d \"$1\" ]; then\n"
        "        mkdir -p \"$1\" 2>/dev/null || true\n"
        "        chown \"$2\" \"$1\" 2>/dev/null || true\n"
        "        chmod \"$3\" \"$1\" 2>/dev/null || true\n"
        "        restorecon \"$1\" 2>/dev/null || true\n"
        "    fi\n"
        "}\n"
        "ensure_dir /data/data system:system 0771\n"
        "ensure_dir /data/user/0 system:system 0771\n"
        "ensure_dir /data/user_de/0 system:system 0771\n"
        "ensure_dir /data/system/users/0 system:system 0700\n"
        "ensure_dir /data/system_de/0 system:system 0770\n"
        "ensure_dir /data/system_ce/0 system:system 0770\n"
        "ensure_dir /data/misc_de/0 system:misc 01771\n"
        "ensure_dir /data/misc_ce/0 system:misc 01771\n"
        "ensure_dir /data/vendor_de/0 root:root 0771\n"
        "ensure_dir /data/vendor_ce/0 root:root 0771\n"
        "ensure_dir /data/singleton_de/0 system:system 0771\n"
        "ensure_dir /data/singleton_ce/0 system:system 0771\n"
        "ensure_dir /data/misc/user/0 root:root 0700\n"
        "ensure_dir /data/misc/profiles/cur/0 system:system 0771\n"
        "ensure_dir /data/misc/profiles/ref/0 system:system 0771\n"
        "ensure_dir /data/media/0 media_rw:media_rw 0770\n"
        "ensure_dir /data/media/0/Android media_rw:media_rw 0770\n"
        "ensure_dir /data/media/obb media_rw:media_rw 0770\n"
        "rm -rf /data/system_de/0/spblob /data/system/locksettings.db* 2>/dev/null || true\n"
        "if [ ! -L /data/user/0 ] && ! grep -q \" /data/user/0 \" /proc/mounts; then\n"
        "    mount -o bind /data/data /data/user/0 2>/dev/null || true\n"
        "fi\n"
        "if [ ! -d /data/misc_de/0/vold ]; then\n"
        "    /system/bin/vold_prepare_subdirs prepare \"\" 0 1 2>/dev/null || true\n"
        "fi\n"
        "if [ ! -d /data/misc_ce/0/vold ]; then\n"
        "    /system/bin/vold_prepare_subdirs prepare \"\" 0 2 2>/dev/null || true\n"
        "fi\n"
        "if [ -s /data/ld.config.sailfish.txt ]; then\n"
        "    if cp /data/ld.config.sailfish.txt /linkerconfig/.ld.config.tmp 2>/dev/null && [ -s /linkerconfig/.ld.config.tmp ]; then\n"
        "        chmod 0644 /linkerconfig/.ld.config.tmp 2>/dev/null || true\n"
        "        chcon u:object_r:linkerconfig_file:s0 /linkerconfig/.ld.config.tmp 2>/dev/null || restorecon /linkerconfig/.ld.config.tmp 2>/dev/null || true\n"
        "        mv -f /linkerconfig/.ld.config.tmp /linkerconfig/ld.config.txt 2>/dev/null || rm -f /linkerconfig/.ld.config.tmp 2>/dev/null || true\n"
        "    else\n"
        "        rm -f /linkerconfig/.ld.config.tmp 2>/dev/null || true\n"
        "    fi\n"
        "fi\n"
        "if ! losetup /dev/block/loop95 >/dev/null 2>&1; then\n"
        "    [ ! -s /data/frp.img ] && dd if=/dev/zero of=/data/frp.img bs=1024 count=512 2>/dev/null || true\n"
        "    losetup /dev/block/loop95 /data/frp.img 2>/dev/null || true\n"
        "fi\n"
        "chown system:system /dev/block/loop95 2>/dev/null || true\n"
        "chmod 0660 /dev/block/loop95 2>/dev/null || true\n"
        "chcon u:object_r:frp_block_device:s0 /dev/block/loop95 2>/dev/null || true\n"
        "ln -sf /dev/block/loop95 /dev/block/platform/soc/624000.ufshc/by-name/frp 2>/dev/null || true\n"
        "ln -sf /dev/block/loop95 /dev/block/bootdevice/by-name/frp 2>/dev/null || true\n"
        "ln -sf /dev/block/loop95 /dev/block/by-name/frp 2>/dev/null || true\n"
        "# Keep kernel schedutil CPUfreq governor active\n"
        "if [ -d /apex/com.android.nfcservices/priv-app ]; then\n"
        "    mount -t tmpfs tmpfs /apex/com.android.nfcservices/priv-app 2>/dev/null || true\n"
        "fi\n";
    write_file_str("/tmp/sailfish_ensure_user0.sh", ensure_user0_sh);
    chmod("/tmp/sailfish_ensure_user0.sh", 0755);
    set_selinux_label("/tmp/sailfish_ensure_user0.sh", "u:object_r:system_file:s0");
    write_file_str("/mnt/system_a/system/etc/sailfish_ensure_user0.sh", ensure_user0_sh);
    chmod("/mnt/system_a/system/etc/sailfish_ensure_user0.sh", 0755);
    set_selinux_label("/mnt/system_a/system/etc/sailfish_ensure_user0.sh", "u:object_r:system_file:s0");
    if (access("/mnt/system_a/system/etc/sailfish_ensure_user0.sh", F_OK) == 0) {
        bind_mount_clean("/tmp/sailfish_ensure_user0.sh", "/mnt/system_a/system/etc/sailfish_ensure_user0.sh");
    }

    const char *user0_rc =
        "on post-fs-data\n"
        "    exec - root root -- /system/bin/sh /system/etc/sailfish_ensure_user0.sh\n"
        "    start sailfish_power_aidl\n"
        "    start sailfish_sensors_aidl\n"
        "\n"
        "on boot\n"
        "    start sailfish_power_aidl\n"
        "    start sailfish_sensors_aidl\n"
        "\n"
        "service sailfish_power_aidl /system/bin/android.hardware.power-service.sailfish\n"
        "    interface aidl android.hardware.power.IPower/default\n"
        "    class hal\n"
        "    user system\n"
        "    group system\n"
        "\n"
        "service sailfish_sensors_aidl /system/bin/android.hardware.sensors-service.sailfish\n"
        "    interface aidl android.hardware.sensors.ISensors/default\n"
        "    class hal\n"
        "    user system\n"
        "    group system\n";
    const char *user0_rc_dst = "/mnt/system_a/system/etc/init/sailfish_user0_fallback.rc";
    const char *user0_rc_tmp = "/tmp/sailfish_user0_fallback.rc";
    write_file_str(user0_rc_tmp, user0_rc);
    chmod(user0_rc_tmp, 0644);
    set_selinux_label(user0_rc_tmp, "u:object_r:system_file:s0");
    umount_all(user0_rc_dst);
    int u0fd = open(user0_rc_dst, O_WRONLY | O_CREAT, 0644);
    if (u0fd >= 0) close(u0fd);
    if (bind_mount_clean(user0_rc_tmp, user0_rc_dst) == 0) {
        set_selinux_label(user0_rc_dst, "u:object_r:system_file:s0");
    } else {
        write_file_str(user0_rc_dst, user0_rc);
        chmod(user0_rc_dst, 0644);
        set_selinux_label(user0_rc_dst, "u:object_r:system_file:s0");
    }


    /* Automated: Overlay /vendor/etc/selinux on tmpfs (vendor_a is MS_RDONLY) and sync
     * GSI SELinux sha256 checksums so precompiled_sepolicy loads cleanly on stock vendor. */
    const char *vsel_dir = "/mnt/system_a/vendor/etc/selinux";
    const char *tmp_vsel = "/tmp/vendor_selinux";
    umount_all(vsel_dir);
    umount_all(tmp_vsel);
    if (access("/mnt/system_a/vendor/etc/selinux/precompiled_sepolicy", F_OK) == 0) {
        mkdir("/tmp", 0755);
        mkdir(tmp_vsel, 0755);
        if (mount("tmpfs", tmp_vsel, "tmpfs", 0, "mode=0755,uid=0,gid=0") == 0) {
            char dir_lbl[128] = {0};
            char sha_lbl[128] = {0};
            copy_selinux_label_or(vsel_dir, dir_lbl, sizeof(dir_lbl), "u:object_r:vendor_configs_file:s0");
            set_selinux_label(tmp_vsel, dir_lbl);
            copy_selinux_label_or("/mnt/system_a/vendor/etc/selinux/precompiled_sepolicy.plat_sepolicy_and_mapping.sha256",
                                  sha_lbl, sizeof(sha_lbl), "u:object_r:vendor_configs_file:s0");

            DIR *sd = opendir(vsel_dir);
            if (sd) {
                struct dirent *de;
                while ((de = readdir(sd)) != NULL) {
                    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                        continue;
                    char sp[512], dp[512], flbl[128];
                    snprintf(sp, sizeof(sp), "%s/%s", vsel_dir, de->d_name);
                    snprintf(dp, sizeof(dp), "%s/%s", tmp_vsel, de->d_name);
                    struct stat st;
                    if (lstat(sp, &st) != 0)
                        continue;
                    if (S_ISREG(st.st_mode)) {
                        copy_selinux_label_or(sp, flbl, sizeof(flbl), "u:object_r:vendor_configs_file:s0");
                        if (copy_file(sp, dp)) {
                            chmod(dp, st.st_mode & 0777);
                            set_selinux_label(dp, flbl);
                        }
                    } else if (S_ISLNK(st.st_mode)) {
                        char target[512];
                        ssize_t len = readlink(sp, target, sizeof(target) - 1);
                        if (len > 0) {
                            target[len] = '\0';
                            copy_selinux_label_or(sp, flbl, sizeof(flbl), "u:object_r:vendor_configs_file:s0");
                            if (symlink(target, dp) == 0)
                                lsetxattr(dp, "security.selinux", flbl, strlen(flbl) + 1, 0);
                        }
                    }
                }
                closedir(sd);
            }

            const char *sha_sources[][3] = {
                { "/mnt/system_a/system/etc/selinux/plat_sepolicy_and_mapping.sha256", NULL,
                  "precompiled_sepolicy.plat_sepolicy_and_mapping.sha256" },
                { "/mnt/system_a/system_ext/etc/selinux/system_ext_sepolicy_and_mapping.sha256",
                  "/mnt/system_a/system/system_ext/etc/selinux/system_ext_sepolicy_and_mapping.sha256",
                  "precompiled_sepolicy.system_ext_sepolicy_and_mapping.sha256" },
                { "/mnt/system_a/product/etc/selinux/product_sepolicy_and_mapping.sha256",
                  "/mnt/system_a/system/product/etc/selinux/product_sepolicy_and_mapping.sha256",
                  "precompiled_sepolicy.product_sepolicy_and_mapping.sha256" },
                { NULL, NULL, NULL }
            };
            int synced = 0;
            for (int i = 0; sha_sources[i][2]; i++) {
                const char *src_sha = (access(sha_sources[i][0], F_OK) == 0) ? sha_sources[i][0] :
                                      (sha_sources[i][1] && access(sha_sources[i][1], F_OK) == 0) ? sha_sources[i][1] : NULL;
                if (src_sha) {
                    char dst_sha[512];
                    snprintf(dst_sha, sizeof(dst_sha), "%s/%s", tmp_vsel, sha_sources[i][2]);
                    if (copy_file(src_sha, dst_sha)) {
                        chmod(dst_sha, 0644);
                        set_selinux_label(dst_sha, sha_lbl);
                        synced++;
                    }
                }
            }

            if (synced > 0 && bind_mount_clean(tmp_vsel, vsel_dir) == 0) {
                usb_write("[+] Automated: Synced SELinux sha256 checksums via /vendor/etc/selinux tmpfs overlay\n");
            }
        }
    }

    /* Automated: Ensure 29.0.cil compatibility symlinks exist across mapping dirs */
    const char *map_dirs[] = {
        "/mnt/system_a/system/etc/selinux/mapping",
        "/mnt/system_a/system_ext/etc/selinux/mapping",
        "/mnt/system_a/product/etc/selinux/mapping",
        NULL
    };
    for (int i = 0; map_dirs[i]; i++) {
        char link29[256], target31[256];
        snprintf(link29, sizeof(link29), "%s/29.0.cil", map_dirs[i]);
        snprintf(target31, sizeof(target31), "%s/31.0.cil", map_dirs[i]);
        if (access(link29, F_OK) != 0 && access(target31, F_OK) == 0) {
            symlink("31.0.cil", link29);
        }
        snprintf(link29, sizeof(link29), "%s/29.0.compat.cil", map_dirs[i]);
        snprintf(target31, sizeof(target31), "%s/31.0.compat.cil", map_dirs[i]);
        if (access(link29, F_OK) != 0 && access(target31, F_OK) == 0) {
            symlink("31.0.compat.cil", link29);
        }
    }

    /* Automated: Ensure ICU libraries are accessible in /system/lib64 for SurfaceFlinger */
    const char *icu_libs[] = {
        "libicu.so", "libicui18n.so", "libicuuc.so", "libandroidicu.so", "libicu_jni.so", NULL
    };
    for (int i = 0; icu_libs[i]; i++) {
        char target[256], linkpath[256];
        snprintf(target, sizeof(target), "/apex/com.android.i18n/lib64/%s", icu_libs[i]);
        snprintf(linkpath, sizeof(linkpath), "/mnt/system_a/system/lib64/%s", icu_libs[i]);
        unlink(linkpath);
        symlink(target, linkpath);
    }

    /* Automated: Create graphics and DRM compatibility device symlinks */
    mkdir("/dev/graphics", 0755);
    mkdir("/mnt/system_a/dev/graphics", 0755);
    if (access("/dev/fb0", F_OK) == 0) {
        symlink("/dev/fb0", "/dev/graphics/fb0");
        symlink("/dev/fb0", "/mnt/system_a/dev/graphics/fb0");
    }
    mkdir("/dev/dri", 0755);
    mkdir("/mnt/system_a/dev/dri", 0755);
    if (access("/dev/dri/card1", F_OK) == 0) {
        if (access("/dev/dri/card0", F_OK) != 0) symlink("/dev/dri/card1", "/dev/dri/card0");
        if (access("/mnt/system_a/dev/dri/card0", F_OK) != 0) symlink("/dev/dri/card1", "/mnt/system_a/dev/dri/card0");
    }

    /* Automated: Install AIDL Power HAL stub binary for HintManagerService (IPower/default) */
    if (access("/tools/power_hal", F_OK) == 0) {
        const char *pwr_dst = "/mnt/system_a/system/bin/android.hardware.power-service.sailfish";
        umount_all(pwr_dst);
        unlink(pwr_dst);
        if (copy_file("/tools/power_hal", pwr_dst)) {
            chmod(pwr_dst, 0755);
            set_selinux_label(pwr_dst, "u:object_r:hal_power_default_exec:s0");
            usb_write("[+] Automated: Installed /system/bin/android.hardware.power-service.sailfish\n");
        } else if (copy_file("/tools/power_hal", "/tmp/power_hal")) {
            chmod("/tmp/power_hal", 0755);
            set_selinux_label("/tmp/power_hal", "u:object_r:hal_power_default_exec:s0");
            int pfd = open(pwr_dst, O_WRONLY | O_CREAT | O_EXCL, 0755);
            if (pfd >= 0) close(pfd);
            if (bind_mount_clean("/tmp/power_hal", pwr_dst) == 0) {
                set_selinux_label(pwr_dst, "u:object_r:hal_power_default_exec:s0");
                usb_write("[+] Automated: Bind-mounted /system/bin/android.hardware.power-service.sailfish\n");
            }
        }
    }

    /* Automated: Install AIDL Sensors HAL stub (ISensors/default, 0 sensors) so
     * SensorService/SystemSensorManager don't block on the absent HIDL nanohub HAL */
    if (access("/tools/sensors_hal", F_OK) == 0) {
        const char *sns_dst = "/mnt/system_a/system/bin/android.hardware.sensors-service.sailfish";
        const char *sns_label = "u:object_r:hal_sensors_default_exec:s0";
        umount_all(sns_dst);
        unlink(sns_dst);
        if (copy_file("/tools/sensors_hal", sns_dst)) {
            chmod(sns_dst, 0755);
            set_selinux_label(sns_dst, sns_label);
            usb_write("[+] Automated: Installed /system/bin/android.hardware.sensors-service.sailfish\n");
        } else if (copy_file("/tools/sensors_hal", "/tmp/sensors_hal")) {
            chmod("/tmp/sensors_hal", 0755);
            set_selinux_label("/tmp/sensors_hal", sns_label);
            int sfd = open(sns_dst, O_WRONLY | O_CREAT | O_EXCL, 0755);
            if (sfd >= 0) close(sfd);
            if (bind_mount_clean("/tmp/sensors_hal", sns_dst) == 0) {
                set_selinux_label(sns_dst, sns_label);
                usb_write("[+] Automated: Bind-mounted /system/bin/android.hardware.sensors-service.sailfish\n");
            }
        }
    }

    /* Automated: Inject VINTF manifest fragments (graphics HALs + AIDL Power/Sensors HALs) */
    if (access("/tools/android.hardware.graphics.xml", F_OK) == 0 ||
        access("/tools/android.hardware.power.xml", F_OK) == 0 ||
        access("/tools/android.hardware.sensors.xml", F_OK) == 0) {
        const char *vintf_dir = "/mnt/system_a/system/etc/vintf/manifest";
        mkdir("/tmp/vintf_manifest", 0755);
        umount2("/tmp/vintf_manifest", MNT_DETACH);
        if (mount("tmpfs", "/tmp/vintf_manifest", "tmpfs", 0, "mode=0755,uid=0,gid=0") == 0) {
            DIR *vd = opendir(vintf_dir);
            if (vd) {
                struct dirent *de;
                while ((de = readdir(vd)) != NULL) {
                    if (de->d_name[0] == '.') continue;
                    char src_path[256], dst_path[256];
                    snprintf(src_path, sizeof(src_path), "%s/%s", vintf_dir, de->d_name);
                    snprintf(dst_path, sizeof(dst_path), "/tmp/vintf_manifest/%s", de->d_name);
                    copy_file(src_path, dst_path);
                    set_selinux_label(dst_path, "u:object_r:system_file:s0");
                }
                closedir(vd);
            }
            if (access("/tools/android.hardware.graphics.xml", F_OK) == 0) {
                copy_file("/tools/android.hardware.graphics.xml", "/tmp/vintf_manifest/android.hardware.graphics.xml");
                set_selinux_label("/tmp/vintf_manifest/android.hardware.graphics.xml", "u:object_r:system_file:s0");
            }
            if (access("/tools/android.hardware.power.xml", F_OK) == 0) {
                copy_file("/tools/android.hardware.power.xml", "/tmp/vintf_manifest/android.hardware.power.xml");
                set_selinux_label("/tmp/vintf_manifest/android.hardware.power.xml", "u:object_r:system_file:s0");
            }
            if (access("/tools/android.hardware.sensors.xml", F_OK) == 0) {
                copy_file("/tools/android.hardware.sensors.xml", "/tmp/vintf_manifest/android.hardware.sensors.xml");
                set_selinux_label("/tmp/vintf_manifest/android.hardware.sensors.xml", "u:object_r:system_file:s0");
            }
            if (mount("/tmp/vintf_manifest", vintf_dir, NULL, MS_BIND, NULL) == 0) {
                usb_write("[+] Automated: Overlaid /system/etc/vintf/manifest with graphics, power & sensors HAL fragments\n");
            }
        }
    }

    /* Overlay /system/product/overlay with static RRO enabling A17 mainline wpa_supplicant */
    if (access("/tools/SailfishWifiOverlay.apk", F_OK) == 0) {
        const char *ovl_dir = "/mnt/system_a/system/product/overlay";
        umount_all(ovl_dir);
        if (access(ovl_dir, F_OK) == 0) {
            const char *tmp_ovl = "/tmp/product_overlay";
            mkdir("/tmp", 0755);
            mkdir(tmp_ovl, 0755);
            umount_all(tmp_ovl);
            if (mount("tmpfs", tmp_ovl, "tmpfs", 0, "mode=0755,uid=0,gid=0") == 0) {
                copy_dir_labeled(ovl_dir, tmp_ovl, "u:object_r:system_file:s0", 0);
                mkdir("/tmp/product_overlay/SailfishWifiOverlay", 0755);
                chmod("/tmp/product_overlay/SailfishWifiOverlay", 0755);
                set_selinux_label("/tmp/product_overlay/SailfishWifiOverlay",
                                  "u:object_r:system_file:s0");
                const char *dst_apk =
                    "/tmp/product_overlay/SailfishWifiOverlay/SailfishWifiOverlay.apk";
                if (copy_file("/tools/SailfishWifiOverlay.apk", dst_apk)) {
                    chmod(dst_apk, 0644);
                    set_selinux_label(dst_apk, "u:object_r:system_file:s0");
                    if (mount(tmp_ovl, ovl_dir, NULL, MS_BIND, NULL) == 0) {
                        usb_write("[+] Automated: Overlaid /system/product/overlay with SailfishWifiOverlay.apk\n");
                    }
                }
            }
        }
    }

    /* Automated: Overlay /vendor/etc/vintf/manifest.xml with only active HIDL HALs
     * so hwservicemanager never advertises stubbed HALs (wifi, soundtrigger, etc.)
     * that would crash system_server threads via NoSuchElementException. */
    if (access("/mnt/system_a/vendor/etc/vintf/manifest.xml", F_OK) == 0) {
        const char *vendor_manifest_xml =
            "<manifest version=\"1.0\" type=\"device\" target-level=\"2\">\n"
            "    <hal format=\"hidl\">\n"
            "        <name>android.hardware.graphics.allocator</name>\n"
            "        <transport>hwbinder</transport>\n"
            "        <version>2.0</version>\n"
            "        <interface>\n"
            "            <name>IAllocator</name>\n"
            "            <instance>default</instance>\n"
            "        </interface>\n"
            "        <fqname>@2.0::IAllocator/default</fqname>\n"
            "    </hal>\n"
            "    <hal format=\"hidl\">\n"
            "        <name>android.hardware.graphics.composer</name>\n"
            "        <transport>hwbinder</transport>\n"
            "        <version>2.0</version>\n"
            "        <version>2.1</version>\n"
            "        <interface>\n"
            "            <name>IComposer</name>\n"
            "            <instance>default</instance>\n"
            "        </interface>\n"
            "        <fqname>@2.1::IComposer/default</fqname>\n"
            "    </hal>\n"
            "    <hal format=\"hidl\">\n"
            "        <name>android.hardware.graphics.mapper</name>\n"
            "        <transport arch=\"32+64\">passthrough</transport>\n"
            "        <version>2.0</version>\n"
            "        <version>2.1</version>\n"
            "        <interface>\n"
            "            <name>IMapper</name>\n"
            "            <instance>default</instance>\n"
            "        </interface>\n"
            "        <fqname>@2.0::IMapper/default</fqname>\n"
            "        <fqname>@2.1::IMapper/default</fqname>\n"
            "    </hal>\n"
            "    <hal format=\"hidl\">\n"
            "        <name>android.hardware.health</name>\n"
            "        <transport>hwbinder</transport>\n"
            "        <version>2.0</version>\n"
            "        <interface>\n"
            "            <name>IHealth</name>\n"
            "            <instance>default</instance>\n"
            "        </interface>\n"
            "        <fqname>@2.0::IHealth/default</fqname>\n"
            "    </hal>\n"
            "    <hal format=\"hidl\">\n"
            "        <name>android.hardware.power</name>\n"
            "        <transport>hwbinder</transport>\n"
            "        <version>1.1</version>\n"
            "        <interface>\n"
            "            <name>IPower</name>\n"
            "            <instance>default</instance>\n"
            "        </interface>\n"
            "        <fqname>@1.1::IPower/default</fqname>\n"
            "    </hal>\n"
            "    <hal format=\"hidl\">\n"
            "        <name>android.renderscript</name>\n"
            "        <transport arch=\"32+64\">passthrough</transport>\n"
            "        <version>1.0</version>\n"
            "        <interface>\n"
            "            <name>IDevice</name>\n"
            "            <instance>default</instance>\n"
            "        </interface>\n"
            "        <fqname>@1.0::IDevice/default</fqname>\n"
            "    </hal>\n"
            "    <sepolicy>\n"
            "        <version>29.0</version>\n"
            "    </sepolicy>\n"
            "</manifest>\n";
        mkdir("/tmp", 0755);
        write_file_str("/tmp/vendor_manifest.xml", vendor_manifest_xml);
        chmod("/tmp/vendor_manifest.xml", 0644);
        set_selinux_label("/tmp/vendor_manifest.xml", "u:object_r:vendor_configs_file:s0");
        if (bind_mount_clean("/tmp/vendor_manifest.xml", "/mnt/system_a/vendor/etc/vintf/manifest.xml") == 0) {
            usb_write("[+] Automated: Overlaid /vendor/etc/vintf/manifest.xml (active HIDL HALs only)\n");
        }
    }
    if (access("/mnt/system_a/vendor/etc/vintf/manifest", F_OK) == 0) {
        mkdir("/tmp", 0755);
        mkdir("/tmp/empty_vendor_vintf", 0755);
        umount_all("/tmp/empty_vendor_vintf");
        if (mount("tmpfs", "/tmp/empty_vendor_vintf", "tmpfs", 0, "mode=0755,uid=0,gid=0") == 0) {
            set_selinux_label("/tmp/empty_vendor_vintf", "u:object_r:vendor_configs_file:s0");
            if (bind_mount_clean("/tmp/empty_vendor_vintf", "/mnt/system_a/vendor/etc/vintf/manifest") == 0) {
                usb_write("[+] Automated: Overlaid empty /vendor/etc/vintf/manifest\n");
            }
        }
    }

    /* Automated: Clean any stale linker configs left on persistent flash */
    unlink("/mnt/system_a/system/etc/ld.config.arm64.txt");
    unlink("/mnt/system_a/system/etc/ld.config.arm.txt");
    unlink("/mnt/system_a/system/etc/ld.config.txt");
    unlink("/mnt/system_a/system/etc/ld.config.vndk_lite.txt");
    unlink("/mnt/system_a/linkerconfig/ld.config.txt");
    unlink("/mnt/system_a/linkerconfig/ld.config.arm64.txt");

    /* Automated: Clean 0-byte placeholder files in /system/lib64 and /system/lib (NEVER unlink symlinks) */
    clean_zero_byte_libs("/mnt/system_a/system/lib64");
    clean_zero_byte_libs("/mnt/system_a/system/lib");

    /* Automated: Overlay ueventd.rc so /dev/ion, /dev/dma_heap/, and /dev/dri/ stay accessible */
    overlay_ueventd_file("/mnt/system_a/system/etc/ueventd.rc", "/tmp/ueventd.rc", "u:object_r:system_file:s0");
    overlay_ueventd_file("/mnt/system_a/vendor/ueventd.rc", "/tmp/vendor_ueventd.rc", "u:object_r:vendor_configs_file:s0");

    /* Automated: Restore critical Bionic C library symlinks in /system/lib64 and /system/lib */
    const char *bionic_libs[] = {
        "libc.so", "libm.so", "libdl.so", "libdl_android.so", NULL
    };
    for (int b = 0; bionic_libs[b]; b++) {
        /* 64-bit in /system/lib64 */
        char target64[256], rel64[128];
        snprintf(target64, sizeof(target64), "/mnt/system_a/system/lib64/%s", bionic_libs[b]);
        snprintf(rel64, sizeof(rel64), "bootstrap/%s", bionic_libs[b]);
        struct stat st64;
        if (stat(target64, &st64) != 0 || (S_ISREG(st64.st_mode) && st64.st_size == 0)) {
            unlink(target64);
            symlink(rel64, target64);
            chmod(target64, 0755);
            set_selinux_label(target64, "u:object_r:system_lib_file:s0");
            char bmsg[128];
            snprintf(bmsg, sizeof(bmsg), "[+] Restored 64-bit Bionic: %s -> %s\n", target64, rel64);
            usb_write(bmsg);
        }

        /* 32-bit in /system/lib */
        char target32[256], rel32[128];
        snprintf(target32, sizeof(target32), "/mnt/system_a/system/lib/%s", bionic_libs[b]);
        snprintf(rel32, sizeof(rel32), "bootstrap/%s", bionic_libs[b]);
        struct stat st32;
        if (stat(target32, &st32) != 0 || (S_ISREG(st32.st_mode) && st32.st_size == 0)) {
            unlink(target32);
            symlink(rel32, target32);
            chmod(target32, 0755);
            set_selinux_label(target32, "u:object_r:system_lib_file:s0");
        }
    }

    /* Ensure dynamic linkers exist */
    struct stat st_lnk;
    if (stat("/mnt/system_a/system/bin/linker64", &st_lnk) != 0) {
        unlink("/mnt/system_a/system/bin/linker64");
        symlink("bootstrap/linker64", "/mnt/system_a/system/bin/linker64");
        chmod("/mnt/system_a/system/bin/linker64", 0755);
        set_selinux_label("/mnt/system_a/system/bin/linker64", "u:object_r:system_file:s0");
    }
    if (stat("/mnt/system_a/system/bin/linker", &st_lnk) != 0) {
        unlink("/mnt/system_a/system/bin/linker");
        symlink("bootstrap/linker", "/mnt/system_a/system/bin/linker");
        chmod("/mnt/system_a/system/bin/linker", 0755);
        set_selinux_label("/mnt/system_a/system/bin/linker", "u:object_r:system_file:s0");
    }

    /* Mount tmpfs on system_a/apex, bootstrap-apex, and linkerconfig */
    mkdir("/mnt/system_a/apex", 0755);
    umount2("/mnt/system_a/apex", MNT_DETACH);
    mount("tmpfs", "/mnt/system_a/apex", "tmpfs", 0, "mode=0755,uid=0,gid=0");

    mkdir("/mnt/system_a/bootstrap-apex", 0755);
    umount2("/mnt/system_a/bootstrap-apex", MNT_DETACH);
    mount("tmpfs", "/mnt/system_a/bootstrap-apex", "tmpfs", 0, "mode=0755,uid=0,gid=0");

    mkdir("/mnt/system_a/linkerconfig", 0755);
    umount2("/mnt/system_a/linkerconfig", MNT_DETACH);
    mount("tmpfs", "/mnt/system_a/linkerconfig", "tmpfs", 0, "mode=0755,uid=0,gid=0");

    /* Automated: Populate permissive multi-arch /linkerconfig/ld.config.txt */
    if (write_permissive_ld_config("/mnt/system_a/linkerconfig/ld.config.txt", "u:object_r:linkerconfig_file:s0")) {
        usb_write("[+] Automated: Generated permissive /linkerconfig/ld.config.txt\n");
    }

    /* Automated: Populate com.android.runtime Bionic libraries in /apex and /bootstrap-apex */
    const char *apex_roots[] = { "/mnt/system_a/apex", "/mnt/system_a/bootstrap-apex", NULL };
    for (int a = 0; apex_roots[a]; a++) {
        char ap[256];
        snprintf(ap, sizeof(ap), "%s/com.android.runtime", apex_roots[a]);
        mkdir(ap, 0755);
        set_selinux_label(ap, "u:object_r:system_file:s0");

        snprintf(ap, sizeof(ap), "%s/com.android.runtime/bin", apex_roots[a]);
        mkdir(ap, 0755);
        set_selinux_label(ap, "u:object_r:system_file:s0");

        snprintf(ap, sizeof(ap), "%s/com.android.runtime/bin/linker64", apex_roots[a]);
        symlink("/system/bin/bootstrap/linker64", ap);
        snprintf(ap, sizeof(ap), "%s/com.android.runtime/bin/linker", apex_roots[a]);
        symlink("/system/bin/bootstrap/linker", ap);

        /* 64-bit */
        snprintf(ap, sizeof(ap), "%s/com.android.runtime/lib64", apex_roots[a]);
        mkdir(ap, 0755);
        set_selinux_label(ap, "u:object_r:system_file:s0");
        snprintf(ap, sizeof(ap), "%s/com.android.runtime/lib64/bionic", apex_roots[a]);
        mkdir(ap, 0755);
        set_selinux_label(ap, "u:object_r:system_file:s0");

        const char *bionic_rt[] = { "libc.so", "libm.so", "libdl.so", "libdl_android.so", NULL };
        for (int b = 0; bionic_rt[b]; b++) {
            snprintf(ap, sizeof(ap), "%s/com.android.runtime/lib64/bionic/%s", apex_roots[a], bionic_rt[b]);
            char src[128];
            snprintf(src, sizeof(src), "/system/lib64/bootstrap/%s", bionic_rt[b]);
            symlink(src, ap);

            snprintf(ap, sizeof(ap), "%s/com.android.runtime/lib64/%s", apex_roots[a], bionic_rt[b]);
            symlink(src, ap);
        }

        /* 32-bit */
        snprintf(ap, sizeof(ap), "%s/com.android.runtime/lib", apex_roots[a]);
        mkdir(ap, 0755);
        set_selinux_label(ap, "u:object_r:system_file:s0");
        snprintf(ap, sizeof(ap), "%s/com.android.runtime/lib/bionic", apex_roots[a]);
        mkdir(ap, 0755);
        set_selinux_label(ap, "u:object_r:system_file:s0");

        for (int b = 0; bionic_rt[b]; b++) {
            snprintf(ap, sizeof(ap), "%s/com.android.runtime/lib/bionic/%s", apex_roots[a], bionic_rt[b]);
            char src[128];
            snprintf(src, sizeof(src), "/system/lib/bootstrap/%s", bionic_rt[b]);
            symlink(src, ap);

            snprintf(ap, sizeof(ap), "%s/com.android.runtime/lib/%s", apex_roots[a], bionic_rt[b]);
            symlink(src, ap);
        }
    }

    /* Automated: Populate Qualcomm MDSS sysfs mock before binding /sys */
    setup_mdss_sysfs_mock();

    /* Pre-create FunctionFS ADB mount & gadget function dir inside shared devtmpfs before
     * MS_REC bind into /mnt/system_a/dev so adbd can always open /dev/usb-ffs/adb/ep0. */
    mkdir("/sys/kernel/config/usb_gadget/g1/functions/ffs.adb", 0770);
    chown("/sys/kernel/config/usb_gadget/g1/functions/ffs.adb", 2000, 2000);
    umount_all("/dev/usb-ffs/adb");
    mkdir("/dev/usb-ffs", 0770);
    chown("/dev/usb-ffs", 2000, 2000);
    set_selinux_label("/dev/usb-ffs", "u:object_r:functionfs:s0");
    mkdir("/dev/usb-ffs/adb", 0770);
    chown("/dev/usb-ffs/adb", 2000, 2000);
    if (mount("adb", "/dev/usb-ffs/adb", "functionfs", 0, "rmode=0770,fmode=0660,uid=2000,gid=2000") == 0) {
        set_selinux_label("/dev/usb-ffs/adb", "u:object_r:functionfs:s0");
        usb_write("[+] Automated: Mounted FunctionFS /dev/usb-ffs/adb\n");
    }

    /* Bind /dev, /proc, /sys into system_a */
    mkdir("/mnt/system_a/dev", 0755);
    mount("/dev", "/mnt/system_a/dev", NULL, MS_BIND | MS_REC, NULL);

    mkdir("/mnt/system_a/proc", 0755);
    mount("/proc", "/mnt/system_a/proc", NULL, MS_BIND | MS_REC, NULL);

    mkdir("/mnt/system_a/sys", 0755);
    /* Pre-enable BPF JIT and clear unprivileged restriction before netbpfload starts */
    write_proc_sysctl("/proc/sys/kernel/unprivileged_bpf_disabled", "0\n");
    write_proc_sysctl("/proc/sys/net/core/bpf_jit_enable", "1\n");
    write_proc_sysctl("/proc/sys/net/core/bpf_jit_kallsyms", "1\n");
    /* Silence synchronous UART printk bottleneck once early init diagnostics finish */
    write_proc_sysctl("/sys/module/printk/parameters/ignore_loglevel", "N\n");
    write_proc_sysctl("/proc/sys/kernel/printk", "1 1 1 1\n");

    umount_all("/mnt/system_a/sys/fs/selinux");
    umount_all("/mnt/system_a/sys");
    mount("/sys", "/mnt/system_a/sys", NULL, MS_BIND | MS_REC, NULL);

    mkdir("/mnt/system_a/sys/fs/selinux", 0755);
    mount("none", "/mnt/system_a/sys/fs/selinux", "selinuxfs", 0, NULL);

    /* Mount configfs for Android USB gadget at /config */
    umount_all("/mnt/system_a/config");
    mkdir("/mnt/system_a/config", 0755);
    mount("none", "/mnt/system_a/config", "configfs", 0, NULL);

    /* Label critical /dev nodes with Android SELinux contexts */
    const char *dev_dirs[] = { "/dev", "/mnt/system_a/dev", NULL };
    for (int i = 0; dev_dirs[i]; i++) {
        char p[256];
        snprintf(p, sizeof(p), "%s/kmsg", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:kmsg_device:s0");
        snprintf(p, sizeof(p), "%s/null", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:null_device:s0");
        snprintf(p, sizeof(p), "%s/zero", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:zero_device:s0");
        snprintf(p, sizeof(p), "%s/console", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:console_device:s0");
        snprintf(p, sizeof(p), "%s/ptmx", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:ptmx_device:s0");
        snprintf(p, sizeof(p), "%s/tty", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:tty_device:s0");
        snprintf(p, sizeof(p), "%s/random", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:random_device:s0");
        snprintf(p, sizeof(p), "%s/urandom", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:urandom_device:s0");
        snprintf(p, sizeof(p), "%s/binder", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:binder_device:s0");
        snprintf(p, sizeof(p), "%s/hwbinder", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:hwbinder_device:s0");
        snprintf(p, sizeof(p), "%s/vndbinder", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:vndbinder_device:s0");
        snprintf(p, sizeof(p), "%s/binderfs/binder", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:binder_device:s0");
        snprintf(p, sizeof(p), "%s/binderfs/hwbinder", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:hwbinder_device:s0");
        snprintf(p, sizeof(p), "%s/binderfs/vndbinder", dev_dirs[i]);
        set_selinux_label(p, "u:object_r:vndbinder_device:s0");
    }
    set_selinux_label("/mnt/system_a/system/bin/init", "u:object_r:init_exec:s0");
    set_selinux_label("/mnt/system_a/init", "u:object_r:init_exec:s0");

    /* /data is already mounted (persistent /dev/sda35 or tmpfs) by mount_userdata_partition(). */

    /* Write User 0 & FRP ensure script into writable /data */
    unlink("/mnt/system_a/data/sailfish_ensure_user0.sh");
    write_file_str("/mnt/system_a/data/sailfish_ensure_user0.sh", ensure_user0_sh);
    chmod("/mnt/system_a/data/sailfish_ensure_user0.sh", 0755);
    set_selinux_label("/mnt/system_a/data/sailfish_ensure_user0.sh", "u:object_r:system_file:s0");

    /* Automated: Install qservice_daemon and setup_composer_sysfs.sh into /data */
    install_data_file("/tools/qservice_daemon", "/mnt/system_a/data/qservice_daemon",
                      0755, "u:object_r:system_file:s0",
                      "[+] Automated: Installed /data/qservice_daemon\n");
    install_data_file("/tools/setup_composer_sysfs.sh", "/mnt/system_a/data/setup_composer_sysfs.sh",
                      0755, "u:object_r:system_file:s0",
                      "[+] Automated: Installed /data/setup_composer_sysfs.sh\n");
    unlink("/mnt/system_a/data/start_qservice.sh");
    write_file_str("/mnt/system_a/data/start_qservice.sh",
                   "#!/system/bin/sh\n"
                   "if ! pgrep -x qservice_daemon >/dev/null 2>&1 && [ -x /data/qservice_daemon ]; then\n"
                   "    /data/qservice_daemon >/dev/null 2>&1 &\n"
                   "    sleep 1\n"
                   "fi\n");
    chmod("/mnt/system_a/data/start_qservice.sh", 0755);
    set_selinux_label("/mnt/system_a/data/start_qservice.sh", "u:object_r:system_file:s0");

    /* Automated: Install libsdm_shim.so into /system/lib64, /vendor/lib64, and /data */
    if (access("/tools/libsdm_shim.so", F_OK) == 0) {
        mkdir("/mnt/system_a/system/lib64", 0755);
        mkdir("/mnt/system_a/vendor/lib64", 0755);

        install_data_file("/tools/libsdm_shim.so", "/mnt/system_a/data/libsdm_shim.so",
                          0755, "u:object_r:system_file:s0",
                          "[+] Automated: Installed /data/libsdm_shim.so\n");

        const char *sys_shim = "/mnt/system_a/system/lib64/libsdm_shim.so";
        struct stat st_shim;
        umount_all(sys_shim);
        if (lstat(sys_shim, &st_shim) != 0 || st_shim.st_size < 1000) {
            unlink(sys_shim);
            if (!copy_file("/tools/libsdm_shim.so", sys_shim)) {
                int sfd = open(sys_shim, O_WRONLY | O_CREAT | O_EXCL, 0755);
                if (sfd >= 0) close(sfd);
                bind_mount_clean("/tools/libsdm_shim.so", sys_shim);
            }
        }
        chmod(sys_shim, 0755);
        set_selinux_label(sys_shim, "u:object_r:system_file:s0");
        usb_write("[+] Automated: Verified /system/lib64/libsdm_shim.so\n");

        const char *ven_shim = "/mnt/system_a/vendor/lib64/libsdm_shim.so";
        umount_all(ven_shim);
        if (access(ven_shim, F_OK) != 0) {
            int vfd = open(ven_shim, O_WRONLY | O_CREAT | O_EXCL, 0755);
            if (vfd >= 0) close(vfd);
        }
        bind_mount_clean("/tools/libsdm_shim.so", ven_shim);
        chmod(ven_shim, 0755);
        set_selinux_label(ven_shim, "u:object_r:vendor_file:s0");
    }

    /* Automated: Install patched libui.so via writable /tmp to bypass Android 17 Gralloc 4/5 restriction */
    const char *orig_libui = "/mnt/system_a/system/lib64/libui.so";
    const char *tmp_libui = "/tmp/libui.so";
    mkdir("/tmp", 0755);
    bool libui_ready = false;
    if (access("/tools/libui.so", F_OK) == 0 && copy_file("/tools/libui.so", tmp_libui)) {
        chmod(tmp_libui, 0755);
        set_selinux_label(tmp_libui, "u:object_r:system_lib_file:s0");
        libui_ready = true;
    }
    if (libui_ready && access(tmp_libui, F_OK) == 0 && access(orig_libui, F_OK) == 0) {
        if (mount(tmp_libui, orig_libui, NULL, MS_BIND, NULL) == 0) {
            usb_write("[+] Automated: Successfully bind-mounted Gralloc2-enabled libui.so over /system/lib64/libui.so\n");
        }
    }

    /* Automated: Short-circuit AudioSystem/mediautils service wait stubs so StartAudioService
     * returns in 0ms instead of starving system_server for 75s+ Watchdog timeouts, and skip
     * synchronous binderDied() in registerSoundTriggerCaptureStateListener (0x5f2e0 -> 0x5f4d8)
     * so ExternalCaptureStateTracker does not spin-loop at 85%+ CPU across system_server/logd. */
    static const unsigned char audiobase_ret_null[12] = {
        0x5f, 0x24, 0x03, 0xd5, /* bti c */
        0x1f, 0x01, 0x00, 0xf9, /* str xzr, [x8] */
        0xc0, 0x03, 0x5f, 0xd6  /* ret */
    };
    static const unsigned char audiobase_skip_capture_die[4] = {
        0x7e, 0x00, 0x00, 0x14  /* b 0x5f4d8 (return 0 without binderDied) */
    };
    const struct bin_patch audiobase_patches[] = {
        { 0x3c7a0, audiobase_ret_null, sizeof(audiobase_ret_null) },
        { 0x3c7ac, audiobase_ret_null, sizeof(audiobase_ret_null) },
        { 0x42da0, audiobase_ret_null, sizeof(audiobase_ret_null) },
        { 0x42dac, audiobase_ret_null, sizeof(audiobase_ret_null) },
        { 0x5f2e0, audiobase_skip_capture_die, sizeof(audiobase_skip_capture_die) },
        { 0x63f04, audiobase_ret_null, sizeof(audiobase_ret_null) },
        { 0x6afa0, audiobase_ret_null, sizeof(audiobase_ret_null) },
    };
    patch_and_bind_lib(
        "/mnt/system_a/system/lib64/libaudiobase.so", "/tmp/libaudiobase.so",
        "u:object_r:system_lib_file:s0",
        audiobase_patches, sizeof(audiobase_patches) / sizeof(audiobase_patches[0]),
        "[+] Automated: Patched & bind-mounted /system/lib64/libaudiobase.so\n");

    /* Automated: Force retry=false in libhidlbase getRawServiceInternal so absent HALs
     * never hang gatekeeperd or SoundTriggerMiddlewareService forever. */
    static const unsigned char hidlbase_no_retry[4] = { 0xf9, 0x03, 0x1f, 0x2a }; /* mov w25, wzr */
    const struct bin_patch hidlbase_patches[] = {
        { 0x48b14, hidlbase_no_retry, sizeof(hidlbase_no_retry) },
    };
    patch_and_bind_lib(
        "/mnt/system_a/system/lib64/libhidlbase.so", "/tmp/libhidlbase.so",
        "u:object_r:system_lib_file:s0",
        hidlbase_patches, sizeof(hidlbase_patches) / sizeof(hidlbase_patches[0]),
        "[+] Automated: Patched & bind-mounted /system/lib64/libhidlbase.so\n");

    /* Automated: Make libselinux selinux_check_access() return 0 so keystore2 does not fail
     * with EINVAL when Android 10 precompiled_sepolicy lacks keystore2_key/locksettings_key. */
    static const unsigned char selinux_allow_all[12] = {
        0x5f, 0x24, 0x03, 0xd5, /* bti c */
        0x00, 0x00, 0x80, 0x52, /* mov w0, #0 */
        0xc0, 0x03, 0x5f, 0xd6  /* ret */
    };
    const struct bin_patch selinux_patches[] = {
        { 0x35e28, selinux_allow_all, sizeof(selinux_allow_all) },
    };
    patch_and_bind_lib(
        "/mnt/system_a/system/lib64/libselinux.so", "/tmp/libselinux.so",
        "u:object_r:system_lib_file:s0",
        selinux_patches, sizeof(selinux_patches) / sizeof(selinux_patches[0]),
        "[+] Automated: Patched & bind-mounted /system/lib64/libselinux.so\n");
    install_data_file("/tools/libEGL_adreno_shim.so", "/mnt/system_a/data/libEGL_adreno_shim.so",
                      0755, "u:object_r:system_file:s0",
                      "[+] Automated: Installed /data/libEGL_adreno_shim.so\n");
    install_data_file("/tools/libadreno_utils.so", "/mnt/system_a/data/libadreno_utils.so",
                      0644, "u:object_r:same_process_hal_file:s0",
                      "[+] Automated: Installed /data/libadreno_utils.so\n");

    /* Automated: Save permissive ld.config.txt copy on /data so init/supervisor can restore it */
    unlink("/mnt/system_a/data/ld.config.sailfish.txt");
    if (write_permissive_ld_config("/mnt/system_a/data/ld.config.sailfish.txt", "u:object_r:system_file:s0")) {
        usb_write("[+] Automated: Installed /data/ld.config.sailfish.txt backup\n");
    }

    /* Automated: Write safe composite USB ADB helper script (leaves acm.gs0 active if ffs fails) */
    const char *setup_usb_adb_sh =
        "#!/system/bin/sh\n"
        "ACTION=\"$1\"\n"
        "UDC=\"$2\"\n"
        "[ -z \"$UDC\" ] && UDC=\"$(ls /sys/class/udc 2>/dev/null | head -n 1)\"\n"
        "[ -z \"$UDC\" ] && exit 0\n"
        "G1=/config/usb_gadget/g1\n"
        "[ -d \"$G1\" ] || exit 0\n"
        "CUR_UDC=\"$(cat $G1/UDC 2>/dev/null)\"\n"
        "if [ \"$ACTION\" = \"unbind\" ]; then\n"
        "    [ -e \"$G1/configs/c.1/ffs.adb\" ] || exit 0\n"
        "    echo \"none\" > \"$G1/UDC\" 2>/dev/null || echo \"\" > \"$G1/UDC\" 2>/dev/null || true\n"
        "    rm -f \"$G1/configs/c.1/ffs.adb\"\n"
        "    echo 0x18d1 > \"$G1/idVendor\" 2>/dev/null || true\n"
        "    echo 0xd001 > \"$G1/idProduct\" 2>/dev/null || true\n"
        "    echo \"CDC-ACM\" > \"$G1/configs/c.1/strings/0x409/configuration\" 2>/dev/null || true\n"
        "    [ -e \"$G1/configs/c.1/acm.gs0\" ] || ln -s \"$G1/functions/acm.gs0\" \"$G1/configs/c.1/acm.gs0\" 2>/dev/null || true\n"
        "    echo \"$UDC\" > \"$G1/UDC\" 2>/dev/null || true\n"
        "    exit 0\n"
        "fi\n"
        "if [ ! -e /dev/usb-ffs/adb/ep0 ]; then\n"
        "    mkdir -p /dev/usb-ffs/adb 2>/dev/null || true\n"
        "    mount -t functionfs adb /dev/usb-ffs/adb -o rmode=0770,fmode=0660,uid=2000,gid=2000 2>/dev/null || true\n"
        "fi\n"
        "mkdir -p \"$G1/functions/ffs.adb\" 2>/dev/null || true\n"
        "if [ -e \"$G1/configs/c.1/ffs.adb\" ] && [ -e \"$G1/configs/c.1/acm.gs0\" ] && [ \"$CUR_UDC\" = \"$UDC\" ]; then\n"
        "    exit 0\n"
        "fi\n"
        "echo \"none\" > \"$G1/UDC\" 2>/dev/null || echo \"\" > \"$G1/UDC\" 2>/dev/null || true\n"
        "echo 0x18d1 > \"$G1/idVendor\" 2>/dev/null || true\n"
        "echo 0x4ee2 > \"$G1/idProduct\" 2>/dev/null || true\n"
        "echo 0x00 > \"$G1/bDeviceClass\" 2>/dev/null || true\n"
        "echo 0x00 > \"$G1/bDeviceSubClass\" 2>/dev/null || true\n"
        "echo 0x00 > \"$G1/bDeviceProtocol\" 2>/dev/null || true\n"
        "echo \"ACM-ADB\" > \"$G1/configs/c.1/strings/0x409/configuration\" 2>/dev/null || true\n"
        "[ -e \"$G1/configs/c.1/acm.gs0\" ] || ln -s \"$G1/functions/acm.gs0\" \"$G1/configs/c.1/acm.gs0\" 2>/dev/null || true\n"
        "rm -f \"$G1/configs/c.1/ffs.adb\" 2>/dev/null || true\n"
        "ln -s \"$G1/functions/ffs.adb\" \"$G1/configs/c.1/ffs.adb\" 2>/dev/null || true\n"
        "if ! echo \"$UDC\" > \"$G1/UDC\" 2>/dev/null; then\n"
        "    rm -f \"$G1/configs/c.1/ffs.adb\"\n"
        "    echo 0xd001 > \"$G1/idProduct\" 2>/dev/null || true\n"
        "    echo \"CDC-ACM\" > \"$G1/configs/c.1/strings/0x409/configuration\" 2>/dev/null || true\n"
        "    echo \"$UDC\" > \"$G1/UDC\" 2>/dev/null || true\n"
        "fi\n";
    unlink("/mnt/system_a/data/setup_usb_adb.sh");
    write_file_str("/mnt/system_a/data/setup_usb_adb.sh", setup_usb_adb_sh);
    chmod("/mnt/system_a/data/setup_usb_adb.sh", 0755);
    set_selinux_label("/mnt/system_a/data/setup_usb_adb.sh", "u:object_r:system_file:s0");
    usb_write("[+] Automated: Installed /data/setup_usb_adb.sh\n");

    /* Automated: Generate /data/start_graphics.sh environment helper (init manages HAL/SF/bootanim via wrap_preload) */
    unlink("/mnt/system_a/data/start_graphics.sh");
    FILE *gf = fopen("/mnt/system_a/data/start_graphics.sh", "w");
    if (gf) {
        fprintf(gf, "#!/system/bin/sh\n"
                    "unset LD_LIBRARY_PATH\n"
                    "setenforce 0 2>/dev/null\n"
                    "chmod 666 /dev/ion 2>/dev/null\n"
                    "chmod 666 /dev/dma_heap/* 2>/dev/null\n"
                    "mkdir -p /dev/graphics 2>/dev/null\n"
                    "[ ! -e /dev/graphics/fb0 ] && [ -e /dev/fb0 ] && ln -sf /dev/fb0 /dev/graphics/fb0\n"
                    "mkdir -p /dev/dri 2>/dev/null\n"
                    "[ ! -e /dev/dri/card0 ] && [ -e /dev/dri/card1 ] && ln -sf /dev/dri/card1 /dev/dri/card0\n"
                    "chmod 666 /dev/dri/* 2>/dev/null\n"
                    "touch /sys/devices/virtual/graphics/fb0/vsync_event 2>/dev/null\n"
                    "chmod 666 /sys/devices/virtual/graphics/fb0/vsync_event 2>/dev/null\n"
                    "echo 0 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null || true\n"
                    "echo 96 > /sys/class/backlight/994000.dsi.0/brightness 2>/dev/null || true\n"
                    "if [ -b /dev/block/zram0 ] && ! grep -q zram0 /proc/swaps 2>/dev/null; then\n"
                    "    echo lz4 > /sys/block/zram0/comp_algorithm 2>/dev/null || true\n"
                    "    echo 2147483648 > /sys/block/zram0/disksize 2>/dev/null || true\n"
                    "    /system/bin/mkswap /dev/block/zram0 2>/dev/null || true\n"
                    "    /system/bin/swapon -p 32758 /dev/block/zram0 2>/dev/null || true\n"
                    "    echo 0 > /proc/sys/vm/page-cluster 2>/dev/null || true\n"
                    "    echo 100 > /proc/sys/vm/swappiness 2>/dev/null || true\n"
                    "fi\n"
                    "if [ -d /apex/com.android.nfcservices/priv-app ]; then\n"
                    "    mount -t tmpfs tmpfs /apex/com.android.nfcservices/priv-app 2>/dev/null || true\n"
                    "fi\n"
                    "if [ -f /data/libadreno_utils.so ]; then\n"
                    "    mount -o bind /data/libadreno_utils.so /vendor/lib64/libadreno_utils.so 2>/dev/null || true\n"
                    "fi\n"
                    "# Keep kernel schedutil CPUfreq governor active\n"
                    "if ! losetup /dev/block/loop95 >/dev/null 2>&1; then\n"
                    "    dd if=/dev/zero of=/data/frp.img bs=1024 count=512 2>/dev/null || true\n"
                    "    losetup /dev/block/loop95 /data/frp.img 2>/dev/null || true\n"
                    "fi\n"
                    "chown system:system /dev/block/loop95 2>/dev/null || true\n"
                    "chmod 0660 /dev/block/loop95 2>/dev/null || true\n"
                    "chcon u:object_r:frp_block_device:s0 /dev/block/loop95 2>/dev/null || true\n"
                    "ln -sf /dev/block/loop95 /dev/block/platform/soc/624000.ufshc/by-name/frp 2>/dev/null || true\n"
                    "ln -sf /dev/block/loop95 /dev/block/bootdevice/by-name/frp 2>/dev/null || true\n"
                    "ln -sf /dev/block/loop95 /dev/block/by-name/frp 2>/dev/null || true\n"
                    "(\n"
                    "    hwc_restarted=0\n"
                    "    bootanim_stopped=0\n"
                    "    qservice_up=0\n"
                    "    nfc_hidden=0\n"
                    "    fbcon_unbound=0\n"
                    "    if ! pgrep -f android.hardware.power-service.sailfish >/dev/null 2>&1; then\n"
                    "        /system/bin/android.hardware.power-service.sailfish >/dev/null 2>&1 &\n"
                    "    fi\n"
                    "    if ! pgrep -f android.hardware.sensors-service.sailfish >/dev/null 2>&1; then\n"
                    "        /system/bin/android.hardware.sensors-service.sailfish >/dev/null 2>&1 &\n"
                    "    fi\n"
                    "    while true; do\n"
                    "        if ! pgrep -f android.hardware.power-service.sailfish >/dev/null 2>&1; then\n"
                    "            /system/bin/android.hardware.power-service.sailfish >/dev/null 2>&1 &\n"
                    "        fi\n"
                    "        if ! pgrep -f android.hardware.sensors-service.sailfish >/dev/null 2>&1; then\n"
                    "            /system/bin/android.hardware.sensors-service.sailfish >/dev/null 2>&1 &\n"
                    "        fi\n"
                    "        if [ \"$nfc_hidden\" = \"0\" ] && [ -d /apex/com.android.nfcservices/priv-app ]; then\n"
                    "            if mount -t tmpfs tmpfs /apex/com.android.nfcservices/priv-app 2>/dev/null; then\n"
                    "                nfc_hidden=1\n"
                    "            fi\n"
                    "        fi\n"
                    "        boot_done=\"$(getprop sys.boot_completed)\"\n"
                    "        if [ \"$boot_done\" != \"1\" ]; then\n"
                    "            chown system:system /dev/block/loop95 2>/dev/null || true\n"
                    "            chmod 0660 /dev/block/loop95 2>/dev/null || true\n"
                    "            chcon u:object_r:frp_block_device:s0 /dev/block/loop95 2>/dev/null || true\n"
                    "            ln -sf /dev/block/loop95 /dev/block/platform/soc/624000.ufshc/by-name/frp 2>/dev/null || true\n"
                    "            ln -sf /dev/block/loop95 /dev/block/bootdevice/by-name/frp 2>/dev/null || true\n"
                    "            ln -sf /dev/block/loop95 /dev/block/by-name/frp 2>/dev/null || true\n"
                    "        elif [ ! -f /data/.sailfish_provisioned ]; then\n"
                    "            if /system/bin/cmd settings put global device_provisioned 1 2>/dev/null && \\\n"
                    "               /system/bin/cmd settings put secure user_setup_complete 1 2>/dev/null; then\n"
                    "                /system/bin/cmd settings put global stay_on_while_plugged_in 0 2>/dev/null || true\n"
                    "                /system/bin/cmd settings put system screen_off_timeout 600000 2>/dev/null || true\n"
                    "                /system/bin/cmd settings put global disable_window_blurs 1 2>/dev/null || true\n"
                    "                /system/bin/cmd settings put global window_animation_scale 1.0 2>/dev/null || true\n"
                    "                /system/bin/cmd settings put global transition_animation_scale 1.0 2>/dev/null || true\n"
                    "                /system/bin/cmd settings put global animator_duration_scale 1.0 2>/dev/null || true\n"
                    "                /system/bin/cmd lock_settings set-disabled true 2>/dev/null || /system/bin/locksettings set-disabled true 2>/dev/null || true\n"
                    "                /system/bin/cmd package disable-user --user 0 com.android.nfc 2>/dev/null || true\n"
                    "                /system/bin/cmd settings put global adb_enabled 0 2>/dev/null || true\n"
                    "                touch /data/.sailfish_provisioned 2>/dev/null || true\n"
                    "            fi\n"
                    "        fi\n"
                    "        if [ \"$boot_done\" = \"1\" ] && [ ! -f /data/.sailfish_adb_off_v1 ]; then\n"
                    "            if [ \"$(getprop sys.sailfish.adb)\" != \"1\" ]; then\n"
                    "                /system/bin/cmd settings put global adb_enabled 0 2>/dev/null || true\n"
                    "                setprop persist.sys.usb.config none 2>/dev/null || true\n"
                    "                setprop sys.usb.config none 2>/dev/null || true\n"
                    "                stop adbd 2>/dev/null || true\n"
                    "            fi\n"
                    "            touch /data/.sailfish_adb_off_v1 2>/dev/null || true\n"
                    "        fi\n"
                    "        if [ \"$boot_done\" = \"1\" ] && [ ! -f /data/.sailfish_power_tuned_v3 ]; then\n"
                    "            /system/bin/cmd settings put global stay_on_while_plugged_in 0 2>/dev/null || true\n"
                    "            /system/bin/cmd settings put system screen_off_timeout 600000 2>/dev/null || true\n"
                    "            /system/bin/cmd settings put global disable_window_blurs 1 2>/dev/null || true\n"
                    "            /system/bin/cmd settings put global window_animation_scale 1.0 2>/dev/null || true\n"
                    "            /system/bin/cmd settings put global transition_animation_scale 1.0 2>/dev/null || true\n"
                    "            /system/bin/cmd settings put global animator_duration_scale 1.0 2>/dev/null || true\n"
                    "            touch /data/.sailfish_power_tuned_v3 2>/dev/null || true\n"
                    "        fi\n"
                    "        sf_pid=\"$(pidof surfaceflinger.real 2>/dev/null || pidof surfaceflinger 2>/dev/null || pgrep -f '/system/bin/surfaceflinger' 2>/dev/null | head -n 1)\"\n"
                    "        if [ -n \"$sf_pid\" ] && [ \"$fbcon_unbound\" = \"0\" ]; then\n"
                    "            echo 0 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null || true\n"
                    "            for v in /sys/class/vtconsole/vtcon*; do\n"
                    "                if [ -f \"$v/name\" ] && grep -qi \"frame buffer\" \"$v/name\" 2>/dev/null; then\n"
                    "                    echo 0 > \"$v/bind\" 2>/dev/null || true\n"
                    "                fi\n"
                    "            done\n"
                    "            fbcon_unbound=1\n"
                    "        fi\n"
                    "        if [ \"$boot_done\" = \"1\" ]; then\n"
                    "            if [ -n \"$(pidof bootanimation.real 2>/dev/null || pidof bootanimation 2>/dev/null || pgrep -f '/system/bin/bootanimation' 2>/dev/null)\" ]; then\n"
                    "                setprop service.bootanim.exit 1 2>/dev/null || true\n"
                    "                setprop ctl.stop bootanim 2>/dev/null || true\n"
                    "                stop bootanim 2>/dev/null || true\n"
                    "            fi\n"
                    "        fi\n"
                    "        if [ \"$boot_done\" = \"1\" ] && [ \"$bootanim_stopped\" = \"0\" ]; then\n"
                    "            echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null || true\n"
                    "            echo 0 > /sys/class/backlight/994000.dsi.0/bl_power 2>/dev/null || true\n"
                    "            echo 96 > /sys/class/backlight/994000.dsi.0/brightness 2>/dev/null || true\n"
                    "            /system/bin/cmd lock_settings set-disabled true 2>/dev/null || /system/bin/locksettings set-disabled true 2>/dev/null || true\n"
                    "            /system/bin/cmd input keyevent 224 2>/dev/null || true\n"
                    "            /system/bin/cmd window dismiss-keyguard 2>/dev/null || true\n"
                    "            /system/bin/cmd statusbar collapse 2>/dev/null || true\n"
                    "            setprop service.bootanim.exit 1 2>/dev/null || true\n"
                    "            setprop ctl.stop bootanim 2>/dev/null || true\n"
                    "            stop bootanim 2>/dev/null || true\n"
                    "            sleep 2\n"
                    "            if /system/bin/dumpsys window 2>/dev/null | grep -q \"mCurrentFocus=.*Android System\"; then\n"
                    "                /system/bin/cmd input keyevent 66 2>/dev/null || true\n"
                    "            fi\n"
                    "            /system/bin/cmd statusbar collapse 2>/dev/null || true\n"
                    "            bootanim_stopped=1\n"
                    "        fi\n"
                    "        if [ \"$qservice_up\" = \"0\" ]; then\n"
                    "            if pgrep -f vndservicemanager >/dev/null 2>&1 || [ \"$(getprop init.svc.vndservicemanager)\" = \"running\" ]; then\n"
                    "                if pgrep -x qservice_daemon >/dev/null 2>&1; then\n"
                    "                    qservice_up=1\n"
                    "                else\n"
                    "                    hwc_was_up=0\n"
                    "                    if pgrep -f graphics.composer@2.1-service >/dev/null 2>&1 || [ \"$(getprop init.svc.vendor.hwcomposer-2-1)\" = \"running\" ]; then\n"
                    "                        hwc_was_up=1\n"
                    "                    fi\n"
                    "                    /data/qservice_daemon >/dev/null 2>&1 &\n"
                    "                    if [ \"$boot_done\" != \"1\" ] && [ \"$hwc_was_up\" = \"1\" ] && [ \"$hwc_restarted\" = \"0\" ]; then\n"
                    "                        sleep 1\n"
                    "                        setprop ctl.restart vendor.hwcomposer-2-1 2>/dev/null || true\n"
                    "                        if [ -n \"$sf_pid\" ]; then\n"
                    "                            sleep 1\n"
                    "                            setprop ctl.restart surfaceflinger 2>/dev/null || true\n"
                    "                        fi\n"
                    "                        hwc_restarted=1\n"
                    "                    fi\n"
                    "                    qservice_up=1\n"
                    "                fi\n"
                    "            fi\n"
                    "        fi\n"
                    "        if [ \"$boot_done\" = \"1\" ] && [ \"$bootanim_stopped\" = \"1\" ]; then\n"
                    "            sleep 10\n"
                    "        elif [ \"$boot_done\" = \"1\" ]; then\n"
                    "            sleep 3\n"
                    "        else\n"
                    "            sleep 1\n"
                    "        fi\n"
                    "    done\n"
                    ") >/dev/null 2>&1 &\n"
                    "echo '[+] Graphics environment & permissions ready!'\n");
        fclose(gf);
        chmod("/mnt/system_a/data/start_graphics.sh", 0755);
        set_selinux_label("/mnt/system_a/data/start_graphics.sh", "u:object_r:system_file:s0");
        usb_write("[+] Automated: Created /data/start_graphics.sh\n");
    }

    /* NOTE: wrap_app_process64 removed — Android init runs /system/bin/app_process64 directly once ld.config.txt is intact */

    /* Mount in-memory tmpfs for cache and metadata */
    mkdir("/mnt/system_a/cache", 0770);
    mount("tmpfs", "/mnt/system_a/cache", "tmpfs", 0, "mode=0770,uid=1000,gid=2001");
    set_selinux_label("/mnt/system_a/cache", "u:object_r:cache_file:s0");

    /* 0755 like a real ext4 /metadata root: aconfigd-system platform-init runs as
     * user system and must traverse /metadata to populate /metadata/aconfig
     * (with 0700 the storage stayed empty -> AconfigFlags returned null ->
     * AdvancedProtectionService "Invalid feature flag" crash). */
    mkdir("/mnt/system_a/metadata", 0755);
    mount("tmpfs", "/mnt/system_a/metadata", "tmpfs", 0, "mode=0755");
    set_selinux_label("/mnt/system_a/metadata", "u:object_r:metadata_file:s0");

    /* Mount persist (sda3) and modem firmware (sda2) */
    mkdir("/mnt/system_a/mnt", 0755);
    umount2("/mnt/system_a/mnt", MNT_DETACH);
    mount("tmpfs", "/mnt/system_a/mnt", "tmpfs", 0, "mode=0755,uid=0,gid=1000");
    mkdir("/mnt/system_a/mnt/vendor", 0755);
    mkdir("/mnt/system_a/mnt/vendor/persist", 0771);
    mount("/dev/sda3", "/mnt/system_a/mnt/vendor/persist", "ext4", 0, NULL);
    set_selinux_label("/mnt/system_a/mnt/vendor/persist", "u:object_r:persist_file:s0");

    mkdir("/mnt/system_a/vendor/firmware_mnt", 0755);
    mount("/dev/sda2", "/mnt/system_a/vendor/firmware_mnt", "vfat", MS_RDONLY, NULL);
    set_selinux_label("/mnt/system_a/vendor/firmware_mnt", "u:object_r:firmware_file:s0");

    /* Ensure block device symlinks and loop devices exist for Android fstab & apexd */
    mkdir("/dev/block", 0755);
    mkdir("/mnt/system_a/dev/block", 0755);
    mkdir("/mnt/system_a/dev/block/mapper", 0755);
    mkdir("/mnt/system_a/dev/block/by-name", 0755);
    mkdir("/mnt/system_a/dev/block/bootdevice", 0755);
    mkdir("/mnt/system_a/dev/block/bootdevice/by-name", 0755);
    mkdir("/mnt/system_a/dev/block/platform", 0755);
    mkdir("/mnt/system_a/dev/block/platform/soc", 0755);
    mkdir("/mnt/system_a/dev/block/platform/soc/624000.ufshc", 0755);
    mkdir("/mnt/system_a/dev/block/platform/soc/624000.ufshc/by-name", 0755);

    int lcfd = open("/dev/loop-control", O_RDWR);
    for (int l = 0; l < 96; l++) {
        if (lcfd >= 0)
            ioctl(lcfd, 0x4C80, l); /* LOOP_CTL_ADD */
        char lpath[128], blpath[128];
        snprintf(lpath, sizeof(lpath), "/dev/loop%d", l);
        snprintf(blpath, sizeof(blpath), "/dev/block/loop%d", l);
        mknod(lpath, S_IFBLK | 0660, makedev(7, l));
        mknod(blpath, S_IFBLK | 0660, makedev(7, l));
        set_selinux_label(lpath, "u:object_r:loop_device:s0");
        set_selinux_label(blpath, "u:object_r:loop_device:s0");
    }
    if (lcfd >= 0)
        close(lcfd);

    /* Automated: Pre-configure zeroed 512 KB FRP loop block device on /dev/block/loop95
     * so PersistentDataBlockService initializes with Android 17 FRP_SECRET_MAGIC. */
    const char *frp_backing = g_data_persistent ? "/mnt/system_a/data/frp.img" : "/tmp/frp.img";
    if (!g_data_persistent)
        mkdir("/tmp", 0755);
    struct stat frp_st;
    bool need_frp_init = !(g_data_persistent && stat(frp_backing, &frp_st) == 0 && frp_st.st_size == 512 * 1024);
    int frp_flags = need_frp_init ? (O_RDWR | O_CREAT | O_TRUNC) : O_RDWR;
    int frp_ffd = open(frp_backing, frp_flags, 0600);
    if (frp_ffd >= 0) {
        if (!need_frp_init || ftruncate(frp_ffd, 512 * 1024) == 0) {
            int frp_lfd = open("/dev/block/loop95", O_RDWR);
            if (frp_lfd >= 0) {
                ioctl(frp_lfd, 0x4C01, 0); /* LOOP_CLR_FD */
                if (ioctl(frp_lfd, 0x4C00, frp_ffd) == 0) { /* LOOP_SET_FD */
                    usb_write(g_data_persistent ?
                              "[+] Automated: Attached 512 KB /data/frp.img -> /dev/block/loop95\n" :
                              "[+] Automated: Attached zeroed 512 KB /tmp/frp.img -> /dev/block/loop95\n");
                }
                close(frp_lfd);
            }
        }
        close(frp_ffd);
    }
    chown("/dev/block/loop95", 1000, 1000);
    chmod("/dev/block/loop95", 0660);
    set_selinux_label("/dev/block/loop95", "u:object_r:frp_block_device:s0");
    chown("/mnt/system_a/dev/block/loop95", 1000, 1000);
    chmod("/mnt/system_a/dev/block/loop95", 0660);
    set_selinux_label("/mnt/system_a/dev/block/loop95", "u:object_r:frp_block_device:s0");

    /* Ensure /dev/ion device permissions and SELinux label */
    chmod("/dev/ion", 0666);
    chmod("/mnt/system_a/dev/ion", 0666);
    set_selinux_label("/mnt/system_a/dev/ion", "u:object_r:ion_device:s0");

    /* Ensure graphics display nodes and permissions */
    mkdir("/mnt/system_a/dev/graphics", 0755);
    symlink("/dev/fb0", "/mnt/system_a/dev/graphics/fb0");
    mkdir("/dev/graphics", 0755);
    symlink("/dev/fb0", "/dev/graphics/fb0");

    mkdir("/mnt/system_a/dev/dri", 0755);
    mkdir("/dev/dri", 0755);
    if (access("/dev/dri/card1", F_OK) == 0 && access("/dev/dri/card0", F_OK) != 0) {
        symlink("/dev/dri/card1", "/dev/dri/card0");
        symlink("/dev/dri/card1", "/mnt/system_a/dev/dri/card0");
    }
    chmod("/dev/dri/card0", 0666);
    chmod("/dev/dri/card1", 0666);
    chmod("/dev/dri/renderD128", 0666);
    chmod("/mnt/system_a/dev/dri/card0", 0666);
    chmod("/mnt/system_a/dev/dri/card1", 0666);
    chmod("/mnt/system_a/dev/dri/renderD128", 0666);

    write_file_str("/sys/class/backlight/994000.dsi.0/brightness", "200\n");
    int vfd = open("/sys/devices/virtual/graphics/fb0/vsync_event", O_WRONLY | O_CREAT, 0666);
    if (vfd >= 0) close(vfd);
    chmod("/sys/devices/virtual/graphics/fb0/vsync_event", 0666);

    const char *prefixes[] = {
        "/mnt/system_a/dev/block/by-name",
        "/mnt/system_a/dev/block/bootdevice/by-name",
        "/mnt/system_a/dev/block/platform/soc/624000.ufshc/by-name",
    };

    /* Dynamically populate all sda partition symlinks from /sys/block/sda uevent files */
    DIR *sd = opendir("/sys/block/sda");
    if (sd) {
        struct dirent *sde;
        while ((sde = readdir(sd)) != NULL) {
            if (strncmp(sde->d_name, "sda", 3) == 0 && isdigit(sde->d_name[3])) {
                char devnode[64], blknode[128], uev_path[128], uev[256];
                snprintf(devnode, sizeof(devnode), "/dev/%s", sde->d_name);
                snprintf(blknode, sizeof(blknode), "/dev/block/%s", sde->d_name);
                symlink(devnode, blknode);

                snprintf(uev_path, sizeof(uev_path), "/sys/block/sda/%s/uevent", sde->d_name);
                FILE *uf = fopen(uev_path, "r");
                if (uf) {
                    while (fgets(uev, sizeof(uev), uf)) {
                        if (strncmp(uev, "PARTNAME=", 9) == 0) {
                            char *nl = strchr(uev, '\n');
                            if (nl) *nl = '\0';
                            const char *pname = uev + 9;
                            for (int p = 0; p < 3; p++) {
                                char lpath[256];
                                snprintf(lpath, sizeof(lpath), "%s/%s", prefixes[p], pname);
                                symlink(devnode, lpath);
                            }
                        }
                    }
                    fclose(uf);
                }
            }
        }
        closedir(sd);
    }

    for (int p = 0; p < 3; p++) {
        char linkpath[256];
        snprintf(linkpath, sizeof(linkpath), "%s/system", prefixes[p]);
        symlink(sys_dev, linkpath);
        snprintf(linkpath, sizeof(linkpath), "%s/system_a", prefixes[p]);
        symlink(sys_dev, linkpath);
        snprintf(linkpath, sizeof(linkpath), "%s/system_b", prefixes[p]);
        symlink(strcmp(sys_dev, "/dev/sda35") == 0 ? "/dev/sda35" : "/dev/sda34", linkpath);

        snprintf(linkpath, sizeof(linkpath), "%s/vendor", prefixes[p]);
        symlink("/dev/sda31", linkpath);
        snprintf(linkpath, sizeof(linkpath), "%s/vendor_a", prefixes[p]);
        symlink("/dev/sda31", linkpath);
        snprintf(linkpath, sizeof(linkpath), "%s/vendor_b", prefixes[p]);
        symlink("/dev/sda32", linkpath);

        snprintf(linkpath, sizeof(linkpath), "%s/boot", prefixes[p]);
        symlink("/dev/sda27", linkpath);
        snprintf(linkpath, sizeof(linkpath), "%s/boot_a", prefixes[p]);
        symlink("/dev/sda27", linkpath);
        snprintf(linkpath, sizeof(linkpath), "%s/boot_b", prefixes[p]);
        symlink("/dev/sda28", linkpath);

        snprintf(linkpath, sizeof(linkpath), "%s/userdata", prefixes[p]);
        symlink(strcmp(sys_dev, "/dev/sda35") == 0 ? "/dev/sda33" : "/dev/sda35", linkpath);

        snprintf(linkpath, sizeof(linkpath), "%s/persist", prefixes[p]);
        symlink("/dev/sda3", linkpath);

        snprintf(linkpath, sizeof(linkpath), "%s/modem", prefixes[p]);
        symlink("/dev/sda2", linkpath);
        snprintf(linkpath, sizeof(linkpath), "%s/modem_a", prefixes[p]);
        symlink("/dev/sda2", linkpath);

        snprintf(linkpath, sizeof(linkpath), "%s/frp", prefixes[p]);
        unlink(linkpath);
        symlink("/dev/block/loop95", linkpath);
    }

    return true;
}

static pid_t find_android_init_pid(void) {
    DIR *d = opendir("/proc");
    if (!d)
        return -1;
    struct dirent *de;
    pid_t found = -1;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit(de->d_name[0]))
            continue;
        pid_t p = atoi(de->d_name);
        if (p <= 1)
            continue;
        char comm_path[128], comm[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)p);
        if (read_first_line(comm_path, comm, sizeof(comm)) && strcmp(comm, "init") == 0) {
            char test_sh[128];
            snprintf(test_sh, sizeof(test_sh), "/proc/%d/root/system/bin/sh", (int)p);
            if (access(test_sh, F_OK) != 0)
                continue;

            /* Prefer the PID that is PID 1 inside its own PID namespace */
            char status_path[128], line[256];
            snprintf(status_path, sizeof(status_path), "/proc/%d/status", (int)p);
            FILE *sf = fopen(status_path, "r");
            bool is_ns_pid1 = false;
            if (sf) {
                while (fgets(line, sizeof(line), sf)) {
                    if (strncmp(line, "NSpid:", 6) == 0) {
                        size_t len = strlen(line);
                        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == ' ' || line[len - 1] == '\t'))
                            line[--len] = '\0';
                        if (len >= 2 && line[len - 1] == '1' && (line[len - 2] == '\t' || line[len - 2] == ' '))
                            is_ns_pid1 = true;
                        break;
                    }
                }
                fclose(sf);
            }
            found = p;
            if (is_ns_pid1)
                break;
        }
    }
    closedir(d);
    return found;
}

static void dump_logcat_filtered(const char *filter) {
    FILE *f = fopen("/dev/logcat.txt", "r");
    if (!f) {
        usb_write("[-] /dev/logcat.txt not found (run boot_android first)\n");
        return;
    }
    #define LOGCAT_RING 120
    char *ring[LOGCAT_RING];
    memset(ring, 0, sizeof(ring));
    int idx = 0, count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "Waited one second for SurfaceFlingerAIDL"))
            continue;
        if (filter && *filter && !strstr(line, filter))
            continue;
        if (ring[idx])
            free(ring[idx]);
        ring[idx] = strdup(line);
        idx = (idx + 1) % LOGCAT_RING;
        count++;
    }
    fclose(f);

    int start = (count > LOGCAT_RING) ? idx : 0;
    int total = (count > LOGCAT_RING) ? LOGCAT_RING : count;
    for (int i = 0; i < total; i++) {
        int pos = (start + i) % LOGCAT_RING;
        if (ring[pos]) {
            usb_write(ring[pos]);
            free(ring[pos]);
        }
    }
}

static void launch_android_shell(void) {
    pid_t init_pid = find_android_init_pid();
    if (init_pid > 1) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "[+] Joining live Android init (PID %d) PID + mount namespaces (non-destructive)...\n",
                 (int)init_pid);
        usb_write(msg);
    } else {
        usb_write("[+] Preparing Android rootfs & APEX runtime...\n");
        unmount_android_mounts();
        prepare_android_rootfs(true);
    }

    usb_write("[+] Forking interactive Android Bionic root shell (/system/bin/sh)...\n");
    usb_write("[+] Type 'exit' to return to sailfish# bringup shell.\n\n");

    /* Switch ttyGS0 to blocking mode for interactive child shell */
    if (g_ttygs_fd >= 0) {
        int flags = fcntl(g_ttygs_fd, F_GETFL, 0);
        fcntl(g_ttygs_fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    pid_t pid = fork();
    if (pid < 0) {
        char err[128];
        snprintf(err, sizeof(err), "[-] fork failed: %s\n", strerror(errno));
        usb_write(err);
        return;
    }

    if (pid == 0) {
        /* Set new session and acquire ttyGS0 as controlling terminal */
        setsid();
        if (g_ttygs_fd >= 0) {
            ioctl(g_ttygs_fd, TIOCSCTTY, 1);
            dup2(g_ttygs_fd, 0);
            dup2(g_ttygs_fd, 1);
            dup2(g_ttygs_fd, 2);
        }

        bool joined_ns = false;
        if (init_pid > 1) {
            char pid_path[128], mnt_path[128], root_path[128];
            snprintf(pid_path, sizeof(pid_path), "/proc/%d/ns/pid", (int)init_pid);
            snprintf(mnt_path, sizeof(mnt_path), "/proc/%d/ns/mnt", (int)init_pid);
            snprintf(root_path, sizeof(root_path), "/proc/%d/root", (int)init_pid);
            int pid_fd = open(pid_path, O_RDONLY);
            int mnt_fd = open(mnt_path, O_RDONLY);
            int root_fd = open(root_path, O_RDONLY | O_DIRECTORY);
            if (pid_fd >= 0) {
                setns(pid_fd, CLONE_NEWPID);
                close(pid_fd);
            }
            if (mnt_fd >= 0 && root_fd >= 0) {
                if (setns(mnt_fd, CLONE_NEWNS) == 0 &&
                    fchdir(root_fd) == 0 &&
                    chroot(".") == 0 &&
                    chdir("/") == 0) {
                    joined_ns = true;
                }
            }
            if (mnt_fd >= 0) close(mnt_fd);
            if (root_fd >= 0) close(root_fd);

            if (joined_ns) {
                /* Fork inner child so it actually enters the target CLONE_NEWPID namespace! */
                pid_t inner_sh = fork();
                if (inner_sh > 0) {
                    /* Parent ignores terminal signals so Ctrl+C goes directly to the foreground shell job */
                    signal(SIGINT, SIG_IGN);
                    signal(SIGQUIT, SIG_IGN);
                    signal(SIGTSTP, SIG_IGN);
                    int ist = 0;
                    waitpid(inner_sh, &ist, 0);
                    _exit(WIFEXITED(ist) ? WEXITSTATUS(ist) : 0);
                }

                /* Child: assign own process group and make it foreground on controlling tty.
                 * After setpgid() we are a *background* pgrp, so tcsetpgrp() raises SIGTTOU,
                 * whose default action STOPS us -> parent waitpid() hangs forever and the
                 * serial console appears frozen. Ignore SIGTTOU around the call. */
                signal(SIGTTOU, SIG_IGN);
                setpgid(0, 0);
                if (g_ttygs_fd >= 0) {
                    tcsetpgrp(0, getpgrp());
                }
                signal(SIGTTOU, SIG_DFL);
                signal(SIGINT, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);
                signal(SIGTSTP, SIG_DFL);
            }
        }

        if (!joined_ns) {
            if (chroot("/mnt/system_a") != 0) {
                perror("[-] chroot(/mnt/system_a) failed");
                _exit(1);
            }
            if (chdir("/") != 0) {
                perror("[-] chdir(/) failed");
                _exit(1);
            }
        }

        char *envp[] = {
            "PATH=/system/bin:/system/xbin:/vendor/bin:/vendor/xbin:/apex/com.android.runtime/bin:/apex/com.android.art/bin",
            "ANDROID_ROOT=/system",
            "ANDROID_DATA=/data",
            "ANDROID_STORAGE=/storage",
            "ANDROID_ART_ROOT=/apex/com.android.art",
            "ANDROID_I18N_ROOT=/apex/com.android.i18n",
            "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
            "TERM=xterm-256color",
            "HOSTNAME=sailfish",
            "HOME=/",
            NULL
        };

        /* Auto-start vndservicemanager and qservice_daemon if /dev/vndbinder is ready */
        if (!joined_ns && access("/dev/vndbinder", F_OK) == 0 && access("/vendor/bin/vndservicemanager", F_OK) == 0) {
            pid_t bg = fork();
            if (bg == 0) {
                int nfd = open("/dev/null", O_RDWR);
                if (nfd >= 0) {
                    dup2(nfd, 0); dup2(nfd, 1); dup2(nfd, 2);
                    close(nfd);
                }
                setsid();
                char *vargs[] = { (char *)"/vendor/bin/vndservicemanager", (char *)"/dev/vndbinder", NULL };
                pid_t vp = fork();
                if (vp == 0) {
                    execve(vargs[0], vargs, envp);
                    _exit(1);
                }
                usleep(500000);
                if (access("/data/qservice_daemon", F_OK) == 0) {
                    char *qargs[] = { (char *)"/data/qservice_daemon", NULL };
                    pid_t qp = fork();
                    if (qp == 0) {
                        execve(qargs[0], qargs, envp);
                        _exit(1);
                    }
                }
                _exit(0);
            }
        }

        char *argv[] = { (char *)"/system/bin/sh", NULL };
        execve("/system/bin/sh", argv, envp);
        execve("/system/bin/toybox", argv, envp);

        perror("[-] execve /system/bin/sh failed");
        _exit(127);
    }

    /* Parent waits for child shell */
    int status = 0;
    waitpid(pid, &status, 0);

    /* Restore non-blocking on ttyGS0 */
    if (g_ttygs_fd >= 0) {
        int flags = fcntl(g_ttygs_fd, F_GETFL, 0);
        fcntl(g_ttygs_fd, F_SETFL, flags | O_NONBLOCK);
    }

    char res[128];
    snprintf(res, sizeof(res), "\n[+] Android shell exited (status=%d)\n", WEXITSTATUS(status));
    usb_write(res);
}

static void drain_kmsg_to_usb(int kfd) {
    if (kfd < 0)
        return;
    char buf[4096];
    char out[4160];
    int lines = 50;
    while (lines-- > 0) {
        ssize_t n = read(kfd, buf, sizeof(buf) - 1);
        if (n < 0) {
            if (errno == EPIPE || errno == EINVAL)
                continue;
            break;
        }
        if (n == 0)
            break;
        buf[n] = '\0';
        char *semi = strchr(buf, ';');
        const char *msg = semi ? semi + 1 : buf;
        snprintf(out, sizeof(out), "[kmsg] %s", msg);
        usb_write_ex(out, false);
    }
}

static pid_t find_process_by_name(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    pid_t found = -1;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit(de->d_name[0])) continue;
        pid_t p = atoi(de->d_name);
        if (p <= 1) continue;
        char comm_path[128], comm[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)p);
        if (read_first_line(comm_path, comm, sizeof(comm)) && strstr(comm, name)) {
            found = p;
            break;
        }
    }
    closedir(d);
    return found;
}

static void supervisor_setprop(pid_t init_pid, const char *key, const char *val) {
    pid_t sp = fork();
    if (sp == 0) {
        int nfd = open("/dev/null", O_RDWR);
        if (nfd >= 0) {
            dup2(nfd, 0); dup2(nfd, 1); dup2(nfd, 2);
            close(nfd);
        }
        char pid_ns[128], mnt_ns[128], root_path[128];
        snprintf(pid_ns, sizeof(pid_ns), "/proc/%d/ns/pid", (int)init_pid);
        snprintf(mnt_ns, sizeof(mnt_ns), "/proc/%d/ns/mnt", (int)init_pid);
        snprintf(root_path, sizeof(root_path), "/proc/%d/root", (int)init_pid);

        int pfd = open(pid_ns, O_RDONLY);
        int mfd = open(mnt_ns, O_RDONLY);
        int rfd = open(root_path, O_RDONLY | O_DIRECTORY);

        if (pfd >= 0) { setns(pfd, CLONE_NEWPID); close(pfd); }
        if (mfd >= 0 && rfd >= 0) {
            setns(mfd, CLONE_NEWNS);
            fchdir(rfd);
            chroot(".");
            chdir("/");
            close(mfd);
            close(rfd);
        }
        char *envp[] = {
            "PATH=/system/bin:/system/xbin:/vendor/bin:/vendor/xbin:/apex/com.android.runtime/bin",
            "LD_LIBRARY_PATH=/system/lib64:/vendor/lib64:/system/lib64/bootstrap:/apex/com.android.runtime/lib64/bionic:/apex/com.android.runtime/lib64",
            "ANDROID_ROOT=/system",
            "ANDROID_DATA=/data",
            NULL
        };
        char *argv[] = { (char *)"/system/bin/setprop", (char *)key, (char *)val, NULL };
        execve("/system/bin/setprop", argv, envp);
        _exit(1);
    }
    if (sp > 0) {
        int st = 0;
        waitpid(sp, &st, 0);
    }
}

static void spawn_android_supervisor(void) {
    pid_t sup = fork();
    if (sup != 0) return; /* Parent returns immediately */

    setsid();
    usb_write("[+] Android boot supervisor started in background\n");

    /* Wait for Android init to start and property_service socket to appear */
    pid_t init_pid = -1;
    for (int w = 0; w < 240; w++) {
        init_pid = find_android_init_pid();
        if (init_pid > 1) {
            restore_linkerconfig_if_needed(init_pid);
            char prop_sock[128];
            snprintf(prop_sock, sizeof(prop_sock), "/proc/%d/root/dev/socket/property_service", (int)init_pid);
            if (access(prop_sock, F_OK) == 0) {
                break;
            }
        }
        usleep(250000);
    }

    if (init_pid <= 1) {
        usb_write("[-] Supervisor: Android property_service timed out\n");
        _exit(1);
    }

    /* Wait a moment for property_service to finish bind & listen */
    for (int r = 0; r < 6; r++) {
        restore_linkerconfig_if_needed(init_pid);
        usleep(250000);
    }
    usb_write("[+] Supervisor: Android property_service is online!\n");

    /* Apply critical boot properties */
    const char *props[][2] = {
        { "ctl.stop", "vendor.qseecomd" },
        { "ctl.stop", "vendor.time_daemon" },
        { "ctl.stop", "vendor.keymaster-3-0" },
        { "ctl.stop", "vendor.configstore-hal" },
        { "ro.crypto.state", "unencrypted" },
        { "ro.hw_timeout_multiplier", "10" },
        { "sys.use_memfd", "true" },
        { "debug.gralloc.gfx_ubwc_disable", "1" },
        { "debug.gralloc.enable_fb_ubwc", "0" },
        { "service.sf.prime_shader_cache", "0" },
        { "dalvik.vm.dexopt-flags", "v=n" },
        { "dalvik.vm.extra-opts", "-Xverify:none" },
        { "pm.dexopt.first-boot", "skip" },
        { "pm.dexopt.boot-after-ota", "skip" },
        { "dalvik.vm.boot-dex2oat-threads", "2" },
        { "dalvik.vm.dex2oat-threads", "2" },
        { "dalvik.vm.background-dex2oat-threads", "1" },
        { "sys.listeners.registered", "true" },
        { "sys.time.set", "true" },
        { "keystore.module_hash.sent", "true" },
        { "odsign.key.done", "1" },
        { "odsign.verification.done", "1" },
        { NULL, NULL }
    };

    for (int p = 0; props[p][0]; p++) {
        if (g_data_persistent &&
            (strcmp(props[p][0], "dalvik.vm.dexopt-flags") == 0 ||
             strcmp(props[p][0], "dalvik.vm.extra-opts") == 0 ||
             strncmp(props[p][0], "pm.dexopt.", 10) == 0)) {
            continue;
        }
        if (!g_data_persistent && strstr(props[p][0], "dex2oat-threads")) {
            continue;
        }
        supervisor_setprop(init_pid, props[p][0], props[p][1]);
        usleep(50000);
    }
    restore_linkerconfig_if_needed(init_pid);
    usb_write("[+] Supervisor: Boot properties applied (keystore/odsign gates unblocked)\n");

    /* Execute /data/start_graphics.sh inside the namespace as guaranteed fallback */
    char test_script[128];
    snprintf(test_script, sizeof(test_script), "/proc/%d/root/data/start_graphics.sh", (int)init_pid);
    if (access(test_script, F_OK) == 0) {
        pid_t gfork = fork();
        if (gfork == 0) {
            char pid_ns[128], mnt_ns[128], root_path[128];
            snprintf(pid_ns, sizeof(pid_ns), "/proc/%d/ns/pid", (int)init_pid);
            snprintf(mnt_ns, sizeof(mnt_ns), "/proc/%d/ns/mnt", (int)init_pid);
            snprintf(root_path, sizeof(root_path), "/proc/%d/root", (int)init_pid);

            int pfd = open(pid_ns, O_RDONLY);
            int mfd = open(mnt_ns, O_RDONLY);
            int rfd = open(root_path, O_RDONLY | O_DIRECTORY);

            if (pfd >= 0) { setns(pfd, CLONE_NEWPID); close(pfd); }
            if (mfd >= 0 && rfd >= 0) {
                setns(mfd, CLONE_NEWNS);
                fchdir(rfd);
                chroot(".");
                chdir("/");
                close(mfd);
                close(rfd);
            }
            char *gargv[] = { (char *)"/system/bin/sh", (char *)"/data/start_graphics.sh", NULL };
            char *genvp[] = {
                "PATH=/system/bin:/system/xbin:/vendor/bin:/vendor/xbin:/apex/com.android.runtime/bin",
                "ANDROID_ROOT=/system",
                "ANDROID_DATA=/data",
                NULL
            };
            execve("/system/bin/sh", gargv, genvp);
            _exit(1);
        }
        if (gfork > 0) {
            int gst = 0;
            waitpid(gfork, &gst, 0);
            usb_write("[+] Supervisor: /data/start_graphics.sh executed!\n");
        }
    }

    /* Monitor boot milestones (while keeping /linkerconfig/ld.config.txt intact) */
    bool sf_seen = false, ba_seen = false, zy_seen = false, sys_seen = false;
    for (int m = 0; m < 240; m++) {
        restore_linkerconfig_if_needed(init_pid);
        if (!sf_seen) {
            pid_t p = find_process_by_name("surfaceflinger");
            if (p > 0) {
                char msg[96];
                snprintf(msg, sizeof(msg), "[+] Milestone: surfaceflinger running (PID %d)\n", (int)p);
                usb_write(msg);
                sf_seen = true;
            }
        }
        if (!ba_seen) {
            pid_t p = find_process_by_name("bootanimation");
            if (p > 0) {
                char msg[96];
                snprintf(msg, sizeof(msg), "[+] Milestone: bootanimation running (PID %d)\n", (int)p);
                usb_write(msg);
                ba_seen = true;
            }
        }
        if (!zy_seen) {
            pid_t p = find_process_by_name("zygote64");
            if (p <= 0) p = find_process_by_name("app_process64");
            if (p > 0) {
                char msg[96];
                snprintf(msg, sizeof(msg), "[+] Milestone: zygote64 running (PID %d)\n", (int)p);
                usb_write(msg);
                zy_seen = true;
            }
        }
        if (init_pid > 1 && m > 15) {
            char zy_sock[128], sec_sock[128];
            snprintf(zy_sock, sizeof(zy_sock), "/proc/%d/root/dev/socket/zygote", (int)init_pid);
            snprintf(sec_sock, sizeof(sec_sock), "/proc/%d/root/dev/socket/zygote_secondary", (int)init_pid);
            if (access(zy_sock, F_OK) == 0 && access(sec_sock, F_OK) != 0) {
                symlink("/dev/socket/zygote", sec_sock);
                usb_write("[+] Supervisor: Linked /dev/socket/zygote_secondary -> zygote\n");
            }
            char usap_sock[128], usap_sec[128];
            snprintf(usap_sock, sizeof(usap_sock), "/proc/%d/root/dev/socket/usap_pool_primary", (int)init_pid);
            snprintf(usap_sec, sizeof(usap_sec), "/proc/%d/root/dev/socket/usap_pool_secondary", (int)init_pid);
            if (access(usap_sock, F_OK) == 0 && access(usap_sec, F_OK) != 0) {
                symlink("/dev/socket/usap_pool_primary", usap_sec);
                usb_write("[+] Supervisor: Linked /dev/socket/usap_pool_secondary -> usap_pool_primary\n");
            }
        }
        if (!sys_seen) {
            pid_t p = find_process_by_name("system_server");
            if (p > 0) {
                char msg[96];
                snprintf(msg, sizeof(msg), "[+] Milestone: system_server running (PID %d)\n", (int)p);
                usb_write(msg);
                sys_seen = true;
            }
        }
        for (int sub = 0; sub < 10; sub++) {
            usleep(100000);
            restore_linkerconfig_if_needed(init_pid);
        }
    }

    _exit(0);
}

static void boot_android_init(const char *stage) {
    if (!stage || !*stage)
        stage = "selinux_setup";

    /* Wait briefly for USB CDC-ACM gadget so early messages are captured */
    for (int u = 0; u < 25; u++) {
        if (!g_usb_bound) g_usb_bound = setup_usb_gadget();
        open_ttygs_if_ready();
        if (g_ttygs_fd >= 0) break;
        usleep(100000);
    }

    usb_write("[+] Preparing Android rootfs & filesystems...\n");
    unmount_android_mounts();
    prepare_android_rootfs(false);

    int kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (kfd >= 0)
        lseek(kfd, 0, SEEK_END);

    char msg[192];
    snprintf(msg, sizeof(msg),
             "[+] Launching Android /system/bin/init (%s) in isolated PID 1 namespace...\n"
             "[+] Streaming live /dev/kmsg to serial! (Press Ctrl+C to return to shell)\n\n",
             stage);
    usb_write(msg);

    pid_t outer = fork();
    if (outer < 0) {
        if (kfd >= 0) close(kfd);
        usb_write("[-] fork failed\n");
        return;
    }

    if (outer == 0) {
        if (kfd >= 0) close(kfd);

        if (chdir("/mnt/system_a") != 0) {
            perror("[-] chdir(/mnt/system_a) failed");
            _exit(1);
        }
        if (chroot(".") != 0) {
            perror("[-] chroot(/mnt/system_a) failed");
            _exit(1);
        }
        chdir("/");

        /* unshare(CLONE_NEWNS) clones fs->root (/mnt/system_a) as the true mnt_ns->root! */
        if (unshare(CLONE_NEWPID | CLONE_NEWNS) != 0) {
            perror("[-] unshare(CLONE_NEWPID|CLONE_NEWNS) failed");
        }

        pid_t inner = fork();
        if (inner < 0) {
            perror("[-] inner fork failed");
            _exit(1);
        }

        if (inner == 0) {
            /* Child is now PID 1 inside its own pid_namespace and mnt_namespace rooted at /mnt/system_a */
            mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL);
            /* Restore shared propagation on /data so /data/data -> /data/user/0 propagates to /data_mirror */
            mount(NULL, "/data", NULL, MS_SHARED, NULL);

            /* Mount fresh /proc for this PID namespace so /proc/1 is Android init */
            umount2("/proc", MNT_DETACH);
            mkdir("/proc", 0555);
            mount("proc", "/proc", "proc", 0, NULL);

            /* Prevent USB-plugged power-on resets from trapping Android init in charger mode */
            int cmd_fd = open("/proc/cmdline", O_RDONLY);
            if (cmd_fd >= 0) {
                char cmdline[2048];
                ssize_t clen = read(cmd_fd, cmdline, sizeof(cmdline) - 1);
                close(cmd_fd);
                if (clen > 0) {
                    cmdline[clen] = '\0';
                    const char *needle = "androidboot.mode=charger";
                    char *hit = strstr(cmdline, needle);
                    if (hit) {
                        char patched[2048];
                        size_t pre = (size_t)(hit - cmdline);
                        snprintf(patched, sizeof(patched), "%.*sandroidboot.mode=normal%s",
                                 (int)pre, cmdline, hit + strlen(needle));
                        write_file_str("/dev/.sailfish_cmdline", patched);
                        chmod("/dev/.sailfish_cmdline", 0444);
                        mount("/dev/.sailfish_cmdline", "/proc/cmdline", NULL, MS_BIND, NULL);
                    }
                }
            }

            /* Direct early init stdout/stderr to kmsg and ttyGS0 */
            int kfd_out = open("/dev/kmsg", O_WRONLY | O_NOCTTY);
            if (kfd_out >= 0) {
                dup2(kfd_out, 1);
                dup2(kfd_out, 2);
                close(kfd_out);
            }
            if (g_ttygs_fd >= 0) {
                dup2(g_ttygs_fd, 1);
                dup2(g_ttygs_fd, 2);
            }

            char *argv_stage[] = { (char *)"/system/bin/init", (char *)stage, NULL };
            char *argv_plain[] = { (char *)"/system/bin/init", NULL };
            char *envp[] = {
                "PATH=/sbin:/system/sbin:/system/bin:/system/xbin:/vendor/bin:/vendor/xbin:/apex/com.android.runtime/bin",
                "ANDROID_ROOT=/system",
                "ANDROID_DATA=/data",
                "ANDROID_STORAGE=/storage",
                "ANDROID_ART_ROOT=/apex/com.android.art",
                "ANDROID_I18N_ROOT=/apex/com.android.i18n",
                "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
                "TERM=linux",
                NULL
            };

            if (strcmp(stage, "first_stage") == 0 || strcmp(stage, "none") == 0) {
                execve("/system/bin/init", argv_plain, envp);
            } else {
                execve("/system/bin/init", argv_stage, envp);
            }
            execve("/system/bin/init", argv_plain, envp);
            perror("[-] execve /system/bin/init failed");
            _exit(127);
        }

        int st = 0;
        waitpid(inner, &st, 0);
        if (WIFEXITED(st))
            _exit(WEXITSTATUS(st));
        _exit(128 + WTERMSIG(st));
    }

    /* Spawn background supervisor to monitor boot and apply runtime properties */
    spawn_android_supervisor();

    int status = 0;
    bool user_detached = false;
    while (1) {
        if (!g_usb_bound)
            g_usb_bound = setup_usb_gadget();

        open_ttygs_if_ready();

        drain_kmsg_to_usb(kfd);

        pid_t w = waitpid(outer, &status, WNOHANG);
        if (w > 0 || w < 0)
            break;

        if (g_ttygs_fd >= 0) {
            struct pollfd pfd;
            pfd.fd = g_ttygs_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int prc = poll(&pfd, 1, 50);
            if (prc > 0) {
                if (pfd.revents & POLLIN) {
                    char rx[64];
                    ssize_t n = read(g_ttygs_fd, rx, sizeof(rx));
                    if (n > 0) {
                        for (ssize_t i = 0; i < n; i++) {
                            if (rx[i] == 0x03) { /* Ctrl+C */
                                user_detached = true;
                                goto done;
                            } else if (rx[i] == '\r' || rx[i] == '\n') {
                                usb_write("\n[sailfish: streaming live /dev/kmsg. Press Ctrl+C for shell]\n");
                            }
                        }
                    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        close_ttygs();
                        usleep(100000);
                    }
                }
                if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    close_ttygs();
                    usleep(100000);
                }
            }
        } else {
            usleep(50000);
        }
    }

done:
    usleep(200000);
    drain_kmsg_to_usb(kfd);
    if (kfd >= 0)
        close(kfd);

    if (user_detached) {
        usb_write("\n[!] Ctrl+C detected: Detached log stream (Android init continues running in background)\n");
        usb_write("[+] Type 'sh' for Android root shell, 'logcat' for logs, or 'boot_android' to stream.\n\n");
    } else {
        char exit_msg[160];
        snprintf(exit_msg, sizeof(exit_msg),
                 "\n[+] Android /init exited (status=%d) — phone kept alive in sailfish# shell!\n",
                 WIFEXITED(status) ? WEXITSTATUS(status) : status);
        usb_write(exit_msg);
    }
}

static void boot_linux_init(void) {
    usb_write("[+] Preparing Linux rootfs & mount points...\n");
    mkdir("/mnt", 0755);
    mkdir("/mnt/system_a", 0755);

    if (mount("/dev/sda33", "/mnt/system_a", "ext4", MS_REMOUNT, NULL) != 0 &&
        mount("/dev/sda33", "/mnt/system_a", "ext4", 0, NULL) != 0) {
        mount("/dev/sda35", "/mnt/system_a", "ext4", 0, NULL);
    }

    mkdir("/mnt/system_a/dev", 0755);
    mount("/dev", "/mnt/system_a/dev", NULL, MS_BIND | MS_REC, NULL);

    mkdir("/mnt/system_a/proc", 0755);
    mount("/proc", "/mnt/system_a/proc", NULL, MS_BIND | MS_REC, NULL);

    mkdir("/mnt/system_a/sys", 0755);
    mount("/sys", "/mnt/system_a/sys", NULL, MS_BIND | MS_REC, NULL);

    usb_write("[+] Moving root mount to /mnt/system_a (switch_root)...\n");
    mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL);

    if (chdir("/mnt/system_a") != 0) {
        usb_write("[-] chdir(/mnt/system_a) failed\n");
        return;
    }

    if (mount(".", "/", NULL, MS_MOVE, NULL) != 0) {
        char err[128];
        snprintf(err, sizeof(err), "[-] mount MS_MOVE failed: %s (falling back to chroot)\n", strerror(errno));
        usb_write(err);
    }

    if (chroot(".") != 0) {
        usb_write("[-] chroot failed\n");
        return;
    }
    chdir("/");

    usb_write("[+] Executing Linux init (/sbin/init / OpenRC / systemd) as PID 1...\n\n");
    sync();
    usleep(100000);

    char *argv[] = { (char *)"/sbin/init", NULL };
    char *envp[] = {
        "PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/bin",
        "TERM=linux",
        "HOME=/root",
        "USER=root",
        NULL
    };

    execve("/sbin/init", argv, envp);
    execve("/usr/lib/systemd/systemd", argv, envp);
    execve("/lib/systemd/systemd", argv, envp);
    execve("/bin/sh", argv, envp);

    char err2[128];
    snprintf(err2, sizeof(err2), "[-] execve failed: %s\n", strerror(errno));
    usb_write(err2);
}

static void test_gpu(void) {
    char out[256];
    usb_write("[+] DRM DRI nodes (/dev/dri):\n");
    dump_sysfs_dir("dri", "/dev/dri", true);

    const char *nodes[] = {
        "/dev/dri/renderD128",
        "/dev/dri/card1",
        "/dev/dri/card0",
        NULL
    };

    bool opened = false;
    for (int i = 0; nodes[i]; i++) {
        int fd = open(nodes[i], O_RDWR);
        if (fd >= 0) {
            snprintf(out, sizeof(out), "[+] Successfully opened %s (fd=%d) - triggered load_gpu()\n", nodes[i], fd);
            usb_write(out);

            char name[64] = {0};
            char date[64] = {0};
            char desc[128] = {0};
            struct drm_version ver;
            memset(&ver, 0, sizeof(ver));
            ver.name = name;
            ver.name_len = sizeof(name) - 1;
            ver.date = date;
            ver.date_len = sizeof(date) - 1;
            ver.desc = desc;
            ver.desc_len = sizeof(desc) - 1;
            if (ioctl(fd, DRM_IOCTL_VERSION, &ver) == 0) {
                snprintf(out, sizeof(out), "    DRM Driver: %s (%s) v%d.%d.%d\n    Desc: %s\n",
                         name, date, ver.version_major, ver.version_minor, ver.version_patchlevel, desc);
                usb_write(out);
            }

            close(fd);
            opened = true;
            break;
        } else {
            snprintf(out, sizeof(out), "[-] Cannot open %s: %s\n", nodes[i], strerror(errno));
            usb_write(out);
        }
    }

    usb_write("[+] Checking kernel dmesg for Adreno / Freedreno GPU initialization:\n");
    dump_dmesg_filtered(60, "adreno");
    dump_dmesg_filtered(60, "msm");
}

static void cmd_format_userdata(bool confirmed) {
    if (!confirmed) {
        usb_write("[!] WARNING: This formats /dev/sda35 (userdata) as unencrypted ext4 for persistent /data.\n"
                  "[!] Any stock or encrypted data currently on /dev/sda35 will be ERASED.\n"
                  "[!] Run 'format_userdata confirm' if you want to proceed.\n");
        return;
    }

    if (find_android_init_pid() > 1) {
        usb_write("[-] Cannot format userdata while Android init is running. Reboot to sailfish# first.\n");
        return;
    }

    if (access("/dev/sda35", F_OK) != 0) {
        usb_write("[-] /dev/sda35 not found\n");
        return;
    }

    umount_all("/mnt/system_a/data");
    umount_all("/mnt/userdata_probe");

    if (dev_in_proc_mounts("/dev/sda35")) {
        usb_write("[-] /dev/sda35 is still mounted elsewhere (e.g. as system root); refusing format.\n");
        return;
    }

    mkdir("/mnt", 0755);
    mkdir("/mnt/system_a", 0755);
    if (access("/mnt/system_a/system/bin/mke2fs", F_OK) != 0) {
        mount("/dev/sda33", "/mnt/system_a", "ext4", MS_RDONLY, NULL);
    }
    if (access("/mnt/system_a/system/bin/mke2fs", F_OK) != 0) {
        usb_write("[-] /mnt/system_a/system/bin/mke2fs not available on sda33; use 'fastboot format:ext4 userdata'\n");
        return;
    }

    bool bound_dev = false, bound_proc = false;
    if (access("/mnt/system_a/dev/sda35", F_OK) != 0) {
        mkdir("/mnt/system_a/dev", 0755);
        bound_dev = (mount("/dev", "/mnt/system_a/dev", NULL, MS_BIND | MS_REC, NULL) == 0);
    }
    if (access("/mnt/system_a/proc/mounts", F_OK) != 0) {
        mkdir("/mnt/system_a/proc", 0755);
        bound_proc = (mount("proc", "/mnt/system_a/proc", "proc", 0, NULL) == 0);
    }

    int prev_flags = -1;
    if (g_ttygs_fd >= 0) {
        prev_flags = fcntl(g_ttygs_fd, F_GETFL, 0);
        if (prev_flags >= 0)
            fcntl(g_ttygs_fd, F_SETFL, prev_flags & ~O_NONBLOCK);
    }

    usb_write("[+] Formatting /dev/sda35 as unencrypted ext4 (label=userdata)...\n");
    pid_t pid = fork();
    if (pid == 0) {
        if (g_ttygs_fd >= 0) {
            dup2(g_ttygs_fd, 0);
            dup2(g_ttygs_fd, 1);
            dup2(g_ttygs_fd, 2);
        }
        if (chroot("/mnt/system_a") != 0 || chdir("/") != 0)
            _exit(126);
        char *const argv[] = { "mke2fs", "-F", "-t", "ext4", "-b", "4096", "-L", "userdata", "/dev/sda35", NULL };
        char *const envp[] = { "PATH=/system/bin", NULL };
        execve("/system/bin/mke2fs", argv, envp);
        _exit(127);
    }

    int st = -1;
    if (pid > 0)
        waitpid(pid, &st, 0);

    if (prev_flags >= 0 && g_ttygs_fd >= 0)
        fcntl(g_ttygs_fd, F_SETFL, prev_flags);

    if (bound_proc)
        umount_all("/mnt/system_a/proc");
    if (bound_dev)
        umount_all("/mnt/system_a/dev");

    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        char emsg[128];
        snprintf(emsg, sizeof(emsg), "[-] mke2fs failed (status=%d)\n", WIFEXITED(st) ? WEXITSTATUS(st) : st);
        usb_write(emsg);
        return;
    }

    mkdir("/mnt/userdata_probe", 0755);
    if (mount("/dev/sda35", "/mnt/userdata_probe", "ext4", MS_NOATIME, NULL) == 0) {
        int mfd = open("/mnt/userdata_probe/.sailfish_unencrypted", O_WRONLY | O_CREAT, 0600);
        if (mfd >= 0) {
            write(mfd, "sailfish-unencrypted-data\n", 26);
            close(mfd);
        }
        chown("/mnt/userdata_probe", 1000, 1000);
        chmod("/mnt/userdata_probe", 0771);
        sync();
        if (umount2("/mnt/userdata_probe", 0) != 0)
            umount_all("/mnt/userdata_probe");
        usb_write("[+] /dev/sda35 formatted & marked unencrypted! Run 'boot_android' or 'reboot'.\n");
    } else {
        usb_write("[!] Formatted /dev/sda35, but probe-mount failed; reboot and check.\n");
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
                  "  ramoops [filter]  - Dump ramoops log (default: last 16KB; 'all' for full)\n"
                  "  sh / android_sh   - Launch interactive Android Bionic root shell\n"
                  "  boot_android      - Pivot root and hand off control to Android /init\n"
                  "  boot_linux / pmos - Pivot root and hand off control to Linux /sbin/init\n"
                  "  input / touch     - List input devices & monitor events for 10s\n"
                  "  battery           - Show battery capacity, voltage, and charger state\n"
                  "  thermal / diag    - Show thermal temps, CPU/GPU freqs, ZRAM, and ramoops\n"
                  "  display / drm     - Show DRM/KMS connectors, CRTCs, and backlight\n"
                  "  gpu / freedreno   - Test DRI nodes & trigger GPU firmware load\n"
                  "  wifi              - Show PCIe/ath10k/wlan0/rfkill status & dmesg\n"
                  "  android / aosp    - Check Binder, DMA-BUF heaps, PSI, and SELinux\n"
                  "  touch_test        - Sweep switch & reset GPIOs and test I2C ACK\n"
                  "  i2c / i2c_scan    - Scan /dev/i2c-0 for responsive slave addresses\n"
                  "  usb               - Show UDC status & speed\n"
                  "  parts             - Show /proc/partitions\n"
                  "  dtb / stock_dtb   - Scan stock boot_a for factory DTB & touch node\n"
                  "  dump_dtb          - Dump stock factory DTB as base64\n"
                  "  dump_zap          - Dump signed Adreno zap shader files as base64\n"
                  "  b64 <file>        - Dump any file as base64\n"
                  "  mounts            - Show /proc/mounts\n"
                  "  ls [dir]          - List directory entries\n"
                  "  cat <file>        - Print file contents (up to 2000 lines)\n"
                  "  mount_sys         - Mount sda33 (/mnt/system_a) & sda31 (/mnt/vendor_a) ro\n"
                  "  format_userdata   - Format /dev/sda35 as unencrypted ext4 /data (needs confirm)\n"
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
    } else if (strcmp(cmd, "logcat") == 0 || strncmp(cmd, "logcat ", 7) == 0) {
        const char *flt = (strlen(cmd) > 7) ? cmd + 7 : NULL;
        while (flt && *flt == ' ') flt++;
        dump_logcat_filtered(flt);
    } else if (strcmp(cmd, "ramoops") == 0 || strncmp(cmd, "ramoops ", 8) == 0) {
        const char *flt = (strlen(cmd) > 8) ? cmd + 8 : NULL;
        while (flt && *flt == ' ') flt++;
        dump_sysfs_dir("pstore", "/sys/fs/pstore", true);
        if (flt && strcmp(flt, "all") == 0) {
            dump_file_raw("/sys/fs/pstore/console-ramoops-0");
        } else if (flt && *flt && strcmp(flt, "tail") != 0) {
            dump_file_lines("/sys/fs/pstore/console-ramoops-0", 500, flt, true);
        } else {
            usb_write("[+] Showing last 16KB of /sys/fs/pstore/console-ramoops-0:\n");
            dump_file_tail("/sys/fs/pstore/console-ramoops-0", 16384);
        }
    } else if (strcmp(cmd, "input") == 0 || strcmp(cmd, "touch") == 0) {
        test_input_events(10);
    } else if (strcmp(cmd, "battery") == 0 || strcmp(cmd, "power") == 0) {
        dump_battery_info();
    } else if (strcmp(cmd, "thermal") == 0 || strcmp(cmd, "diag") == 0) {
        dump_thermal_perf();
    } else if (strcmp(cmd, "display") == 0 || strcmp(cmd, "drm") == 0) {
        dump_display_info();
    } else if (strcmp(cmd, "gpu") == 0 || strcmp(cmd, "freedreno") == 0) {
        test_gpu();
    } else if (strcmp(cmd, "wifi") == 0) {
        dump_wifi_info();
    } else if (strcmp(cmd, "android") == 0 || strcmp(cmd, "aosp") == 0) {
        test_android_primitives();
    } else if (strcmp(cmd, "android_sh") == 0 || strcmp(cmd, "sh") == 0) {
        launch_android_shell();
    } else if (strcmp(cmd, "boot_android") == 0 || strcmp(cmd, "boot") == 0) {
        pid_t cur_init = find_android_init_pid();
        if (cur_init > 1) {
            char amsg[256];
            snprintf(amsg, sizeof(amsg),
                     "[!] Android init is already running (PID %d)! Streaming /dev/kmsg...\n"
                     "[+] Press Ctrl+C to return to sailfish# shell, or type 'sh' to enter Android root shell.\n\n",
                     (int)cur_init);
            usb_write(amsg);
            int kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (kfd >= 0)
        lseek(kfd, 0, SEEK_END);
            if (kfd >= 0) lseek(kfd, 0, SEEK_END);
            while (1) {
                drain_kmsg_to_usb(kfd);
                if (g_ttygs_fd >= 0) {
                    struct pollfd pfd = { .fd = g_ttygs_fd, .events = POLLIN, .revents = 0 };
                    if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
                        char rx[16];
                        ssize_t n = read(g_ttygs_fd, rx, sizeof(rx));
                        for (ssize_t i = 0; i < n; i++) {
                            if (rx[i] == 0x03) goto stream_done;
                        }
                    }
                } else {
                    usleep(50000);
                }
            }
stream_done:
            if (kfd >= 0) close(kfd);
        } else {
            boot_android_init("selinux_setup");
        }
    } else if (strncmp(cmd, "boot_android ", 13) == 0) {
        const char *st = cmd + 13;
        while (*st == ' ') st++;
        boot_android_init(st);
    } else if (strcmp(cmd, "boot_linux") == 0 || strcmp(cmd, "boot_pmos") == 0 || strcmp(cmd, "pmos") == 0) {
        boot_linux_init();
    } else if (strcmp(cmd, "usb") == 0) {
        char udc[96], state[64], speed[64], vid[16], pid[16];
        if (!find_udc(udc, sizeof(udc)))
            snprintf(udc, sizeof(udc), "6a00000.usb");
        get_udc_state(state, sizeof(state));
        char sp_path[160];
        snprintf(sp_path, sizeof(sp_path), "/sys/class/udc/%s/current_speed", udc);
        read_first_line(sp_path, speed, sizeof(speed));
        read_first_line("/sys/kernel/config/usb_gadget/g1/idVendor", vid, sizeof(vid));
        read_first_line("/sys/kernel/config/usb_gadget/g1/idProduct", pid, sizeof(pid));
        char out[256];
        snprintf(out, sizeof(out), "UDC=%s state=%s speed=%s id=%s:%s ffs_adb=%s\n",
                 udc, state, speed, vid, pid,
                 access("/sys/kernel/config/usb_gadget/g1/configs/c.1/ffs.adb", F_OK) == 0 ? "linked" : "unlinked");
        usb_write(out);
        dump_sysfs_dir("USB gadget configs/c.1", "/sys/kernel/config/usb_gadget/g1/configs/c.1", true);
    } else if (strcmp(cmd, "parts") == 0) {
        dump_file_lines("/proc/partitions", 120, NULL, true);
    } else if (strcmp(cmd, "dtb") == 0 || strcmp(cmd, "stock_dtb") == 0) {
        scan_sda_for_dtb(true, false);
    } else if (strcmp(cmd, "dump_dtb") == 0) {
        scan_sda_for_dtb(true, true);
    } else if (strcmp(cmd, "dump_zap") == 0) {
        usb_write("[+] Dumping signed Adreno 530 zap shader firmware files:\n");
        dump_firmware_file_b64("a530_zap_signed.mdt");
        dump_firmware_file_b64("a530_zap_signed.b00");
        dump_firmware_file_b64("a530_zap_signed.b01");
        dump_firmware_file_b64("a530_zap_signed.b02");
        dump_firmware_file_b64("a530_zap_signed.mbn");
    } else if (strncmp(cmd, "b64 ", 4) == 0) {
        const char *p = cmd + 4;
        while (*p == ' ') p++;
        const char *slash = strrchr(p, '/');
        const char *lbl = slash ? slash + 1 : p;
        int fd = open(p, O_RDONLY);
        if (fd < 0) {
            char err[160];
            snprintf(err, sizeof(err), "[-] Cannot open %s: %s\n", p, strerror(errno));
            usb_write(err);
        } else {
            struct stat st;
            if (fstat(fd, &st) < 0 || st.st_size <= 0) {
                close(fd);
                usb_write("[-] Bad file size\n");
            } else {
                uint8_t *buf = malloc(st.st_size);
                if (buf && read(fd, buf, st.st_size) == st.st_size) {
                    dump_base64_buf(lbl, buf, st.st_size, true);
                } else {
                    usb_write("[-] Read error\n");
                }
                free(buf);
                close(fd);
            }
        }
    } else if (strcmp(cmd, "touch_test") == 0 || strcmp(cmd, "touch_probe") == 0) {
        test_touch_hardware();
    } else if (strcmp(cmd, "i2c") == 0 || strcmp(cmd, "i2c_scan") == 0) {
        scan_i2c_bus();
    } else if (strcmp(cmd, "mounts") == 0) {
        dump_file_lines("/proc/mounts", 120, NULL, true);
    } else if (strcmp(cmd, "ls") == 0 || strncmp(cmd, "ls ", 3) == 0) {
        const char *dir = (strlen(cmd) > 3) ? cmd + 3 : "/";
        while (*dir == ' ') dir++;
        if (strncmp(dir, "-l ", 3) == 0) dir += 3;
        else if (strncmp(dir, "-la ", 4) == 0) dir += 4;
        else if (strncmp(dir, "-al ", 4) == 0) dir += 4;
        else if (strncmp(dir, "-a ", 3) == 0) dir += 3;
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
        const char *sys_dev = "sda33";
        int r1 = mount("/dev/sda33", "/mnt/system_a", "ext4", MS_REMOUNT, NULL);
        if (r1 != 0) r1 = mount("/dev/sda33", "/mnt/system_a", "ext4", 0, NULL);
        if (r1 != 0) r1 = mount("/dev/sda33", "/mnt/system_a", "ext4", MS_RDONLY, NULL);
        if (r1 != 0 && access("/mnt/system_a/system/bin/init", F_OK) == 0) r1 = 0;
        if (r1 != 0) {
            r1 = mount("/dev/sda35", "/mnt/system_a", "ext4", MS_REMOUNT, NULL);
            if (r1 != 0) r1 = mount("/dev/sda35", "/mnt/system_a", "ext4", 0, NULL);
            if (r1 == 0) sys_dev = "sda35(userdata)";
        }
        int r2 = mount("/dev/sda31", "/mnt/vendor_a", "ext4", MS_RDONLY, NULL);
        if (r2 == 0) {
            mkdir("/vendor", 0755);
            mount("/mnt/vendor_a", "/vendor", NULL, MS_BIND | MS_RDONLY, NULL);
            mkdir("/lib", 0755);
            mkdir("/lib/firmware", 0755);
            mount("/mnt/vendor_a/firmware", "/lib/firmware", NULL, MS_BIND | MS_RDONLY, NULL);
            mkdir("/lib/firmware/qcom", 0755);
            mount("/mnt/vendor_a/firmware", "/lib/firmware/qcom", NULL, MS_BIND | MS_RDONLY, NULL);
        }
        char out[220];
        snprintf(out, sizeof(out), "mount system (%s)->/mnt/system_a: %d (%s)\nmount sda31->/mnt/vendor_a: %d (%s)\n",
                 sys_dev, r1, r1 == 0 ? "ok" : strerror(errno),
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
    } else if (strcmp(cmd, "format_userdata") == 0) {
        cmd_format_userdata(false);
    } else if (strcmp(cmd, "format_userdata confirm") == 0) {
        cmd_format_userdata(true);
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
    if (access("/dev/tty", F_OK) != 0) {
        mknod("/dev/tty", S_IFCHR | 0666, makedev(5, 0));
    }
    mount("none", "/proc", "proc", 0, NULL);
    mount("none", "/sys", "sysfs", 0, NULL);
    mkdir("/sys/fs/pstore", 0755);
    mount("pstore", "/sys/fs/pstore", "pstore", 0, NULL);
    mkdir("/sys/fs/selinux", 0755);
    mount("none", "/sys/fs/selinux", "selinuxfs", 0, NULL);
    mkdir("/dev/binderfs", 0755);
    mount("binder", "/dev/binderfs", "binder", 0, "stats=global");
    symlink("/dev/binderfs/binder", "/dev/binder");
    symlink("/dev/binderfs/hwbinder", "/dev/hwbinder");
    symlink("/dev/binderfs/vndbinder", "/dev/vndbinder");

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

    g_usb_bound = setup_usb_gadget();
    if (!g_usb_bound)
        print_msg("[-] UDC not ready yet; will retry in loop\n");

    /* Mount installed system_a (sda33) read-write and vendor_a read-only */
    mkdir("/mnt", 0755);
    mkdir("/mnt/system_a", 0755);
    mkdir("/mnt/vendor_a", 0755);
    int sys_rc = mount("/dev/sda33", "/mnt/system_a", "ext4", 0, NULL);
    if (sys_rc != 0)
        sys_rc = mount("/dev/sda33", "/mnt/system_a", "ext4", MS_RDONLY, NULL);
    if (sys_rc != 0) {
        sys_rc = mount("/dev/sda35", "/mnt/system_a", "ext4", 0, NULL);
        if (sys_rc == 0)
            print_msg("[+] Mounted system root from /dev/sda35 (userdata)\n");
    }
    int ven_rc = mount("/dev/sda31", "/mnt/vendor_a", "ext4", MS_RDONLY, NULL);
    if (ven_rc == 0) {
        mkdir("/vendor", 0755);
        mount("/mnt/vendor_a", "/vendor", NULL, MS_BIND | MS_RDONLY, NULL);
        mkdir("/lib", 0755);
        mkdir("/lib/firmware", 0755);
        mount("/mnt/vendor_a/firmware", "/lib/firmware", NULL, MS_BIND | MS_RDONLY, NULL);
        mkdir("/lib/firmware/qcom", 0755);
        mount("/mnt/vendor_a/firmware", "/lib/firmware/qcom", NULL, MS_BIND | MS_RDONLY, NULL);
        print_msg("[+] Bound /mnt/vendor_a/firmware to /vendor/firmware and /lib/firmware\n");
    }
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

    /* Automated: Auto-boot Android 17 userspace immediately on boot */
    if (!g_usb_bound) g_usb_bound = setup_usb_gadget();
    open_ttygs_if_ready();
    print_msg("[+] Auto-starting Android 17 userspace immediately...\n");
    boot_android_init("selinux_setup");

    char cmd_buf[256];
    size_t cmd_len = 0;
    long start_sec = monotonic_sec();
    long last_hb_sec = start_sec;

    while (1) {
        if (!g_usb_bound)
            g_usb_bound = setup_usb_gadget();

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
