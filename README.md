# Google Pixel (sailfish) - Android 17 / Linux 6.18 Port

This repository contains the full board support files, device tree source, kernel configurations, drivers, and bring-up toolchain to boot modern **Linux Kernel 6.18 (Android 17 Common Kernel - ACK)** on the first-generation **Google Pixel (`sailfish`)**, powered by the Qualcomm Snapdragon 821 (MSM8996 Pro).

---

## Hardware Support Status

| Component | Hardware / Driver | Status | Notes |
| :--- | :--- | :--- | :--- |
| **CPU / SMP** | Kryo Quad-core (2x 2.15 GHz + 2x 1.59 GHz) | **Working** | All 4 CPUs boot stably via PSCI v1.0 and GICv3 |
| **Interrupt Controller** | Qualcomm MSM8996 GICv3 (608 SPIs, 16 PPIs) | **Working** | Firmware bug workaround enabled |
| **Power Management** | Qualcomm SPMI PMIC Arbiter (`pm8994` + `pmi8994`) | **Working** | Power key (`pwrkey`), Vol- (`resin`), Vol+ (`gpio-keys`), RTC |
| **Watchdog** | Qualcomm APCS Watchdog (`0x09830000`) | **Working** | Early assembly pet in `head.S` + runtime keep-alive |
| **Display** | Samsung AMOLED 1080x1920 (`simpledrm` / MDP5) | **Working** | Tear-free via `PING_PONG_0_DONE` sync & VIG0 re-anchoring |
| **Storage (UFS)** | 32 GB Toshiba UFS 2.0 (`624000.ufshc`) | **Working** | All 6 LUNs enumerated; mounts `system_a` & `vendor_a` (ext4) |
| **USB Peripheral** | DWC3 High-Speed (`6a00000.usb`) ConfigFS | **Working** | High-Speed CDC-ACM serial shell (`18d1:d001`) |
| **Crash Logging** | `pstore` / ramoops (`0x83500000`, 1 MB) | **Working** | `earlyramoops` console & `/sys/fs/pstore` accessible live |
| **Touchscreen** | Synaptics RMI4 S3320 (BLSP I2C) | *In Progress* | BLSP I2C mapping |
| **GPU / KMS** | Adreno 530 + Native MDP5 DSI Host | *Planned* | `ea8064tg` DSI command-mode panel |

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
* **Ramdisk Offset:** `0x02200000` (Load: `0x82200000`)
* **Tags Offset:** `0x02000000` (Load: `0x82000000`)
* **Page Size:** `4096`

---

## Key Fixes & Architecture Highlights

1. **MDP5 MIPI DSI Command-Mode Synchronization:**
   The AMOLED panel on Sailfish runs in DSI command mode fed by MDP5 `SSPP_VIG0` through `LM0` and `PING_PONG_0`. Direct single-buffered blits cause tearing and display glitching. In [`drivers/gpu/drm/sysfb/drm_sysfb_modeset.c`](drivers/gpu/drm/sysfb/drm_sysfb_modeset.c), updates wait on `PING_PONG_0_DONE` (`BIT(8)` in `REG_MDP5_INTR_STATUS`) before blitting, then issue a clean `CTL_0_START` kick.
2. **SSPP_VIG0 Framebuffer Alignment & Color Rotation:**
   Sailfish's `aboot` leaves `VIG0_SRC0_ADDR` pointed at `0xc0001000` (+4096 bytes splash header). Because `4096 = 1 * 3240 + 856`, scanlines wrapped at `x ≈ 795` and `RGB888` rotated by +1 byte (turning Tux's orange feet cyan). The kernel now re-anchors `VIG0` to `0xc0000000` (`1080x1920 RGB888`), ensuring pixel-perfect alignment and colors.
3. **TrustZone XPU Security Boundaries:**
   `gpio0..3` (`blsp_spi1`) are locked by TrustZone for the fingerprint sensor. Querying direction on these registers causes `CNOC ERROR` / `RPM:TZ ABORT!`. A guard in [`drivers/pinctrl/qcom/pinctrl-msm.c`](drivers/pinctrl/qcom/pinctrl-msm.c) skips direction reads on reserved GPIO ranges during chip initialization.
4. **Interactive USB CDC-ACM Shell:**
   [`tools/sailfish/init.c`](tools/sailfish/init.c) binds a high-speed USB CDC-ACM serial console (`18d1:d001`) with non-blocking write draining, live kernel logging (`dmesg [filter]`), `/sys/fs/pstore` inspection (`ramoops`), and fastboot reboot (`bootloader`).

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
- Output: [`out/boot.img`](out/boot.img) (v0 Android boot image ready for fastboot)

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
   - `help` — Show list of commands
   - `dmesg [filter]` — Dump live kernel ringbuffer (optional case-insensitive substring search)
   - `ramoops [filter]` — Inspect previous boot's `/sys/fs/pstore/console-ramoops-0`
   - `mounts` — View active mounts (verifies `/mnt/system_a` and `/mnt/vendor_a`)
   - `parts` — View partitions on UFS storage
   - `cat <file>` — Read system/proc/sysfs files
   - `bootloader` — Reboot directly back to fastboot mode
   - `recovery` — Reboot directly to recovery (e.g. TWRP)


