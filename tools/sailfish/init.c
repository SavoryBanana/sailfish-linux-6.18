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

static void write_file_str(const char *path, const char *val) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, val, strlen(val));
        close(fd);
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
        "max_bandwidth_low=9600000000\n"
        "max_bandwidth_high=9600000000\n"
        "max_pipe_bandwidth=2500000000\n"
        "max_sde_clk=412500000\n"
        "clk_fudge_factor=105 100\n"
        "scale_factor=1\n"
        "xtra_ff_factor=1\n"
        "blending_stages=4\n"
        "features=qseed3,ubwc,src_split\n"
        "pipe_count=10\n"
        "pipe_type=vig pipe_ndx=1 rects=1\n"
        "pipe_type=vig pipe_ndx=2 rects=1\n"
        "pipe_type=vig pipe_ndx=3 rects=1\n"
        "pipe_type=vig pipe_ndx=4 rects=1\n"
        "pipe_type=dma pipe_ndx=5 rects=1\n"
        "pipe_type=dma pipe_ndx=6 rects=1\n"
        "pipe_type=dma pipe_ndx=7 rects=1\n"
        "pipe_type=dma pipe_ndx=8 rects=1\n"
        "pipe_type=cursor pipe_ndx=9 rects=1\n"
        "pipe_type=cursor pipe_ndx=10 rects=1\n";
    write_file_str("/sys/devices/virtual/graphics/fb0/mdp/caps", caps_content);

    const char *panel_content =
        "panel_name=s6e3fa5_cmd\n"
        "primary_panel=1\n"
        "xres=1080\n"
        "yres=1920\n"
        "bpp=24\n"
        "type=mipi dsi cmd panel\n"
        "fps=60\n";
    write_file_str("/sys/devices/virtual/graphics/fb0/msm_fb_panel_info", panel_content);

    write_file_str("/sys/devices/virtual/graphics/fb0/msm_fb_type", "mipi dsi cmd panel\n");
    write_file_str("/sys/devices/virtual/graphics/fb0/connected", "1\n");
    write_file_str("/sys/devices/virtual/graphics/fb0/msm_fb_split", "0 0\n");
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

static bool prepare_android_rootfs(bool for_shell) {
    write_str("/proc/sys/kernel/printk_devkmsg", "on\n");
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

    /* Bind vendor into system_a/vendor */
    mkdir("/mnt/system_a/vendor", 0755);
    mount("/mnt/vendor_a", "/mnt/system_a/vendor", NULL, MS_BIND | MS_REC, NULL);

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

    /* Automated: Sync SELinux sha256 checksums to vendor so precompiled_sepolicy loads */
    const char *sha_pairs[][2] = {
        { "/mnt/system_a/system/etc/selinux/plat_sepolicy_and_mapping.sha256",
          "/mnt/system_a/vendor/etc/selinux/precompiled_sepolicy.plat_sepolicy_and_mapping.sha256" },
        { "/mnt/system_a/system_ext/etc/selinux/system_ext_sepolicy_and_mapping.sha256",
          "/mnt/system_a/vendor/etc/selinux/precompiled_sepolicy.system_ext_sepolicy_and_mapping.sha256" },
        { "/mnt/system_a/product/etc/selinux/product_sepolicy_and_mapping.sha256",
          "/mnt/system_a/vendor/etc/selinux/precompiled_sepolicy.product_sepolicy_and_mapping.sha256" },
        { NULL, NULL }
    };
    for (int i = 0; sha_pairs[i][0]; i++) {
        if (access(sha_pairs[i][0], F_OK) == 0 && access(sha_pairs[i][1], F_OK) == 0) {
            mount(sha_pairs[i][0], sha_pairs[i][1], NULL, MS_BIND, NULL);
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

    /* Automated: Inject graphics VINTF manifest fragment (composer@2.1 & allocator@2.0) */
    if (access("/tools/android.hardware.graphics.xml", F_OK) == 0) {
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
            copy_file("/tools/android.hardware.graphics.xml", "/tmp/vintf_manifest/android.hardware.graphics.xml");
            set_selinux_label("/tmp/vintf_manifest/android.hardware.graphics.xml", "u:object_r:system_file:s0");
            if (mount("/tmp/vintf_manifest", vintf_dir, NULL, MS_BIND, NULL) == 0) {
                usb_write("[+] Automated: Overlaid /system/etc/vintf/manifest with android.hardware.graphics.xml\n");
            }
        }
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

    /* Bootstrap fallback inside com.android.runtime when launching interactive sh */
    if (for_shell) {
        const char *apex_roots[] = { "/mnt/system_a/apex", "/mnt/system_a/bootstrap-apex", NULL };
        for (int a = 0; apex_roots[a]; a++) {
            char ap[256];
            snprintf(ap, sizeof(ap), "%s/com.android.runtime", apex_roots[a]);
            mkdir(ap, 0755);
            snprintf(ap, sizeof(ap), "%s/com.android.runtime/bin", apex_roots[a]);
            mkdir(ap, 0755);
            snprintf(ap, sizeof(ap), "%s/com.android.runtime/lib64", apex_roots[a]);
            mkdir(ap, 0755);
            snprintf(ap, sizeof(ap), "%s/com.android.runtime/lib64/bionic", apex_roots[a]);
            mkdir(ap, 0755);
            snprintf(ap, sizeof(ap), "%s/com.android.runtime/bin/linker64", apex_roots[a]);
            symlink("/system/bin/bootstrap/linker64", ap);
            snprintf(ap, sizeof(ap), "%s/com.android.runtime/lib64/bionic/libc.so", apex_roots[a]);
            symlink("/system/lib64/bootstrap/libc.so", ap);
            snprintf(ap, sizeof(ap), "%s/com.android.runtime/lib64/bionic/libm.so", apex_roots[a]);
            symlink("/system/lib64/bootstrap/libm.so", ap);
            snprintf(ap, sizeof(ap), "%s/com.android.runtime/lib64/bionic/libdl.so", apex_roots[a]);
            symlink("/system/lib64/bootstrap/libdl.so", ap);
        }
    }

    /* Automated: Populate Qualcomm MDSS sysfs mock before binding /sys */
    setup_mdss_sysfs_mock();

    /* Bind /dev, /proc, /sys into system_a */
    mkdir("/mnt/system_a/dev", 0755);
    mount("/dev", "/mnt/system_a/dev", NULL, MS_BIND | MS_REC, NULL);

    mkdir("/mnt/system_a/proc", 0755);
    mount("/proc", "/mnt/system_a/proc", NULL, MS_BIND | MS_REC, NULL);

    mkdir("/mnt/system_a/sys", 0755);
    mount("/sys", "/mnt/system_a/sys", NULL, MS_BIND | MS_REC, NULL);

    mkdir("/mnt/system_a/sys/fs/selinux", 0755);
    mount("none", "/mnt/system_a/sys/fs/selinux", "selinuxfs", 0, NULL);

    /* Mount configfs for Android USB gadget at /config */
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

    /* Mount in-memory tmpfs for /data (bypasses Qualcomm ICE encryption dependency) */
    mkdir("/mnt/system_a/data", 0771);
    mount("tmpfs", "/mnt/system_a/data", "tmpfs", 0, "mode=0771,uid=1000,gid=1000");
    set_selinux_label("/mnt/system_a/data", "u:object_r:system_data_root_file:s0");

    /* Automated: Install qservice_daemon into /data */
    if (access("/tools/qservice_daemon", F_OK) == 0) {
        copy_file("/tools/qservice_daemon", "/mnt/system_a/data/qservice_daemon");
        chmod("/mnt/system_a/data/qservice_daemon", 0755);
        set_selinux_label("/mnt/system_a/data/qservice_daemon", "u:object_r:system_file:s0");
        usb_write("[+] Automated: Installed /data/qservice_daemon\n");
    }

    /* Automated: Install setup_composer_sysfs.sh into /data */
    if (access("/tools/setup_composer_sysfs.sh", F_OK) == 0) {
        copy_file("/tools/setup_composer_sysfs.sh", "/mnt/system_a/data/setup_composer_sysfs.sh");
        chmod("/mnt/system_a/data/setup_composer_sysfs.sh", 0755);
        set_selinux_label("/mnt/system_a/data/setup_composer_sysfs.sh", "u:object_r:system_file:s0");
        usb_write("[+] Automated: Installed /data/setup_composer_sysfs.sh\n");
    }

    /* Automated: Install libsdm_shim.so and libEGL_adreno_shim.so into /data */
    if (access("/tools/libsdm_shim.so", F_OK) == 0) {
        copy_file("/tools/libsdm_shim.so", "/mnt/system_a/data/libsdm_shim.so");
        chmod("/mnt/system_a/data/libsdm_shim.so", 0755);
        set_selinux_label("/mnt/system_a/data/libsdm_shim.so", "u:object_r:system_file:s0");
        usb_write("[+] Automated: Installed /data/libsdm_shim.so\n");
    }
    if (access("/tools/libEGL_adreno_shim.so", F_OK) == 0) {
        copy_file("/tools/libEGL_adreno_shim.so", "/mnt/system_a/data/libEGL_adreno_shim.so");
        chmod("/mnt/system_a/data/libEGL_adreno_shim.so", 0755);
        set_selinux_label("/mnt/system_a/data/libEGL_adreno_shim.so", "u:object_r:system_file:s0");
        usb_write("[+] Automated: Installed /data/libEGL_adreno_shim.so\n");
    }

    /* Automated: Generate /data/start_graphics.sh helper script */
    FILE *gf = fopen("/mnt/system_a/data/start_graphics.sh", "w");
    if (gf) {
        fprintf(gf, "#!/system/bin/sh\n"
                    "setenforce 0 2>/dev/null\n"
                    "export LD_LIBRARY_PATH=/apex/com.android.i18n/lib64:/apex/com.android.runtime/lib64:/apex/com.android.art/lib64:/system/lib64:/vendor/lib64\n"
                    "chmod 666 /dev/ion 2>/dev/null\n"
                    "chmod 666 /dev/dma_heap/* 2>/dev/null\n"
                    "mkdir -p /dev/graphics 2>/dev/null\n"
                    "[ ! -e /dev/graphics/fb0 ] && [ -e /dev/fb0 ] && ln -sf /dev/fb0 /dev/graphics/fb0\n"
                    "mkdir -p /dev/dri 2>/dev/null\n"
                    "[ ! -e /dev/dri/card0 ] && [ -e /dev/dri/card1 ] && ln -sf /dev/dri/card1 /dev/dri/card0\n"
                    "chmod 666 /dev/dri/* 2>/dev/null\n"
                    "if [ -f /vendor/lib64/egl/libgallium_dri.so ]; then\n"
                    "    echo '[+] Using Mesa Freedreno GLES driver'\n"
                    "elif [ -f /data/libEGL_adreno_shim.so ]; then\n"
                    "    mount -o bind /data/libEGL_adreno_shim.so /vendor/lib64/egl/libEGL_adreno.so 2>/dev/null || true\n"
                    "    mount -o bind /data/libEGL_adreno_shim.so /vendor/lib64/egl/libGLESv2_adreno.so 2>/dev/null || true\n"
                    "fi\n"
                    "killall -9 qservice_daemon 2>/dev/null\n"
                    "if ! ps -A | grep -q vndservicemanager; then\n"
                    "    /vendor/bin/vndservicemanager /dev/vndbinder >/dev/null 2>&1 &\n"
                    "    sleep 0.5\n"
                    "fi\n"
                    "/data/qservice_daemon >/dev/null 2>&1 &\n"
                    "sleep 0.5\n"
                    "stop vendor.hwcomposer-2-1 2>/dev/null\n"
                    "killall -9 android.hardware.graphics.composer@2.1-service 2>/dev/null\n"
                    "setprop debug.gralloc.gfx_ubwc_disable 1 2>/dev/null\n"
                    "setprop debug.gralloc.enable_fb_ubwc 0 2>/dev/null\n"
                    "/vendor/bin/hw/android.hardware.graphics.allocator@2.0-service >/dev/null 2>&1 &\n"
                    "touch /sys/devices/virtual/graphics/fb0/vsync_event 2>/dev/null\n"
                    "chmod 666 /sys/devices/virtual/graphics/fb0/vsync_event 2>/dev/null\n"
                    "echo 200 > /sys/class/backlight/994000.dsi.0/brightness 2>/dev/null || true\n"
                    "if [ -f /data/libsdm_shim.so ]; then\n"
                    "    LD_PRELOAD=/data/libsdm_shim.so /vendor/bin/hw/android.hardware.graphics.composer@2.1-service >/dev/null 2>&1 &\n"
                    "else\n"
                    "    /vendor/bin/hw/android.hardware.graphics.composer@2.1-service >/dev/null 2>&1 &\n"
                    "fi\n"
                    "sleep 0.5\n"
                    "setprop service.sf.prime_shader_cache 0\n"
                    "killall -9 surfaceflinger 2>/dev/null\n"
                    "if [ -f /data/libsdm_shim.so ]; then\n"
                    "    LD_PRELOAD=/data/libsdm_shim.so /system/bin/surfaceflinger >/data/sf.log 2>&1 &\n"
                    "else\n"
                    "    /system/bin/surfaceflinger >/data/sf.log 2>&1 &\n"
                    "fi\n"
                    "sleep 1\n"
                    "setprop ctl.stop bootanim 2>/dev/null\n"
                    "killall -9 bootanimation 2>/dev/null\n"
                    "if [ -f /data/libsdm_shim.so ]; then\n"
                    "    LD_PRELOAD=/data/libsdm_shim.so /system/bin/bootanimation >/dev/null 2>&1 &\n"
                    "else\n"
                    "    /system/bin/bootanimation >/dev/null 2>&1 &\n"
                    "fi\n"
                    "echo '[+] Graphics stack & BootAnimation launched in background!'\n");
        fclose(gf);
        chmod("/mnt/system_a/data/start_graphics.sh", 0755);
        set_selinux_label("/mnt/system_a/data/start_graphics.sh", "u:object_r:system_file:s0");
        usb_write("[+] Automated: Created /data/start_graphics.sh\n");
    }

    /* Mount in-memory tmpfs for cache and metadata */
    mkdir("/mnt/system_a/cache", 0770);
    mount("tmpfs", "/mnt/system_a/cache", "tmpfs", 0, "mode=0770,uid=1000,gid=2001");
    set_selinux_label("/mnt/system_a/cache", "u:object_r:cache_file:s0");

    mkdir("/mnt/system_a/metadata", 0700);
    mount("tmpfs", "/mnt/system_a/metadata", "tmpfs", 0, "mode=0700");
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

    /* Ensure /dev/ion device permissions and SELinux label */
    chmod("/mnt/system_a/dev/ion", 0666);
    set_selinux_label("/mnt/system_a/dev/ion", "u:object_r:ion_device:s0");

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
            "LD_LIBRARY_PATH=/system/lib64:/vendor/lib64:/apex/com.android.runtime/lib64:/apex/com.android.art/lib64:/apex/com.android.i18n/lib64",
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
        if (access("/dev/vndbinder", F_OK) == 0 && access("/vendor/bin/vndservicemanager", F_OK) == 0) {
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
    while (1) {
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
        usb_write(out);
    }
}

static void boot_android_init(const char *stage) {
    if (!stage || !*stage)
        stage = "selinux_setup";

    usb_write("[+] Preparing Android rootfs & filesystems...\n");
    prepare_android_rootfs(false);

    int kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (kfd >= 0)
        lseek(kfd, 0, SEEK_END);

    char msg[192];
    snprintf(msg, sizeof(msg),
             "[+] Launching Android /system/bin/init (%s) in isolated PID 1 namespace...\n"
             "[+] Streaming live /dev/kmsg to serial! (Press Ctrl+C to abort & return to sailfish#)\n\n",
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

            /* Mount fresh /proc for this PID namespace so /proc/1 is Android init */
            umount2("/proc", MNT_DETACH);
            mkdir("/proc", 0555);
            mount("proc", "/proc", "proc", 0, NULL);

            /* Attach stdout/stderr to ttyGS0 so any early linker/init stderr is visible */
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
                "ANDROID_ART_ROOT=/apex/com.android.runtime",
                "ANDROID_I18N_ROOT=/apex/com.android.tzdata",
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

    int status = 0;
    while (1) {
        drain_kmsg_to_usb(kfd);

        pid_t w = waitpid(outer, &status, WNOHANG);
        if (w > 0 || w < 0)
            break;

        if (g_ttygs_fd >= 0) {
            struct pollfd pfd;
            pfd.fd = g_ttygs_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
                char rx[16];
                ssize_t n = read(g_ttygs_fd, rx, sizeof(rx));
                for (ssize_t i = 0; i < n; i++) {
                    if (rx[i] == 0x03) { /* Ctrl+C */
                        usb_write("\n[!] Ctrl+C received; terminating Android init namespace...\n");
                        kill(outer, SIGKILL);
                        waitpid(outer, &status, 0);
                        goto done;
                    }
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

    char exit_msg[160];
    snprintf(exit_msg, sizeof(exit_msg),
             "\n[+] Android /init exited (status=%d) — phone kept alive in sailfish# shell!\n",
             WIFEXITED(status) ? WEXITSTATUS(status) : status);
    usb_write(exit_msg);
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
                  "  display / drm     - Show DRM/KMS connectors, CRTCs, and backlight\n"
                  "  gpu / freedreno   - Test DRI nodes & trigger GPU firmware load\n"
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
    } else if (strcmp(cmd, "display") == 0 || strcmp(cmd, "drm") == 0) {
        dump_display_info();
    } else if (strcmp(cmd, "gpu") == 0 || strcmp(cmd, "freedreno") == 0) {
        test_gpu();
    } else if (strcmp(cmd, "android") == 0 || strcmp(cmd, "aosp") == 0) {
        test_android_primitives();
    } else if (strcmp(cmd, "android_sh") == 0 || strcmp(cmd, "sh") == 0) {
        launch_android_shell();
    } else if (strcmp(cmd, "boot_android") == 0 || strcmp(cmd, "boot") == 0) {
        boot_android_init("selinux_setup");
    } else if (strncmp(cmd, "boot_android ", 13) == 0) {
        const char *st = cmd + 13;
        while (*st == ' ') st++;
        boot_android_init(st);
    } else if (strcmp(cmd, "boot_linux") == 0 || strcmp(cmd, "boot_pmos") == 0 || strcmp(cmd, "pmos") == 0) {
        boot_linux_init();
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

    bool usb_bound = setup_usb_gadget();
    if (!usb_bound)
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
