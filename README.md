# Google Pixel (sailfish) - Android 17 / Linux 6.18 Port

This repository contains the complete board support package, device tree source, kernel configurations, drivers, and bring-up toolchain to boot modern **Linux Kernel 6.18 (Android 17 Common Kernel - ACK `android17-6.18`)** on the first-generation **Google Pixel (`sailfish`)**, powered by the Qualcomm Snapdragon 821 (MSM8996 Pro).

---

## Hardware Support Status

| Component | Hardware / Driver | Status | Notes |
| :--- | :--- | :--- | :--- |
| **CPU / SMP** | Kryo Quad-core (2x 2.15 GHz + 2x 1.59 GHz) | **Working** | All 4 CPUs online stably via PSCI v1.0 and GICv3 |
| **Interrupt Controller** | Qualcomm MSM8996 GICv3 (608 SPIs, 16 PPIs) | **Working** | Firmware quirk workaround enabled |
| **Power Management** | Qualcomm SPMI PMIC Arbiter (`pm8994` + `pmi8994`) | **Working** | Power key (`pwrkey`), Vol- (`resin`), Vol+ (`gpio-keys`), RTC |
| **RPM Power Rails** | Qualcomm SMD RPM Regulators (`qcom,rpm-pm8994-regulators`) | **Working** | S4, L6, L14, L17, L18, L19 (2.85V), L20, L21, L22, L23, L24, L25, L28, L29 (3.0V), L30, LVS1, LVS2 |
| **Touchscreen** | Synaptics S3708AR (`synaptics_dsxv26`) via RMI4 I2C | **Working** | Multi-touch digitizer (`/dev/input/event2`); real-time X/Y tracking & pressure |
| **Watchdog** | Qualcomm APCS Watchdog (`0x09830000`) | **Working** | Early assembly pet in `head.S` + runtime keep-alive |
| **Display** | Samsung AMOLED 1080x1920 (`simpledrm` / MDP5) | **Working** | Tear-free via `PING_PONG_0_DONE` sync & VIG0 re-anchoring |
| **Storage (UFS)** | 32 GB Toshiba UFS 2.0 (`624000.ufshc`) | **Working** | All 6 LUNs enumerated; mounts `system_a` & `vendor_a` (ext4) |
| **USB Peripheral** | DWC3 High-Speed (`6a00000.usb`) ConfigFS | **Working** | Interactive high-speed CDC-ACM serial shell (`18d1:d001`) |
| **Crash Logging** | `pstore` / ramoops (`0x83500000`, 1 MB) | **Working** | `earlyramoops` console & `/sys/fs/pstore` accessible live |
| **Battery & Charging** | PMI8994 Switch-Mode Charger & Fuel Gauge | **Working** | 1.5A fast charging, USB status, monotonic SoC & voltage reporting |
| **GPU / KMS** | Adreno 530 + Native MDP5 DSI Host | **Working** | Native DRM `panel-samsung-ea8064tg` driver + Freedreno Adreno 530 GPU |

---

## Hardware Specifications & Memory Map

* **SoC:** Qualcomm MSM8996 Pro (Snapdragon 821)
* **Qualcomm Identifiers:** `qcom,msm-id = <0x1a4 0x10001>, <0x1a5 0x10001>;` (420 / 421)
* **Board Identifiers:** `qcom,board-id = <0x80 0>, <3 0>;`
* **RAM:** 4 GB LPDDR4 (`0x80000000` - `0x17e54ffff`)
* **Display Framebuffer:** `0xc0000000` (1080x1920, 24bpp `r8g8b8`, stride 3240 bytes)
* **Ramoops (pstore):** `0x83500000` (1 MB: 512 KB console, 256 KB record, 256 KB pmsg)
* **Storage:** Toshiba UFS 2.0 (`/dev/sda33` = `system_a`, `/dev/sda31` = `vendor_a`, `/dev/sda35` = `userdata`)

### Android Boot Image Offsets (Header v0)
* **Base:** `0x80000000`
* **Kernel Offset:** `0x00080000` (Load: `0x80080000`)
* **Ramdisk Offset:** `0x02800000` (Load: `0x82800000`)
* **Tags Offset:** `0x02600000` (Load: `0x82600000`)
* **Page Size:** `4096`

---

## Key Fixes & Architecture Highlights

1. **Synaptics S3708AR Multi-Touch Digitizer & PM8994 Power Architecture:**
   - **Factory Device Tree Extraction:** Dumped the complete 388,547-byte stock Android DTB directly from UFS partition `boot_a` (`/dev/sda19`) over USB CDC-ACM and decompiled it to [`tools/sailfish/stock_sailfish.dts`](file:///home/patrick-donghil/Desktop/Kernel%20Project/tools/sailfish/stock_sailfish.dts).
   - **Power Rail Restoration:** Identified the Samsung EA8064TG AMOLED On-Cell digitizer power routing:
     - **Analog Power (`vdd-supply`):** PM8994 `L29` configured for **3.0V** (`regulator-always-on`).
     - **Digital Logic & Pull-up (`vio-supply`):** PM8994 `LVS2` configured for **1.8V** (switched from `S4`, `regulator-always-on`).
     - Without `LVS2`, the discrete board-level I2C pull-up resistors on `gpio47` (SDA) and `gpio48` (SCL) remained unpowered at 0V, causing the chip's internal protection diodes to clamp the BLSP1 QUP3 bus.
   - **Subsystem Multiplexing & Reset:**
     - `gpio80` (`switch-gpios`) is actively driven to `0` (AP mode), routing the I2C bus and interrupt line from the Sensor Subsystem (SLPI) to the Kryo Application Processor.
     - `gpio89` (`reset-gpios`) is actively pulsed low for 5 ms, followed by a 100 ms firmware initialization window before initial bus transactions.
   - **Live Telemetry:** The controller enumerates as `Synaptics S3708AR` (`product: S3708AR, fw id: 2433782`) on `/dev/input/event2` and delivers multi-touch coordinates (`ABS_MT_POSITION_X`, `ABS_MT_POSITION_Y`, `ABS_MT_PRESSURE`, `BTN_TOUCH`).

2. **MDP5 MIPI DSI Command-Mode Synchronization:**
   - The AMOLED panel on Sailfish runs in DSI command mode fed by MDP5 `SSPP_VIG0` through `LM0` and `PING_PONG_0`. Direct single-buffered blits cause tearing and display glitching.
   - In [`drivers/gpu/drm/sysfb/drm_sysfb_modeset.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/drivers/gpu/drm/sysfb/drm_sysfb_modeset.c), updates synchronize with `PING_PONG_0_DONE` (`BIT(8)` in `REG_MDP5_INTR_STATUS`) before blitting, then issue a clean `CTL_0_START` kick.

3. **SSPP_VIG0 Framebuffer Alignment & Color Rotation:**
   - Sailfish's bootloader leaves `VIG0_SRC0_ADDR` pointed at `0xc0001000` (+4096 bytes splash header). Because `4096 = 1 * 3240 + 856`, scanlines wrapped at `x ≈ 795` and `RGB888` rotated by +1 byte (turning Tux's orange feet cyan).
   - The kernel re-anchors `VIG0` to `0xc0000000` (`1080x1920 RGB888`), ensuring pixel-perfect alignment and colors.

4. **TrustZone XPU Security Boundaries:**
   - `gpio0..3` (`blsp_spi1`) are locked by TrustZone for the fingerprint sensor. Querying direction on these registers causes `CNOC ERROR` / `RPM:TZ ABORT!`.
   - A guard in [`drivers/pinctrl/qcom/pinctrl-msm.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/drivers/pinctrl/qcom/pinctrl-msm.c) skips direction reads on reserved GPIO ranges during chip initialization.

5. **Battery Management & PMI8994 Switch-Mode Charger:**
   - Supported via [`drivers/power/supply/qcom_smbb.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/drivers/power/supply/qcom_smbb.c) using compatible `"qcom,pmi8994-charger"` at SPMI USID 2 base `0x1000`.
   - **Fuel Gauge Monotonic State of Charge:** Directly reads real-time battery percentage from PMI8994 `FG_SOC` (register `0x4009`) and battery voltage from `FG_BATT` (registers `0x4150` / `0x4151`), exposing `/sys/class/power_supply/battery/capacity` and `/sys/class/power_supply/battery/voltage_now`.
   - **Continuous USB Charging:** The driver clears and disables the charger hardware watchdog and enumeration timer, and raises the USB input current limit up to 1.5A/2.5A to ensure the battery remains charged while running Linux workloads.

6. **Native DRM KMS Display Engine & Freedreno Adreno 530 GPU:**
   - **Samsung EA8064TG AMOLED Panel Driver:** Authored [`drivers/gpu/drm/panel/panel-samsung-ea8064tg.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/drivers/gpu/drm/panel/panel-samsung-ea8064tg.c) driving the 5.0" 1080x1920 command-mode DSI display with 4 lanes at 60Hz.
   - **Power Routing & Reset:** Powered from PM8994 `LVS2` (1.8V VDDIO) and `L29` (3.0V VCI), with active-low hardware reset on `gpio39` and tearing-effect (TE) sync on `gpio10` (`mdp_vsync`).
   - **Freedreno Adreno 530 GPU:** Enabled `&gpu` and `&adreno_smmu` with `a530_zap.mbn` TrustZone microcode loading from `/vendor/firmware/` via `firmware_class.path`.

7. **Interactive USB CDC-ACM Diagnostic Shell:**
   - [`tools/sailfish/init.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/tools/sailfish/init.c) binds a high-speed USB CDC-ACM serial console (`18d1:d001`) with commands:
     - `battery` / `power`: inspect battery SoC %, voltage, health, and charger state.
     - `display` / `drm`: inspect DRM KMS connectors, CRTCs, and backlight brightness.
     - `input` / `touch`: test live multi-touch events and pressure readings.
     - `dmesg [filter]`: search live kernel log.
     - `ramoops`: inspect `/sys/fs/pstore`.
     - `bootloader`: reboot directly to fastboot.

---

## Building

### Prerequisites (Ubuntu / Debian)
```bash
sudo apt update
sudo apt install -y build-essential bison flex libssl-dev libelf-dev bc clang lld llvm gcc-aarch64-linux-gnu fastboot picocom
```

### Compile Kernel & Package `boot.img`
Run the automated build script:
```bash
bash ./tools/sailfish/build.sh
```

This compiles:
- The static aarch64 diagnostic `/init` program and initramfs
- The modern kernel image with Clang/LLVM (`arch/arm64/boot/Image`)
- The appended Device Tree Blob (`arch/arm64/boot/dts/qcom/msm8996pro-google-sailfish.dtb`)
- Output: [`out/boot.img`](file:///home/patrick-donghil/Desktop/Kernel%20Project/out/boot.img) (v0 Android boot image ready for fastboot)

---

## Testing on Device (Fastboot)

Boot directly from host RAM without overwriting your device's flash:

1. Put the Pixel into Fastboot mode (**Volume Down + Power**).
2. Boot the image:
   ```bash
   fastboot boot out/boot.img
   ```
3. Connect to the interactive USB diagnostic shell:
   ```bash
   sudo picocom -b 115200 /dev/ttyACM0
   ```
4. Available commands in `sailfish# `:
   - `help` — Show list of available commands
   - `input` / `touch` — Monitor live multi-touch coordinates & hardware button events for 10s
   - `i2c` / `i2c_scan` — Scan `/dev/i2c-0` for responsive slave addresses
   - `touch_test` — Run low-level GPIO switch/reset sweep and I2C ACK diagnostics
   - `dtb` / `stock_dtb` — Scan stock `boot_a` partition for factory DTB and touch node
   - `dump_dtb` — Dump the entire factory DTB as base64 over serial
   - `dmesg [filter]` — Dump live kernel ringbuffer (optional case-insensitive substring search)
   - `ramoops [filter]` — Inspect previous boot's `/sys/fs/pstore/console-ramoops-0`
   - `mounts` — View active mounts (verifies `/mnt/system_a` and `/mnt/vendor_a`)
   - `parts` — View partitions on UFS storage
   - `cat <file>` — Read system/proc/sysfs files
   - `bootloader` — Reboot directly back to fastboot mode
   - `recovery` — Reboot directly to recovery (e.g. TWRP)
   - `reboot` — Normal reboot
