#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$KERNEL_DIR"

OUT_DIR="$KERNEL_DIR/out"
mkdir -p "$OUT_DIR"

echo "=== 1. Configuring Kernel (sailfish_defconfig) ==="
if [ ! -f "$KERNEL_DIR/.config" ]; then
    make sailfish_defconfig ARCH=arm64 LLVM=1
fi

echo "=== 2. Building Kernel Image.gz & DTB ==="
make -j$(nproc) ARCH=arm64 LLVM=1 Image.gz qcom/msm8996pro-google-sailfish.dtb

echo "=== 3. Building Minimal Bring-up initramfs ==="
if [ ! -f "$OUT_DIR/initramfs.cpio.gz" ]; then
    mkdir -p "$OUT_DIR/initramfs_root"
    clang -target aarch64-linux-gnu -static -O2 "$SCRIPT_DIR/init.c" -o "$OUT_DIR/initramfs_root/init"
    chmod +x "$OUT_DIR/initramfs_root/init"
    (cd "$OUT_DIR/initramfs_root" && find . | cpio -o -H newc | gzip -9 > "$OUT_DIR/initramfs.cpio.gz")
fi

echo "=== 4. Packaging boot.img ==="
cp "$KERNEL_DIR/arch/arm64/boot/dts/qcom/msm8996pro-google-sailfish.dtb" "$OUT_DIR/msm8996pro-google-sailfish.dtb"
cat "$KERNEL_DIR/arch/arm64/boot/Image.gz" "$OUT_DIR/msm8996pro-google-sailfish.dtb" > "$OUT_DIR/Image.gz-dtb"

python3 "$SCRIPT_DIR/mkbootimg.py" \
  --kernel "$OUT_DIR/Image.gz-dtb" \
  --ramdisk "$OUT_DIR/initramfs.cpio.gz" \
  --dtb "$OUT_DIR/msm8996pro-google-sailfish.dtb" \
  --base 0x80000000 \
  --kernel_offset 0x00080000 \
  --ramdisk_offset 0x02200000 \
  --tags_offset 0x02000000 \
  --pagesize 4096 \
  --cmdline "earlycon=msm_serial_dm,0x75b0000 console=ttyMSM0,115200n8 androidboot.hardware=sailfish root=/dev/ram0 rw init=/init" \
  -o "$OUT_DIR/boot.img"

echo "=== Build Complete: out/boot.img ($(ls -lh "$OUT_DIR/boot.img" | awk '{print $5}')) ==="
