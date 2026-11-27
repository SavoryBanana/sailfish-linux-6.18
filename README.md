# Google Pixel (`sailfish`) — Linux 6.18 & Android 17

Mainline/ACK **Linux 6.18.32** (`android17-6.18`) board support, device tree, drivers, and native Android 17 AIDL HALs for the **Google Pixel 1 (`sailfish`, Snapdragon 821 / MSM8996 Pro)** running an **Android 17 QPR3 Beta 4 AOSP GSI** over untouched stock Android 10 `vendor_a`.

## Hardware & System Status

| Component | Implementation | Status |
| :--- | :--- | :--- |
| **CPU & Scheduler** | Kryo 4-core (1.59 / 1.90 GHz), `schedutil`, `uclamp`, MGLRU | **Working** |
| **Power & Deep Sleep** | Mainline AIDL `IPower` V7 + ADPF `IPowerHintSession`, `CONFIG_PM_WAKELOCKS` & `PM_AUTOSLEEP` | **Working** |
| **GPU (3D)** | Adreno 530 (133–624 MHz with `vdd_gfx` OPP scaling), signed zap shader, Mesa 25.2.8 Freedreno (`FD_MESA_DEBUG=noubwc`) | **Working** |
| **Display & Composer** | MDP5 v1.7 + Samsung EA8064TG 1080×1920@60Hz AMOLED, native AIDL Composer3 with 6-plane Hybrid/NV12 HW overlays & DRM VBLANK VSYNC | **Working** |
| **Touchscreen** | Synaptics S3708AR (`rmi4_i2c`), multi-touch & pressure | **Working** |
| **Wi-Fi & Bluetooth** | QCA6174A (`ath10k_pci` + `hci_qca`), native AIDL `IBluetoothHci` & `IHostapd` | **Working** |
| **Audio & Microphone** | WCD9335 + TFA989x speaker/earpiece/headphone output, built-in `DMIC2`/`DMIC5` mic, native AIDL `IAudio` | **Working** |
| **Haptics** | PMI8994 SPMI LRA (`qcom-spmi-haptics`), native AIDL `IVibrator` V2 | **Working** |
| **RGB LED & Torch** | PMI8994 LPG RGB (`leds-qcom-lpg`, AIDL `ILights` V2) & dual-channel rear torch (`leds-qcom-pmi8994-flash`, `/sys/class/leds/led:torch_{0,1}`) | **Working** |
| **Battery & Charging** | PMI8994 SMBCHG + Fuel Gauge (`qcom_smbb`), 1.5A USB charging & monotonic SoC/voltage | **Working** |
| **Storage & GMS** | UFS 2.0 (`624000.ufshc`), read-only `/` for ART AOT, zero-repartition GMS `overlayfs` (`/data/local/gms`) | **Working** |
| **Debug & Crash Logs** | DWC3 USB CDC-ACM root shell (`18d1:d001`) + 1 MB `ramoops` (`0x83500000`) | **Working** |

### Known Limitations / In Progress
- **Camera / Quick Settings Flashlight:** Rear torch LEDs work via sysfs; minimal `ICameraProvider` AIDL HAL for the Quick Settings tile is planned.
- **Hardware Video Decode (`qcom-venus`):** Software Codec2 is used (mainline V4L2 Venus requires a `v4l2_codec2` HAL and non-passthrough SMMU domain).
- **Sensors & Fingerprint:** Stub `ISensors` V3 HAL (no SLPI/IIO sensor bridge yet); fingerprint sensor not yet wired.

## Build & Flash

### Prerequisites (Ubuntu / Debian)
```bash
sudo apt install -y build-essential bison flex libssl-dev libelf-dev bc clang lld llvm gcc-aarch64-linux-gnu fastboot picocom
```

### Build `out/boot.img`
```bash
./tools/sailfish/build.sh
```
Builds the static `/init` + initramfs, `sailfish_defconfig` kernel (`LLVM=1`, `CONFIG_WERROR=y`), DTB (`msm8996pro-google-sailfish.dtb`), and v0 `out/boot.img` (must stay `< 32 MiB`).

To rebuild individual native HALs in `tools/sailfish/<hal>/`:
```bash
./tools/sailfish/composer_hal/build.sh
./tools/sailfish/power_hal/build.sh
./tools/sailfish/audio_hal/build.sh
```

### Flash & Console
```bash
# Flash Android 17 GSI once (if not already installed)
fastboot flash system out/system.img

# Boot from RAM or flash to boot partition
fastboot flash boot out/boot.img
fastboot reboot

# Attach to USB CDC-ACM root shell
sudo picocom -b 115200 /dev/ttyACM0
```
