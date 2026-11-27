/*
 * Minimal AIDL android.hardware.sensors.ISensors/default (V3) stub for the
 * Pixel 1 (sailfish) bring-up.
 *
 * The 6.18 kernel has no nanohub driver, so the vendor HIDL
 * android.hardware.sensors@1.0-service is replaced with pause_stub. Without
 * any sensors HAL, SensorDevice in system_server blocks forever waiting for
 * the HIDL service, "sensorservice" never gets published and
 * SystemSensorManager.nativeCreate() trips the Watchdog.
 *
 * SensorDevice tries the AIDL HAL first (AServiceManager_isDeclared +
 * AServiceManager_waitForService). This stub registers ISensors/default and
 * reports an empty sensor list, so SensorService initialises with 0 sensors.
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;

typedef void *(*AIBinder_Class_onCreate)(void *args);
typedef void (*AIBinder_Class_onDestroy)(void *userData);
typedef int32_t (*AIBinder_Class_onTransact)(AIBinder *binder, uint32_t code,
                                             const AParcel *in, AParcel *out);

/* Transaction codes from BpSensors in android.hardware.sensors-V3-ndk.so */
enum {
    TX_ACTIVATE = 1,
    TX_BATCH = 2,
    TX_CONFIG_DIRECT_REPORT = 3,
    TX_FLUSH = 4,
    TX_GET_SENSORS_LIST = 5,
    TX_INITIALIZE = 6,
    TX_INJECT_SENSOR_DATA = 7,
    TX_REGISTER_DIRECT_CHANNEL = 8,
    TX_SET_OPERATION_MODE = 9,
    TX_UNREGISTER_DIRECT_CHANNEL = 10,
    TX_GET_INTERFACE_HASH = 16777214,
    TX_GET_INTERFACE_VERSION = 16777215,
};

enum {
    STATUS_OK_VAL = 0,
    STATUS_UNKNOWN_TX_VAL = -EBADMSG,
    EX_UNSUPPORTED_OP_VAL = -7,
};

static const char ISENSORS_DESC[] = "android.hardware.sensors.ISensors";
static const char ISENSORS_INSTANCE[] = "android.hardware.sensors.ISensors/default";
static const char ISENSORS_HASH[] = "b051523be857524c99057b69f253a7d0bd295400";

static struct {
    AIBinder_Class *(*Class_define)(const char *, AIBinder_Class_onCreate,
                                    AIBinder_Class_onDestroy,
                                    AIBinder_Class_onTransact);
    AIBinder *(*AIBinder_new)(const AIBinder_Class *, void *);
    void (*markVintfStability)(AIBinder *);
    int32_t (*addService)(AIBinder *, const char *);
    bool (*setThreadPoolMaxThreadCount)(uint32_t);
    void (*startThreadPool)(void);
    void (*joinThreadPool)(void);
    AStatus *(*AStatus_newOk)(void);
    AStatus *(*AStatus_fromExceptionCode)(int32_t);
    void (*AStatus_delete)(AStatus *);
    int32_t (*writeStatusHeader)(AParcel *, const AStatus *);
    int32_t (*writeInt32)(AParcel *, int32_t);
    int32_t (*writeString)(AParcel *, const char *, int32_t);
} g_ndk;

static void kmsg_log(const char *msg) {
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t len = 0;
        while (msg[len] != '\0') len++;
        (void)write(fd, msg, len);
        close(fd);
    }
}

static bool resolve_ndk(void *lib) {
    const struct {
        const char *name;
        void **slot;
    } syms[] = {
        { "AIBinder_Class_define", (void **)&g_ndk.Class_define },
        { "AIBinder_new", (void **)&g_ndk.AIBinder_new },
        { "AIBinder_markVintfStability", (void **)&g_ndk.markVintfStability },
        { "AServiceManager_addService", (void **)&g_ndk.addService },
        { "ABinderProcess_setThreadPoolMaxThreadCount", (void **)&g_ndk.setThreadPoolMaxThreadCount },
        { "ABinderProcess_startThreadPool", (void **)&g_ndk.startThreadPool },
        { "ABinderProcess_joinThreadPool", (void **)&g_ndk.joinThreadPool },
        { "AStatus_newOk", (void **)&g_ndk.AStatus_newOk },
        { "AStatus_fromExceptionCode", (void **)&g_ndk.AStatus_fromExceptionCode },
        { "AStatus_delete", (void **)&g_ndk.AStatus_delete },
        { "AParcel_writeStatusHeader", (void **)&g_ndk.writeStatusHeader },
        { "AParcel_writeInt32", (void **)&g_ndk.writeInt32 },
        { "AParcel_writeString", (void **)&g_ndk.writeString },
    };

    for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        *syms[i].slot = dlsym(lib, syms[i].name);
        if (!*syms[i].slot) {
            return false;
        }
    }
    return true;
}

static int32_t write_status(AParcel *out, AStatus *status) {
    if (!status) return -ENOMEM;
    int32_t rc = g_ndk.writeStatusHeader(out, status);
    g_ndk.AStatus_delete(status);
    return rc;
}

static void *isensors_on_create(void *args) {
    return args;
}

static void isensors_on_destroy(void *user_data) {
    (void)user_data;
}

static int32_t isensors_on_transact(AIBinder *binder, uint32_t code,
                                    const AParcel *in, AParcel *out) {
    (void)binder;
    (void)in;
    int32_t rc;

    switch (code) {
    /* void methods (none of ISensors is oneway): reply with an OK status */
    case TX_ACTIVATE:
    case TX_BATCH:
    case TX_FLUSH:
    case TX_INITIALIZE:          /* FMQ descriptors + callback are ignored */
    case TX_INJECT_SENSOR_DATA:
    case TX_SET_OPERATION_MODE:
    case TX_UNREGISTER_DIRECT_CHANNEL:
        return write_status(out, g_ndk.AStatus_newOk());

    case TX_GET_SENSORS_LIST:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 0); /* SensorInfo[] of length 0 */

    case TX_CONFIG_DIRECT_REPORT:
    case TX_REGISTER_DIRECT_CHANNEL:
        return write_status(out, g_ndk.AStatus_fromExceptionCode(EX_UNSUPPORTED_OP_VAL));

    case TX_GET_INTERFACE_HASH:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeString(out, ISENSORS_HASH, (int32_t)(sizeof(ISENSORS_HASH) - 1));

    case TX_GET_INTERFACE_VERSION:
        rc = write_status(out, g_ndk.AStatus_newOk());
        if (rc != STATUS_OK_VAL) return rc;
        return g_ndk.writeInt32(out, 3);

    default:
        return STATUS_UNKNOWN_TX_VAL;
    }
}

static uint32_t nanohub_crc32(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xffffffffU;
    size_t i = 0;
    while (i < len) {
        uint32_t w = 0;
        for (int b = 0; b < 4 && i < len; b++, i++) {
            w |= ((uint32_t)buf[i]) << (b * 8);
        }
        crc ^= w;
        for (int bit = 0; bit < 32; bit++) {
            crc = (crc & 0x80000000U) ? ((crc << 1) ^ 0x04c11db7U) : (crc << 1);
        }
    }
    return crc;
}

static int find_tlmm_gpiochip(void) {
    for (int i = 0; i < 10; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/gpiochip%d", i);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct gpiochip_info info;
        memset(&info, 0, sizeof(info));
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 && info.lines >= 140) {
            return fd;
        }
        close(fd);
    }
    return -1;
}

static int req_gpio_out(int chip_fd, uint32_t offset, uint8_t val) {
    struct gpiohandle_request req;
    memset(&req, 0, sizeof(req));
    req.lineoffsets[0] = offset;
    req.flags = GPIOHANDLE_REQUEST_OUTPUT;
    req.default_values[0] = val;
    req.lines = 1;
    snprintf(req.consumer_label, sizeof(req.consumer_label), "nanohub");
    if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0)
        return -1;
    return req.fd;
}

static void set_gpio_val(int line_fd, uint8_t val) {
    if (line_fd < 0)
        return;
    struct gpiohandle_data data;
    memset(&data, 0, sizeof(data));
    data.values[0] = val;
    (void)ioctl(line_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
}

static __attribute__((unused)) int req_gpio_in(int chip_fd, uint32_t offset) {
    struct gpiohandle_request req;
    memset(&req, 0, sizeof(req));
    req.lineoffsets[0] = offset;
    req.flags = GPIOHANDLE_REQUEST_INPUT;
    req.lines = 1;
    snprintf(req.consumer_label, sizeof(req.consumer_label), "nanohub_in");
    if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0)
        return -1;
    return req.fd;
}

static __attribute__((unused)) int get_gpio_val(int line_fd) {
    if (line_fd < 0)
        return -1;
    struct gpiohandle_data data;
    memset(&data, 0, sizeof(data));
    if (ioctl(line_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) < 0)
        return -1;
    return (int)data.values[0];
}

static void log_hex_msg(const char *tag, const uint8_t *buf, int len) {
    char hex[384];
    int pos = snprintf(hex, sizeof(hex), "<6>sailfish_sensors_hal: %s len=%d:", tag, len);
    for (int i = 0; i < len && i < 48 && pos < (int)sizeof(hex) - 4; i++) {
        pos += snprintf(hex + pos, sizeof(hex) - (size_t)pos, " %02x", buf[i]);
    }
    snprintf(hex + pos, sizeof(hex) - (size_t)pos, "\n");
    kmsg_log(hex);
}

static int nanohub_xfer(int spi_fd, uint32_t seq, const uint8_t *payload,
                        uint8_t plen, uint8_t *rx_out, size_t rx_max) {
    uint8_t tx[260];
    size_t padded = (plen + 3U) & ~3U;
    size_t hdr_and_data = 6U + padded;
    memset(tx, 0x00, hdr_and_data + 4U);
    tx[0] = 0x31;
    tx[1] = (uint8_t)(seq & 0xffU);
    tx[2] = (uint8_t)((seq >> 8) & 0xffU);
    tx[3] = (uint8_t)((seq >> 16) & 0xffU);
    tx[4] = (uint8_t)((seq >> 24) & 0xffU);
    tx[5] = plen;
    if (plen > 0 && payload)
        memcpy(tx + 6, payload, plen);
    uint32_t crc = nanohub_crc32(tx, hdr_and_data);
    tx[hdr_and_data + 0] = (uint8_t)(crc & 0xffU);
    tx[hdr_and_data + 1] = (uint8_t)((crc >> 8) & 0xffU);
    tx[hdr_and_data + 2] = (uint8_t)((crc >> 16) & 0xffU);
    tx[hdr_and_data + 3] = (uint8_t)((crc >> 24) & 0xffU);
    tx[hdr_and_data + 4] = 0xff;
    tx[hdr_and_data + 5] = 0xff;
    tx[hdr_and_data + 6] = 0xff;
    tx[hdr_and_data + 7] = 0xff;

    uint8_t dummy_rx[260];
    memset(dummy_rx, 0xff, sizeof(dummy_rx));
    struct spi_ioc_transfer xfer1 = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)dummy_rx,
        .len = (uint32_t)(hdr_and_data + 4U),
        .speed_hz = 4800000,
        .bits_per_word = 8,
    };
    if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &xfer1) < 0)
        return -1;

    uint8_t poll_tx[320];
    uint8_t poll_rx[320];
    memset(poll_tx, 0xff, sizeof(poll_tx));
    size_t rlen = sizeof(poll_rx);
    int last_copy = 0;
    for (int t = 0; t < 8; t++) {
        usleep(4000);
        memset(poll_rx, 0xff, rlen);
        struct spi_ioc_transfer xfer2 = {
            .tx_buf = (unsigned long)poll_tx,
            .rx_buf = (unsigned long)poll_rx,
            .len = (uint32_t)rlen,
            .speed_hz = 4800000,
            .bits_per_word = 8,
        };
        if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &xfer2) <= 0)
            continue;
        for (size_t i = 0; i + 6 < rlen; i++) {
            if (poll_rx[i] == 0x31) {
                uint8_t pkt_len = poll_rx[i + 5];
                size_t avail = rlen - i;
                size_t copy = avail < rx_max ? avail : rx_max;
                memcpy(rx_out, poll_rx + i, copy);
                last_copy = (int)copy;
                if (pkt_len > 1)
                    return last_copy;
                char tag[32];
                snprintf(tag, sizeof(tag), "poll_t%d_i%zu", t, i);
                log_hex_msg(tag, poll_rx + i, 16);
                i += 13;
            }
        }
    }
    return last_copy;
}

static void nanohub_spi_probe(void) {
    int chip_fd = find_tlmm_gpiochip();
    int spi_fd = open("/dev/spidev0.0", O_RDWR | O_CLOEXEC);
    if (chip_fd < 0 || spi_fd < 0) {
        if (chip_fd >= 0) close(chip_fd);
        if (spi_fd >= 0) close(spi_fd);
        return;
    }

    int boot0_fd = req_gpio_out(chip_fd, 130, 0);
    int nreset_fd = req_gpio_out(chip_fd, 38, 0);
    int wakeup_fd = req_gpio_out(chip_fd, 24, 1);
    int irq1_fd = req_gpio_in(chip_fd, 123);
    int irq2_fd = req_gpio_in(chip_fd, 117);
    usleep(10000);
    set_gpio_val(nreset_fd, 1);
    usleep(1500000);

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = 4800000;
    (void)ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);
    (void)ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    (void)ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    char msg[128];
    snprintf(msg, sizeof(msg), "<6>sailfish_sensors_hal: pre1500 irq1=%d irq2=%d\n",
             get_gpio_val(irq1_fd), get_gpio_val(irq2_fd));
    kmsg_log(msg);

    uint8_t rx[96];
    uint8_t cmd_os[4] = { 0x00, 0x10, 0x00, 0x00 }; /* CMD_COMMS_GET_OS_HW_VERSIONS */
    set_gpio_val(wakeup_fd, 0);
    usleep(10000);
    int n = nanohub_xfer(spi_fd, 1, cmd_os, sizeof(cmd_os), rx, sizeof(rx));
    set_gpio_val(wakeup_fd, 1);
    log_hex_msg("os_1500", rx, n);

    usleep(10000);
    uint8_t cmd_rd[8] = { 0x80, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; /* CMD_COMMS_READ */
    set_gpio_val(wakeup_fd, 0);
    usleep(10000);
    n = nanohub_xfer(spi_fd, 2, cmd_rd, sizeof(cmd_rd), rx, sizeof(rx));
    set_gpio_val(wakeup_fd, 1);
    log_hex_msg("rd_1500", rx, n);

    snprintf(msg, sizeof(msg), "<6>sailfish_sensors_hal: post irq1=%d irq2=%d\n",
             get_gpio_val(irq1_fd), get_gpio_val(irq2_fd));
    kmsg_log(msg);
    snprintf(msg, sizeof(msg), "<6>sailfish_sensors_hal: end irq1=%d irq2=%d\n",
             get_gpio_val(irq1_fd), get_gpio_val(irq2_fd));
    kmsg_log(msg);

    if (irq2_fd >= 0) close(irq2_fd);
    if (irq1_fd >= 0) close(irq1_fd);
    if (wakeup_fd >= 0) close(wakeup_fd);
    if (nreset_fd >= 0) close(nreset_fd);
    if (boot0_fd >= 0) close(boot0_fd);
    close(spi_fd);
    close(chip_fd);
}

int main(void) {
    void *lib = dlopen("/system/lib64/libbinder_ndk.so", RTLD_NOW);
    if (!lib || !resolve_ndk(lib)) {
        kmsg_log("<3>sailfish_sensors_hal: failed to load libbinder_ndk.so symbols\n");
        for (;;) pause();
    }

    AIBinder_Class *clazz = g_ndk.Class_define(ISENSORS_DESC, isensors_on_create,
                                               isensors_on_destroy, isensors_on_transact);
    AIBinder *binder = clazz ? g_ndk.AIBinder_new(clazz, NULL) : NULL;
    if (!binder) {
        kmsg_log("<3>sailfish_sensors_hal: AIBinder_new failed\n");
        for (;;) pause();
    }

    g_ndk.markVintfStability(binder);

    while (g_ndk.addService(binder, ISENSORS_INSTANCE) != STATUS_OK_VAL) {
        usleep(250000);
    }
    kmsg_log("<6>sailfish_sensors_hal: registered android.hardware.sensors.ISensors/default (V3 stub, 0 sensors)\n");

    nanohub_spi_probe();

    g_ndk.setThreadPoolMaxThreadCount(4);
    g_ndk.startThreadPool();
    g_ndk.joinThreadPool();

    for (;;) pause();
    return 0;
}
