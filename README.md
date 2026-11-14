# Google Pixel (sailfish) - Android 17 / Linux 6.18 Port

This repository contains the complete board support package, device tree source, kernel configurations, drivers, and bring-up toolchain to boot modern **Linux Kernel 6.18 (Android 17 Common Kernel - ACK `android17-6.18`)** and run **Android 17 (AOSP GSI)** on the first-generation **Google Pixel (`sailfish`)**, powered by the Qualcomm Snapdragon 821 (MSM8996 Pro).

---

## Hardware & System Support Status

| Component | Hardware / Driver | Status | Notes |
| :--- | :--- | :--- | :--- |
| **CPU / SMP** | Kryo Quad-core (2x 2.15 GHz + 2x 1.59 GHz) | **Working** | All 4 CPUs online stably via PSCI v1.0 and GICv3 |
| **Interrupt Controller** | Qualcomm MSM8996 GICv3 (608 SPIs, 16 PPIs) | **Working** | Firmware quirk workaround enabled |
| **Power Management** | Qualcomm SPMI PMIC Arbiter (`pm8994` + `pmi8994`) | **Working** | Power key (`pwrkey`), Vol- (`resin`), Vol+ (`gpio-keys`), RTC |
| **RPM Power Rails** | Qualcomm SMD RPM Regulators (`qcom,rpm-pm8994-regulators`) | **Working** | S4, L6, L14, L17, L18, L19 (2.85V), L20, L21, L22, L23, L24, L25, L28, L29 (3.0V), L30, LVS1, LVS2 |
| **Touchscreen** | Synaptics S3708AR (`synaptics_dsxv26`) via RMI4 I2C | **Working** | Multi-touch digitizer (`/dev/input/event2`); real-time X/Y tracking & pressure |
| **Watchdog** | Qualcomm APCS Watchdog (`0x09830000`) | **Working** | Early assembly pet in `head.S` + runtime keep-alive |
| **Display & KMS** | Native MSM MDP5 v1.7 KMS + DSI Host + Samsung EA8064TG AMOLED | **Working** | Native KMS modesetting (`card1-DSI-1` at 1080x1920@60Hz), fbcon (4 Tux penguins & boot log), DCS backlight |
| **Storage (UFS)** | 32 GB Toshiba UFS 2.0 (`624000.ufshc`) | **Working** | All 6 LUNs enumerated; mounts `system_a` & `vendor_a` (ext4) |
| **USB Peripheral** | DWC3 High-Speed (`6a00000.usb`) ConfigFS | **Working** | Interactive high-speed CDC-ACM serial shell (`18d1:d001`) |
| **Crash Logging** | `pstore` / ramoops (`0x83500000`, 1 MB) | **Working** | `earlyramoops` console & `/sys/fs/pstore` accessible live |
| **Battery & Charging** | PMI8994 Switch-Mode Charger & Fuel Gauge | **Working** | 1.5A fast charging, USB status, monotonic SoC & voltage reporting |
| **GPU (3D)** | Freedreno Adreno 530 (`&gpu`, `&adreno_smmu`) | **Working** | Authentic OEM signed zap shader authenticated via TrustZone PAS; single-ring (`ring=0`) submission at 401.8 MHz; `/dev/dri/card1` & `/dev/dri/renderD128` active |
| **Android 17 Userspace** | Android 17 AOSP GSI (`DP11.261016.003`) | **Working** | `sys.boot_completed=1`; `init`, `apexd`, `zygote64`, `system_server`, `SurfaceFlinger`, `SystemUI`, and `QuickstepLauncher` homescreen running with hardware 3D acceleration |
| **Mesa 25.2.8 Freedreno** | `libEGL_adreno.so`, `libGLESv2_adreno.so`, `libgallium_dri.so` | **Working** | Mainline Mesa 25.2.8 `gallium/freedreno` (`msm` KMD) powering Skia Ganesh GLES in `SurfaceFlinger`, `BootAnimation`, `SystemUI`, and `Launcher3` |
| **Gralloc & Adreno Utils** | Open-source `libadreno_utils.so` + `ion_compat` DMA-BUF heap | **Working** | 64-pixel width (`1088` stride) & 32-pixel height alignment for Adreno 530 GMEM; `/dev/ion` compatibility bridge over `system-uncached` DMA-BUF heap |
| **VINTF Compatibility** | Custom HIDL & AIDL Manifests (`graphics`, `power`, `sensors`) | **Working** | Declares `composer@2.1`, `allocator@2.0`, AIDL `IPower` V7, and `ISensors` V3, satisfying Android 17 `hwservicemanager` & `servicemanager` |
| **Display HAL & Scanout** | `hwcomposer.msm8996` + `libsdm_shim` + DRM/KMS `msm_submit_retire` | **Working** | Live hardware-rendered `BootAnimation`, `SystemUI`, and `QuickstepLauncher` homescreen scanning out to the physical 1080x1920 AMOLED panel and `screencap` capturing valid PNGs |
| **Power & Sensors AIDL HALs** | `android.hardware.power.IPower/default` (V7) & `ISensors/default` (V3) (`libbinder_ndk`) | **Working** | Custom NDK AIDL daemons satisfying Android 17 `HintManagerService` and `SensorService` on startup |
| **SELinux & BPF (`bpffs`)** | Dynamic `fs_bpf_*` subdirectory labeling + permissive `libselinux` check bypass | **Working** | Unblocks Android 17 `netbpfload`, `netd`, `keystore2`, and `aconfigd` |
| **Audio / HIDL / FRP Shims** | In-memory patched `libaudiobase.so` & `libhidlbase.so` + `/dev/block/loop95` FRP block device | **Working** | Eliminates 60s `AudioService` watchdog, infinite `gatekeeperd`/`SoundTrigger` HIDL waits, and `PersistentDataBlockService` cold-boot errors |

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
   - **Fuel Gauge Monotonic State of Charge:** Directly reads real-time battery percentage from PMI8994 `FG_SOC` (register `0x4009`) and battery voltage from `FG_BATT` (registers `0x4150` / `0x4151`), exposing `/sys/class/power_supply/smbb-bif/capacity` and `/sys/class/power_supply/smbb-bif/voltage_now`.
   - **Continuous USB Fast Charging:** Configured `qcom,fast-charge-current-limit = <1500000>` (1.5A) and `qcom,fast-charge-high-threshold-voltage = <4350000>` (4.35V cutoff) in DTS. The driver monitors USB status (`smbb-usbin`) and keeps the battery charged while running Linux workloads.

6. **Native DRM KMS Display Engine & Samsung EA8064TG AMOLED Panel Driver:**
   - **Native Hardware KMS Subsystem:** Fully brought up the native Qualcomm display subsystem in mainline DRM:
     - `msm_mdss` (`900000.display-subsystem`): Master MDSS platform controller managing power domains and component binding via `pm_runtime_resume_and_get`.
     - `msm_mdp` (`901000.display-controller`): Mobile Display Processor MDP5 v1.7. Core clock capped at 300MHz (`max_clk = 300000000`) in [`mdp5_cfg.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/drivers/gpu/drm/msm/disp/mdp5/mdp5_cfg.c) to match MSM8996 PLL operating ranges.
     - `msm_dsi` (`994000.dsi`) & `msm_dsi_phy` (`994400.phy`): DSI 6G v1.3 controller with 4 data lanes and non-continuous clock mode.
   - **Multimedia Clock Controller (MMCC) Bringup:**
     - Probes and drives the MSM8996 Multimedia Clock Controller at `8c0000.clock-controller`.
     - Workaround for retail Pixel TrustZone XPU security: TrustZone locks register `0x529c` (`MMAGIC_BIMC_GDSC`). In [`drivers/clk/qcom/mmcc-msm8996.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/drivers/clk/qcom/mmcc-msm8996.c), disabled runtime GDSC probing (`gdscs = NULL`) and unflagged unclocked MMAGIC branches from `CLK_IS_CRITICAL` to avoid XPU security fault reboots.
   - **MDP IOMMU / SMMU Handling:**
     - The MDP SMMU at `d00000.iommu` probes cleanly using `qcom,skip-init` and `qcom,no-smr-check` properties in [`drivers/iommu/arm/arm-smmu/arm-smmu.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/drivers/iommu/arm/arm-smmu/arm-smmu.c) to prevent hardware reset while continuous scanout is live.
     - Added `qcom,mdp5` to [`drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c) with `IOMMU_DOMAIN_IDENTITY` (bypass) default domain type for uninterrupted linear scanout.
   - **Command-Mode Atomic KMS Modesetting & IRQ Sequencing:**
     - Guarded `mdp_irq_update()` in [`drivers/gpu/drm/msm/disp/mdp5/mdp5_crtc.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/drivers/gpu/drm/msm/disp/mdp5/mdp5_crtc.c) so MMIO register writes to `REG_MDP5_INTR_EN` only execute while CRTC clocks and power domains are active.
     - Configured command-mode ping-pong buffer sync write count (`REG_MDP5_PP_SYNC_WRCOUNT`) and disabled tearcheck gating (`REG_MDP5_PP_TEAR_CHECK_EN = 0`) in [`drivers/gpu/drm/msm/disp/mdp5/mdp5_cmd_encoder.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/drivers/gpu/drm/msm/disp/mdp5/mdp5_cmd_encoder.c) to allow frames to push immediately upon `CTL_START`.
   - **Native Samsung EA8064TG AMOLED Panel Driver:**
     - Fully authored [`drivers/gpu/drm/panel/panel-samsung-ea8064tg.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/drivers/gpu/drm/panel/panel-samsung-ea8064tg.c) driving the 5.0" 1080x1920 AMOLED panel with 4 DSI data lanes.
     - Implements exact Novatek/Samsung manufacturer commands (`0xf0` CMD2 page selects, SPR rainbow RGB mode, SRAM display out, 120ms exit-sleep and display-on delays).
     - Full backlight brightness control via standard 8-bit DCS command (`0x51`, 0-255 levels) registered in `/sys/class/backlight/994000.dsi.0`.
     - Framebuffer console (`fb0: msmdrmfb frame buffer device`) renders live boot text and 4 Kryo Tux penguins on physical AMOLED hardware.

7. **Interactive USB CDC-ACM Diagnostic Shell:**
   - [`tools/sailfish/init.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/tools/sailfish/init.c) binds a high-speed USB CDC-ACM serial console (`18d1:d001`) with commands:
     - `battery` / `power`: inspect battery SoC %, voltage, health, and charger state.
     - `display` / `drm`: inspect DRM KMS connectors, CRTCs, and backlight brightness.
     - `gpu` / `freedreno`: inspect DRI nodes, test DRM ioctls, and trigger GPU hardware initialization.
     - `input` / `touch`: test live multi-touch events and pressure readings.
     - `dmesg [filter]`: search live kernel log.
     - `ramoops`: inspect `/sys/fs/pstore`.
     - `bootloader`: reboot directly to fastboot.

8. **Freedreno Adreno 530 3D GPU Acceleration & Authentic Signed Zap Shader:**
   - **GPU Power & Clock Sequencing:**
     - MMCC explicitly un-collapses `GPU_GDSC` (`0x4034`) and `GPU_GX_GDSC` (`0x4024`) and deasserts clamp IO (`0x4300`), allowing `adreno_smmu` (`b40000.iommu`) and Adreno core graphics clocks (`GPU_GX_GFX3D_CLK`) to operate without power-collapse faults.
     - Device tree supplies `vdd-supply = <&vdd_gfx>` (PM8994 SMPS2) with `regulator-always-on` at 300mV - 1065mV.
   - **Authentic Retail Signed Zap Shader Extraction:**
     - Dumped HTC/Google signed zap shader binaries directly from vendor storage (`/dev/sda31`): `a530_zap_signed.mdt`, `a530_zap_signed.b00`, `a530_zap_signed.b01`, `a530_zap_signed.b02`, and `a530_zap_signed.mbn`.
     - Validated X.509 certificate chain (`OEM attestation CA`, `HW_ID 0x0005F0E1` for MSM8996 Pro) and SHA-256 segment hashes.
     - Staged firmware files into initramfs under `/lib/firmware/qcom/` and device tree `zap-shader { firmware-name = "qcom/a530_zap_signed.mdt"; };`.
   - **TrustZone Authentication & Non-Secure Mode Switch:**
     - TrustZone PAS (Peripheral Authentication Service) authenticates the firmware via SMC `0x72000602` into reserved memory `gpu_mem` (`0x90f00000`).
     - Adreno command processor (CP) sends `CP_SET_SECURE_MODE (0)` packet to switch the GPU out of secure mode.
     - Userspace DRI nodes `/dev/dri/card1` and `/dev/dri/renderD128` query cleanly through DRM UAPI (`msm` v1.13.0) without hardware access faults (`NOC_ERROR`).

9. **Android 17 Userspace Architecture & APEX Loop Subsystem:**
   - **Booting Official AOSP GSI:** Booting Google's official Android 17 Generic System Image (`DP11.261016.003-16708287`) on physical Pixel 1 hardware using Linux Kernel 6.18.
   - **Initramfs Setup ([`tools/sailfish/init.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/tools/sailfish/init.c)):**
     - Pre-allocates 96 loop block devices (`/dev/loop0`..`/dev/loop95`).
     - Activates BinderFS (`/dev/binderfs` with nodes `binder`, `hwbinder`, `vndbinder`) mounted at `/dev/binderfs` and symlinked into `/dev/`.
     - Mounts essential pseudo-filesystems: `cgroup2`, `debugfs`, `tracefs`, and `bpf`.
     - Loop mounts all 44 Android APEX packages under `/apex/` directly or prepares them for `apexd`.
     - Spawns an interactive root diagnostic shell that joins Android `init`'s mount and PID namespaces (`CLONE_NEWNS` / `CLONE_NEWPID`) without disrupting runtime properties or socket bindings.
   - **Framework Lifecycle:** Android `init` starts critical core daemons (`servicemanager`, `hwservicemanager`, `vndservicemanager`), initializes `zygote64`, and boots `system_server`. Core bootstrap services (`ActivityManagerService`, `PowerManagerService`, `BatteryService`) run stably, reaching `DisplayManagerService`.

10. **Mesa 25.2.8 Freedreno (`FD530`) GLES Driver & Open-Source `libadreno_utils.so` ([`tools/sailfish/mesa/`](file:///home/patrick-donghil/Desktop/Kernel%20Project/tools/sailfish/mesa/), [`tools/sailfish/adreno_utils/`](file:///home/patrick-donghil/Desktop/Kernel%20Project/tools/sailfish/adreno_utils/)):**
    - **Root Cause:** Qualcomm's legacy Android 10 proprietary blobs (`libEGL_adreno.so`, `libGLESv2_adreno.so`, `libadreno_utils.so`) require downstream `/dev/kgsl-3d0` and `/dev/ion`.
    - **Mesa 25.2.8 Freedreno Integration:** Cross-compiled Mesa 25.2.8 (`gallium/freedreno` over mainline `drm/msm`) linked solely against Android 17 LLNDK libraries (`libc`, `libm`, `libdl`, `liblog`, `libnativewindow`, `libsync`) and bind-mounted into `/vendor/lib64/egl/`.
    - **Adreno 530 GMEM Stride Alignment (`libadreno_utils.so`):** Authored an open-source replacement [`libadreno_utils.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/tools/sailfish/adreno_utils/libadreno_utils.c) bind-mounted over `/vendor/lib64/libadreno_utils.so` that aligns buffer widths to 64 pixels (`1088` pixels = `4352` bytes for `1080x1920`) and heights to 32 pixels to match Freedreno's `gmem_align_w = 64`, plus exports Android 17 `Gralloc2Mapper::validateBufferSize` and `gralloc1` validation symbols.
    - **Single-Ring Adreno A530 Command Submission:** Configured [`a5xx_gpu.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/drivers/gpu/drm/msm/adreno/a5xx_gpu.c) with `nr_rings = 1` (`ring=0`), `has_whereami = false`, runtime PM stay-awake after initial `hw_init`, and `401.8 MHz` nominal OPP cap, completing every hardware 3D command stream in ~9 ms via `a5xx_irq: CACHE_FLUSH_TS`.

11. **Android 17 VINTF HIDL Compliance ([`tools/sailfish/android.hardware.graphics.xml`](file:///home/patrick-donghil/Desktop/Kernel%20Project/tools/sailfish/android.hardware.graphics.xml)):**
    - **VINTF Enforcement:** Android 17's `hwservicemanager` strictly checks both Device and Framework HAL manifests. Legacy vendor manifest on Sailfish lacked explicit transport declarations, causing `hwservicemanager` to reject `android.hardware.graphics.composer@2.1` and `android.hardware.graphics.allocator@2.0` with `Transport::EMPTY`.
    - **Manifest Solution:** Authored a targeted VINTF fragment declaring `composer@2.1` and `allocator@2.0` with `<transport>hwbinder</transport>`, deployed to `/system/etc/vintf/manifest/android.hardware.graphics.xml`. Both services register cleanly upon launch.

12. **Vendor Binder (`vndbinder`) & Qualcomm Display `qservice_daemon` ([`tools/sailfish/qservice_daemon/`](file:///home/patrick-donghil/Desktop/Kernel%20Project/tools/sailfish/qservice_daemon/)):**
    - **Dependency on `display.qservice`:** Stock `hwcomposer.msm8996.so` requires `display.qservice` on `/dev/vndbinder` and queries it during initialization via `waitForService("display.qservice")`. Without it, composer fails to open the hwcomposer device after a 5-second timeout.
    - **vndservicemanager Modernization:** Android 17 deprecates legacy HIDL servicemanager. Bind-mounted Android 17 AIDL `servicemanager` over `/vendor/bin/vndservicemanager`, providing modern AIDL binder context on `/dev/vndbinder`.
    - **Daemon Implementation & ARM64 ABI Bridge:** Authored a standalone helper daemon ([`qservice_daemon.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/tools/sailfish/qservice_daemon/qservice_daemon.c)) compiled with dynamic stubs. Utilizes register-level ARM64 `BLR` calling conventions (`x8` indirect return buffer) to bridge Bionic `sp<ProcessState>` and `sp<IServiceManager>` ABI across modern Android 17 and legacy vendor binder:
      - `ProcessState::initWithDriver("/dev/vndbinder")`
      - `ProcessState::getContextObject()`
      - `IServiceManager::asInterface()`
      - `getServiceManagerShimFromAidlServiceManagerForTests()`
      - `setDefaultServiceManager()`
      - `android::Init()` from `libqservice.so`
      - Successfully publishes `display.qservice` into the vendor binder tree.

13. **Qualcomm SDM Core & MDP Caps Reverse-Engineering (`libsdmcore.so`):**
    - **Clock Fudge Factor Crash (SIGSEGV in `strtol_l` / `atoi`):**
      - `libsdmcore.so` crashed at `0x288b0` when reading `/sys/devices/virtual/graphics/fb0/mdp/caps`.
      - Disassembly revealed the parser expects `clk_fudge_factor` to supply **two space-separated integer values** (`<numerator> <denominator>`), e.g. `clk_fudge_factor=105 100`.
    - **SDE Hardware Revision:**
      - Set `hw_rev=268894210` (`0x10070002` in hex), identifying the MDSS hardware as Snapdragon 821 (MSM8996 Pro) SDE v5.0.
    - **MDP Pipe Parser Reverse-Engineering (`0x28e5c`–`0x29330`):**
      - Discovered the exact syntax required by `HWInfo::GetHWResourceInfo` for MDSS hardware pipes:
        - Header: `pipe_count=10`
        - Pipe definition: `pipe_type=<type> pipe_ndx=<id> rects=1`
        - Pipe types parsed: `vig` (increments `num_vig_pipe`), `dma` (increments `num_dma_pipe`), `rgb` (increments `num_rgb_pipe`), `cursor` (increments `num_cursor_pipe`).
      - Populated complete 10-pipe architecture (4 VIG, 4 DMA, 2 Cursor).
      - **Result:** `HWInfo::GetHWResourceInfo: SDE Version = 500, SDE Revision = 10070002, RGB = 0, VIG = 4, DMA = 4, Cursor = 2`.
      - **Milestone:** `CoreInterface::CreateCore` and `HWCDisplayPrimary::Create` succeed!
    - **Hardware-Accelerated Scanout & `libsdm_shim.so` ([`tools/sailfish/sdm_shim/libsdm_shim.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/tools/sailfish/sdm_shim/libsdm_shim.c)):**
      - Intercepts legacy MSMFB ioctls (`MSMFB_METADATA_GET`, `MSMFB_OVERLAY_VSYNC_CTRL`, `MSMFB_DISPLAY_COMMIT`), provides 60 Hz hardware VSYNC pacing (`VSYNC=<ns>`), and patches `libsdmcore.so`'s output buffer check in memory.
      - In [`drivers/gpu/drm/msm/msm_gem_submit.c`](file:///home/patrick-donghil/Desktop/Kernel%20Project/drivers/gpu/drm/msm/msm_gem_submit.c), `msm_submit_retire()` invalidates CPU D-cache lines (`dc civac`), converts completed `1080x1920` (`1088`-stride) `RGBA_8888` GPU buffers to `XRGB_8888` in `fbh->info->screen_buffer`, and triggers `drm_fb_helper_damage_area()` so MDP5 flushes every rendered frame to the physical AMOLED panel.

14. **Kernel & Storage Primitives for Modern Android GSI:**
    - **Ext4 Shared Blocks (`fs/ext4/ext4.h`):** Enabled `EXT4_FEATURE_RO_COMPAT_SHARED_BLOCKS` support to allow mounting modern deduplicated ext4 Android 17 GSI images (`system.img`).
    - **Namespace Mount Propagation (`fs/namespace.c`):** Fixed root mount propagation so child namespaces properly inherit root mounts during Android `pivot_root`.
    - **Reboot Interception (`kernel/pid_namespace.c`):** Intercepted child PID namespace reboot requests from crashing processes during userspace bringup, preventing unintended device reboots.
    - **SELinux Permissive State Management (`security/selinux/`):** Added a fake enforcing state handler and context transition fallbacks for `vendor_init` to maintain stability during boot debugging.

15. **Android 17 Full Framework, SystemUI & Launcher3 Bring-Up (`sys.boot_completed=1`):**
    - **Adreno A530 Resume & Devfreq Locking (`drivers/gpu/drm/msm/adreno/a5xx_gpu.c`, `drivers/gpu/drm/msm/msm_gpu_devfreq.c`):** Returned `1` in `a5xx_zap_shader_resume()` to skip redundant `CP_SET_SECURE_MODE(0)` packets while TrustZone PAS 13 is halted on always-on GDSC, widened `REG_A5XX_RBBM_INTERFACE_HANG_INT_CNTL` (`0x3FFFFF`) to prevent false-positive hangcheck interrupts, reset `REG_A5XX_CP_RB_WPTR`, and locked Adreno A530 devfreq to `401.8 MHz` (`gpu_clamp_to_idle = false`) so rapid `SurfaceFlinger`, `SystemUI`, and `QuickstepLauncher` GLES passes never stall at `133 MHz`.
    - **MDP5 Atomic Plane Pipe Tracking (`drivers/gpu/drm/msm/disp/mdp5/mdp5_pipe.c`, `drivers/gpu/drm/msm/disp/mdp5/mdp5_plane.c`, `drivers/gpu/drm/msm/msm_kms.c`):** Cleared stale `hwpipe`/`r_hwpipe` pointers via `mdp5_get_existing_global_state()`, skipped already-assigned right-hand pipes in `mdp5_pipe_assign()`, avoided releasing pipes reused on the same plane, returned `0` cleanly when `mdp5_pipe_release()` sees an unowned pipe, and returned `-EBUSY` from `msm_kms_pm_prepare()` to prevent system suspend from tearing down live display scanout.
    - **Non-Freezing Kernel Suspend Stub (`kernel/power/suspend.c`):** Short-circuited `pm_suspend()` with an `msleep(500)` and clean `0` return instead of calling `enter_state()`, so Android's `SystemSuspend` loop never triggers `ksys_sync_helper()` or freezes userspace tasks while the display is active.
    - **Custom NDK AIDL Power V7 & Sensors V3 HALs (`tools/sailfish/power_hal/`, `tools/sailfish/sensors_hal/`):** Implemented standalone `libbinder_ndk` daemons and VINTF framework manifest fragments (`tools/sailfish/android.hardware.power.xml`, `tools/sailfish/android.hardware.sensors.xml`) for `android.hardware.power.IPower/default` (V7, handling `getHintSessionPreferredRate`, `getSupportInfo`, and `createHintSessionWithConfig`) and `android.hardware.sensors.ISensors/default` (V3, reporting an empty sensor list in `getSensorsList`), unblocking `HintManagerService` and `SensorService`.
    - **SELinux `bpffs` Subdirectory Contexts & `aconfigd` `/metadata` Storage (`security/selinux/hooks.c`, `security/selinux/ss/services.c`):** Synthesized `u:object_r:fs_bpf_<subdir>:s0` labels in `selinux_inode_getsecurity()` for `bpf` filesystem subdirectories and prioritized `genfscon` lookups for `SE_SBGENFS` filesystems in `security_fs_use()`, unblocking `netbpfload` and `netd`; mounted a writable `0755` tmpfs on `/metadata` so `aconfigd-system` populates feature flag storage for `AdvancedProtectionService`.
    - **Zero-Wait `libaudiobase.so`, `libhidlbase.so`, `libselinux.so` & Filtered Vendor VINTF/Permissions (`tools/sailfish/init.c`):** Applied in-memory binary patches and bind-mounts for `libaudiobase.so` (skipping 60s `AudioSystem`/`mediautils` service waits), `libhidlbase.so` (forcing `retry=false` in `getRawServiceInternal` so `gatekeeperd` and `SoundTriggerMiddlewareService` never hang), and `libselinux.so` (`selinux_check_access() -> 0` for `keystore2`); replaced `/vendor/etc/vintf/manifest.xml` with an active-only manifest (`allocator@2.0`, `composer@2.1`, `mapper@2.0`/`2.1`, `health@2.0`, `power@1.1`, `renderscript@1.0`); filtered `*nfc*`/`*mifare*` feature XMLs and masked `/apex/com.android.nfcservices/priv-app` to eliminate `com.android.nfc` (`NfcNciApex`) crash-loops; provisioned a zeroed 512 KB `/dev/block/loop95` FRP block device; and set `ro.hw_timeout_multiplier=10`.

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

---

## Booting Android 17 (AOSP GSI)

1. **Flash Android 17 GSI to `system`:**
   ```bash
   fastboot flash system out/system.img
   ```

2. **Boot Linux Kernel 6.18:**
   ```bash
   fastboot boot out/boot.img
   ```
   All graphics shims (Mesa 25.2.8 Freedreno, `libadreno_utils.so`, `libsdm_shim.so`), custom AIDL `power_hal` / `sensors_hal` daemons, VINTF manifests, framework library patches (`libaudiobase.so`, `libhidlbase.so`, `libselinux.so`), MDSS sysfs mock nodes, `/dev/block/loop95` FRP backing, and `qservice_daemon` are **100% automated** by `/init` in `out/boot.img` on boot. The device boots directly into `BootAnimation`, completes `sys.boot_completed=1`, and brings up `SystemUI` and the Android 17 Launcher (`QuickstepLauncher`).

3. **Access the Android Root Shell:**
   Connect over USB CDC-ACM serial console:
   ```bash
   sudo picocom -b 115200 /dev/ttyACM0
   ```
   The initramfs automatically mounts APEX packages, configures the automated compatibility overlays, starts Android `init` (`/system/bin/init`), and opens an interactive root shell inside Android's PID/mount namespaces:
   ```text
   sailfish:/ #
   ```

---

## Restoring & Resuming Workflow

When restoring the phone back to stock Android 10 and returning later to resume kernel bringup:
1. **Stock Android 10:** Flash stock factory image if needed.
2. **Resuming Android 17 / Kernel 6.18:**
   - Boot modern Kernel 6.18 (from host RAM or flashed to `boot`):
     ```bash
     fastboot boot out/boot.img
     ```
   - Connect via serial console:
     ```bash
     sudo picocom -b 115200 /dev/ttyACM0
     ```
   - Early setup in `tools/sailfish/init.c` automatically binds Mesa 25.2.8 Freedreno (`libEGL_adreno.so`, `libGLESv2_adreno.so`, `libgallium_dri.so`), `libadreno_utils.so`, and `libsdm_shim.so`, installs custom AIDL `power_hal` V7 and `sensors_hal` V3 services, deploys filtered VINTF manifests and runtime library patches, syncs SELinux sha256 checksums, mounts writable `/metadata` for `aconfigd`, populates MDSS sysfs mock nodes, and launches Android `init` with the background supervisor and `/data/start_graphics.sh` helper until `SystemUI` and `QuickstepLauncher` reach the homescreen.

