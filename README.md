# Google Pixel (sailfish) - Android 17 / Linux 6.18 Port

This repository contains the board support files, device tree source, kernel configurations, and bring-up toolchain to boot modern **Linux Kernel 6.18 (Android 17 Common Kernel - ACK)** on the first-generation **Google Pixel (`sailfish`)**, powered by the Qualcomm Snapdragon 821 (MSM8996 Pro).

---

## Hardware Specifications & Offsets

* **SoC:** Qualcomm MSM8996 Pro (Snapdragon 821, Kryo quad-core)
* **Qualcomm Identifiers:** `qcom,msm-id = <420 0x10001>, <421 0x10001>;`
* **Board Identifiers:** `qcom,board-id = <0x80 0>, <3 0>;` (Rev A & XD)
* **Early Framebuffer:** HTC Continuous Splash Buffer at `0xc0001000` (1080x1920, 32bpp) via `simple-framebuffer` (`simpledrm`).
* **Crash Logging:** Ramoops / pstore RAM console buffer at `0x83500000` (size `0x100000`).
* **Touchscreen:** Synaptics DSX (`synaptics_dsx@20` on I2C bus 3, IRQ GPIO 125, Reset GPIO 89).
* **Storage:** Qualcomm UFS 2.0 (`ufshc@624000`).
* **USB:** DWC3 peripheral mode (`usb3_dwc3`).

### Bootloader Offsets (Header v0)
* **Base:** `0x80000000`
* **Kernel Offset:** `0x00080000` (Load: `0x80080000`)
* **Ramdisk Offset:** `0x02200000` (Load: `0x82200000`)
* **Tags Offset:** `0x02000000` (Load: `0x82000000`)
* **Page Size:** `4096`

---

## Repository Structure

```
├── configs/
│   └── sailfish_early.fragment   # Kconfig fragment for essential early boot drivers
├── dts/
│   └── msm8996pro-google-sailfish.dts # Upstream-compatible Device Tree Source
├── patches/
│   └── 0001-arm64-dts-qcom-add-Google-Pixel-sailfish-device-tree.patch # Patch against ACK android17-6.18
├── src/
│   └── init.c                    # Minimal static aarch64 diagnostic /init program
├── tools/
│   ├── build.sh                  # Automated kernel build & boot.img packaging script
│   ├── test_boot.sh              # Fastboot one-shot RAM boot helper
│   ├── mkbootimg.py              # Android v0 boot image creator
│   └── unpack_bootimg.py          # Android boot image inspector
├── .gitignore                    # Excludes heavy kernel source tree and build outputs
└── README.md
```

---

## Prerequisites

On Ubuntu / Debian:
```bash
sudo apt update
sudo apt install -y build-essential bison flex libssl-dev libelf-dev bc clang lld llvm gcc-aarch64-linux-gnu fastboot
```

---

## Building

To build the kernel `Image.gz`, compile the DTB, build the minimal test ramdisk, and package `out/boot.img`:

```bash
./tools/sailfish/build.sh
```

The script will automatically shallow-clone `android17-6.18` from Google's Android Common Kernel if not already present, apply the device tree and configs, compile with Clang 22 (`LLVM=1`), and produce:
* `out/boot.img` (Android header v0 boot image ready for fastboot)

---

## Testing on Device (Fastboot)

This method tests booting directly from RAM without modifying your existing partitions:

1. Connect your Pixel via USB-C.
2. Put the device into Fastboot mode:
   * Power off, then hold **Volume Down + Power**.
3. Run:
   ```bash
   ./tools/sailfish/test_boot.sh
   ```
   *(Or run manually: `fastboot boot out/boot.img`)*

4. **Observe:**
   * **Screen output:** Boot messages should appear on the AMOLED display via the simple framebuffer.
   * **Crash diagnostics:** If the device reboots, dump the ramoops console log using:
     ```bash
     fastboot oem ramoops
     ```

