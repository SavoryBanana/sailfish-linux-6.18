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
        cp "$OUT_DIR/initramfs_root/lib/firmware/qcom/$fw" "$OUT_DIR/initramfs_root/lib/firmware/$fw"
    fi
done

# Copy any authentic OEM-extracted firmware from tools/sailfish/firmware
if [ -d "$SCRIPT_DIR/firmware" ] && [ "$(ls -A "$SCRIPT_DIR/firmware")" ]; then
    echo "[+] Staging device-specific firmware from $SCRIPT_DIR/firmware"
    cp -r "$SCRIPT_DIR/firmware/"* "$OUT_DIR/initramfs_root/lib/firmware/qcom/"
    cp -r "$SCRIPT_DIR/firmware/"* "$OUT_DIR/initramfs_root/lib/firmware/"
elif [ -f "/lib/firmware/qcom/apq8096/a530_zap.mbn.zst" ]; then
    zstd -d -c "/lib/firmware/qcom/apq8096/a530_zap.mbn.zst" > "$OUT_DIR/initramfs_root/lib/firmware/qcom/a530_zap.mdt"
    cp "$OUT_DIR/initramfs_root/lib/firmware/qcom/a530_zap.mdt" "$OUT_DIR/initramfs_root/lib/firmware/qcom/a530_zap.mbn"
    cp "$OUT_DIR/initramfs_root/lib/firmware/qcom/a530_zap.mdt" "$OUT_DIR/initramfs_root/lib/firmware/a530_zap.mdt"
    cp "$OUT_DIR/initramfs_root/lib/firmware/qcom/a530_zap.mdt" "$OUT_DIR/initramfs_root/lib/firmware/a530_zap.mbn"
fi

(cd "$OUT_DIR/initramfs_root" && find . | cpio -o -H newc | gzip -9 > "$OUT_DIR/initramfs.cpio.gz")

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
  --cmdline "console=tty0 console=earlyramoops keep_bootcon androidboot.hardware=sailfish root=/dev/ram0 rw rdinit=/init init=/init clk_ignore_unused pd_ignore_unused firmware_class.path=/vendor/firmware initcall_debug iommu.passthrough=1" \
  -o "$OUT_DIR/boot.img"

echo "=== Build Complete: out/boot.img ($(ls -lh "$OUT_DIR/boot.img" | awk '{print $5}')) ==="
