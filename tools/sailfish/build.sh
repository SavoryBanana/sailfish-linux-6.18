#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REAL_KERNEL_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
ln -sfn "$REAL_KERNEL_DIR" /tmp/sailfish_kernel
KERNEL_DIR="/tmp/sailfish_kernel"
cd -L "$KERNEL_DIR"

OUT_DIR="$KERNEL_DIR/out"
mkdir -p "$OUT_DIR"

echo "=== 1. Building Minimal Bring-up initramfs ==="
rm -rf "$OUT_DIR/initramfs_root"
mkdir -p "$OUT_DIR/initramfs_root"
clang -target aarch64-linux-gnu -static -O2 "$SCRIPT_DIR/init.c" -o "$OUT_DIR/initramfs_root/init"
chmod +x "$OUT_DIR/initramfs_root/init"

# Stage early GPU microcode in initramfs for early boot
mkdir -p "$OUT_DIR/initramfs_root/lib/firmware/qcom"
for fw in a530_pm4.fw a530_pfp.fw a530v3_gpmu.fw2; do
    if [ -f "/lib/firmware/qcom/$fw.zst" ]; then
        zstd -d -c "/lib/firmware/qcom/$fw.zst" > "$OUT_DIR/initramfs_root/lib/firmware/qcom/$fw"
        ln -sf "qcom/$fw" "$OUT_DIR/initramfs_root/lib/firmware/$fw"
    fi
done

# Copy any authentic OEM-extracted firmware from tools/sailfish/firmware
if [ -d "$SCRIPT_DIR/firmware" ] && [ "$(ls -A "$SCRIPT_DIR/firmware")" ]; then
    echo "[+] Staging device-specific firmware from $SCRIPT_DIR/firmware"
    cp -r "$SCRIPT_DIR/firmware/"* "$OUT_DIR/initramfs_root/lib/firmware/qcom/"
    for f in "$SCRIPT_DIR/firmware/"*; do
        base="$(basename "$f")"
        ln -sf "qcom/$base" "$OUT_DIR/initramfs_root/lib/firmware/$base"
    done
elif [ -f "/lib/firmware/qcom/apq8096/a530_zap.mbn.zst" ]; then
    zstd -d -c "/lib/firmware/qcom/apq8096/a530_zap.mbn.zst" > "$OUT_DIR/initramfs_root/lib/firmware/qcom/a530_zap.mdt"
    ln -sf "a530_zap.mdt" "$OUT_DIR/initramfs_root/lib/firmware/qcom/a530_zap.mbn"
    ln -sf "qcom/a530_zap.mdt" "$OUT_DIR/initramfs_root/lib/firmware/a530_zap.mdt"
    ln -sf "qcom/a530_zap.mbn" "$OUT_DIR/initramfs_root/lib/firmware/a530_zap.mbn"
fi

# Stage Android 17 graphics compatibility shims and manifests
echo "[+] Staging Android 17 graphics compatibility shims into initramfs"
mkdir -p "$OUT_DIR/initramfs_root/tools"
cp "$SCRIPT_DIR/egl_shim/libEGL_adreno_shim.so" "$OUT_DIR/initramfs_root/tools/libEGL_adreno_shim.so"
# Mesa Freedreno GLES driver (built + staged by tools/sailfish/mesa/stage_mesa.sh).
# When present, init.c prefers it over the mock EGL shim.
if [ -f "$SCRIPT_DIR/mesa/lib64/libgallium_dri.so" ]; then
    echo "[+] Staging Mesa Freedreno GLES libs"
    mkdir -p "$OUT_DIR/initramfs_root/tools/mesa"
    cp "$SCRIPT_DIR/mesa/lib64/"*.so "$OUT_DIR/initramfs_root/tools/mesa/"
fi
if [ -f "$SCRIPT_DIR/sdm_shim/libsdm_shim.c" ]; then
    echo "[+] Building libsdm_shim.so"
    VER_FLAG=""
    if [ -f "$SCRIPT_DIR/sdm_shim/libsdm_shim.ver" ]; then
        VER_FLAG="-Wl,--version-script=$SCRIPT_DIR/sdm_shim/libsdm_shim.ver"
    fi
    aarch64-linux-gnu-gcc -shared -fPIC -O2 -nostdlib "$SCRIPT_DIR/sdm_shim/libsdm_shim.c" -o "$SCRIPT_DIR/sdm_shim/libsdm_shim.so" $VER_FLAG -Wl,-z,max-page-size=4096
fi
if [ -f "$SCRIPT_DIR/sdm_shim/libsdm_shim.so" ]; then
    cp "$SCRIPT_DIR/sdm_shim/libsdm_shim.so" "$OUT_DIR/initramfs_root/tools/libsdm_shim.so"
fi
if [ -f "$SCRIPT_DIR/libui_patched.so" ]; then
    echo "[+] Staging patched libui.so (Gralloc 2.x support)"
    cp "$SCRIPT_DIR/libui_patched.so" "$OUT_DIR/initramfs_root/tools/libui.so"
fi
if [ -f "$SCRIPT_DIR/adreno_utils/libadreno_utils.so" ]; then
    cp "$SCRIPT_DIR/adreno_utils/libadreno_utils.so" "$OUT_DIR/initramfs_root/tools/libadreno_utils.so"
fi
if [ ! -f "$SCRIPT_DIR/vdc_stub/vdc_stub" ]; then
    mkdir -p "$SCRIPT_DIR/vdc_stub"
    echo -e ".global _start\n.text\n_start:\n  mov x8, 93\n  mov x0, 0\n  svc 0\n" | aarch64-linux-gnu-gcc -x assembler -nostdlib -static -s -o "$SCRIPT_DIR/vdc_stub/vdc_stub" -
fi
cp "$SCRIPT_DIR/vdc_stub/vdc_stub" "$OUT_DIR/initramfs_root/tools/vdc_stub"
chmod +x "$OUT_DIR/initramfs_root/tools/vdc_stub"

mkdir -p "$SCRIPT_DIR/pause_stub"
# Call __NR_ppoll(NULL, 0, NULL, NULL, 0) (aarch64 syscall 73) so stubbed daemons
# sleep in TASK_INTERRUPTIBLE with 0% CPU instead of busy-looping on epoll_pwait (22).
echo -e ".global _start\n.text\n_start:\n1:\n  mov x0, 0\n  mov x1, 0\n  mov x2, 0\n  mov x3, 0\n  mov x4, 0\n  mov x8, 73\n  svc 0\n  b 1b\n" | aarch64-linux-gnu-gcc -x assembler -nostdlib -static -s -o "$SCRIPT_DIR/pause_stub/pause_stub" -
cp "$SCRIPT_DIR/pause_stub/pause_stub" "$OUT_DIR/initramfs_root/tools/pause_stub"
chmod +x "$OUT_DIR/initramfs_root/tools/pause_stub"

mkdir -p "$SCRIPT_DIR/wrap_preload"
aarch64-linux-gnu-gcc -nostdlib -static -s -O2 -o "$SCRIPT_DIR/wrap_preload/wrap_preload" "$SCRIPT_DIR/wrap_preload/wrap_preload.c"
cp "$SCRIPT_DIR/wrap_preload/wrap_preload" "$OUT_DIR/initramfs_root/tools/wrap_preload"
chmod +x "$OUT_DIR/initramfs_root/tools/wrap_preload"

cp "$SCRIPT_DIR/qservice_daemon/qservice_daemon" "$OUT_DIR/initramfs_root/tools/qservice_daemon"
chmod +x "$OUT_DIR/initramfs_root/tools/qservice_daemon"

"$SCRIPT_DIR/power_hal/build.sh"
cp "$SCRIPT_DIR/power_hal/power_hal" "$OUT_DIR/initramfs_root/tools/power_hal"
chmod +x "$OUT_DIR/initramfs_root/tools/power_hal"

chmod +x "$SCRIPT_DIR/sensors_hal/build.sh"
"$SCRIPT_DIR/sensors_hal/build.sh"
cp "$SCRIPT_DIR/sensors_hal/sensors_hal" "$OUT_DIR/initramfs_root/tools/sensors_hal"
chmod +x "$OUT_DIR/initramfs_root/tools/sensors_hal"

chmod +x "$SCRIPT_DIR/bluetooth_hal/build.sh"
"$SCRIPT_DIR/bluetooth_hal/build.sh"
cp "$SCRIPT_DIR/bluetooth_hal/bluetooth_hal" "$OUT_DIR/initramfs_root/tools/bluetooth_hal"
chmod +x "$OUT_DIR/initramfs_root/tools/bluetooth_hal"

# Stage QCA6174 ath10k firmware & regulatory database under /tools/firmware (survives vendor mount)
# and symlink from /lib/firmware (early pre-mount fallback) without duplicating ~1.4 MB in initramfs.
mkdir -p "$OUT_DIR/initramfs_root/tools/firmware/ath10k/QCA6174/hw3.0" \
         "$OUT_DIR/initramfs_root/lib/firmware"
for fw in firmware-6.bin board.bin; do
    zstd -d -f -c "/lib/firmware/ath10k/QCA6174/hw3.0/$fw.zst" \
        > "$OUT_DIR/initramfs_root/tools/firmware/ath10k/QCA6174/hw3.0/$fw"
done
ln -sfn "/tools/firmware/ath10k" "$OUT_DIR/initramfs_root/lib/firmware/ath10k"
for reg in regulatory.db regulatory.db.p7s; do
    if [ -f "/lib/firmware/$reg" ]; then
        cp "/lib/firmware/$reg" "$OUT_DIR/initramfs_root/tools/firmware/$reg"
        ln -sf "/tools/firmware/$reg" "$OUT_DIR/initramfs_root/lib/firmware/$reg"
    fi
done

# Generate static RRO APK enabling Android 17 mainline wpa_supplicant in com.android.wifi.resources
python3 "$SCRIPT_DIR/wifi_overlay/build_wifi_overlay.py" "$OUT_DIR/initramfs_root/tools/SailfishWifiOverlay.apk"

cp "$SCRIPT_DIR/android.hardware.graphics.xml" "$OUT_DIR/initramfs_root/tools/android.hardware.graphics.xml"
cp "$SCRIPT_DIR/android.hardware.power.xml" "$OUT_DIR/initramfs_root/tools/android.hardware.power.xml"
cp "$SCRIPT_DIR/android.hardware.sensors.xml" "$OUT_DIR/initramfs_root/tools/android.hardware.sensors.xml"
cp "$SCRIPT_DIR/android.hardware.bluetooth.xml" "$OUT_DIR/initramfs_root/tools/android.hardware.bluetooth.xml"
if [ -f "$SCRIPT_DIR/setup_composer_sysfs.sh" ]; then
    cp "$SCRIPT_DIR/setup_composer_sysfs.sh" "$OUT_DIR/initramfs_root/tools/setup_composer_sysfs.sh"
    chmod +x "$OUT_DIR/initramfs_root/tools/setup_composer_sysfs.sh"
fi

# Note: initramfs_root is embedded directly into the kernel Image via
# CONFIG_INITRAMFS_SOURCE (compressed with XZ). Provide a minimal valid
# cpio.gz stub for mkbootimg --ramdisk so Qualcomm aboot is satisfied
# without duplicating the 5 MB ramdisk a second time in boot.img.
(cd "$OUT_DIR/initramfs_root" && printf ".\n" | cpio -o -H newc --quiet | gzip -9 > "$OUT_DIR/initramfs.cpio.gz")

echo "=== 2. Configuring Kernel (sailfish_defconfig) ==="
make sailfish_defconfig ARCH=arm64 LLVM=1

echo "=== 3. Building Kernel Image & DTB ==="
make -j$(nproc) ARCH=arm64 LLVM=1 Image qcom/msm8996pro-google-sailfish.dtb

echo "=== 4. Packaging Appended DTB (Image-dtb with Qualcomm 0x2C Offset) ==="
cp "$KERNEL_DIR/arch/arm64/boot/dts/qcom/msm8996pro-google-sailfish.dtb" "$OUT_DIR/msm8996pro-google-sailfish.dtb"

python3 -c "
import struct

with open('$KERNEL_DIR/arch/arm64/boot/Image', 'rb') as f:
    kdata = bytearray(f.read())

with open('$OUT_DIR/msm8996pro-google-sailfish.dtb', 'rb') as f:
    dtbdata = f.read()

# Pad kernel to page boundary
pad = 4096 - (len(kdata) % 4096)
if pad != 4096:
    kdata += b'\x00' * pad

dtb_offset = len(kdata)

# Write dtb_offset at 0x2C (Qualcomm LK DTB_OFFSET macro)
struct.pack_into('<I', kdata, 0x2C, dtb_offset)

image_dtb = kdata + dtbdata

with open('$OUT_DIR/Image-dtb', 'wb') as f:
    f.write(image_dtb)

print(f'[+] Created Image-dtb ({len(image_dtb)} bytes, DTB at offset {dtb_offset:#x})')
"

echo "=== 5. Packaging boot.img (Stock Sailfish Non-Overlapping Offsets) ==="
python3 "$SCRIPT_DIR/mkbootimg.py" \
  --kernel "$OUT_DIR/Image-dtb" \
  --ramdisk "$OUT_DIR/initramfs.cpio.gz" \
  --base 0x80000000 \
  --kernel_offset 0x00080000 \
  --ramdisk_offset 0x02800000 \
  --tags_offset 0x02600000 \
  --pagesize 4096 \
  --cmdline "console=ramoops0 androidboot.hardware=sailfish androidboot.debuggable=1 androidboot.selinux=permissive androidboot.veritymode=disabled root=/dev/ram0 rw rdinit=/init init=/init clk_ignore_unused pd_ignore_unused firmware_class.path=/vendor/firmware,/tools/firmware iommu.passthrough=1 pcie_aspm=off audit=0" \
  -o "$OUT_DIR/boot.img"

echo "=== Build Complete: out/boot.img ($(ls -lh "$OUT_DIR/boot.img" | awk '{print $5}')) ==="
