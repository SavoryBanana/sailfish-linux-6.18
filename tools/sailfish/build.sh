#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="/home/patrick-donghil/Desktop/kernel_project"
cd "$KERNEL_DIR"

OUT_DIR="$KERNEL_DIR/out"
mkdir -p "$OUT_DIR"

# Ensure custom tools (e.g. lz4) are in PATH
export PATH="$KERNEL_DIR/tools/bin:$PATH"

echo "=== 1. Configuring Kernel (sailfish_defconfig) ==="
if [ ! -f "$KERNEL_DIR/.config" ]; then
    make sailfish_defconfig ARCH=arm64 LLVM=1
fi

echo "=== 2. Building Kernel Image.lz4 & DTB ==="
make -j$(nproc) ARCH=arm64 LLVM=1 LZ4="$KERNEL_DIR/tools/bin/lz4" Image.lz4 qcom/msm8996pro-google-sailfish.dtb

echo "=== 3. Building Minimal Bring-up initramfs ==="
if [ ! -f "$OUT_DIR/initramfs.cpio.gz" ] || [ "$SCRIPT_DIR/init.c" -nt "$OUT_DIR/initramfs.cpio.gz" ]; then
    mkdir -p "$OUT_DIR/initramfs_root"
    clang -target aarch64-linux-gnu -static -O2 "$SCRIPT_DIR/init.c" -o "$OUT_DIR/initramfs_root/init"
    chmod +x "$OUT_DIR/initramfs_root/init"
    (cd "$OUT_DIR/initramfs_root" && find . | cpio -o -H newc | gzip -9 > "$OUT_DIR/initramfs.cpio.gz")
fi

echo "=== 4. Packaging Qualcomm Device Tree Table (QCDT dt.img) ==="
cp "$KERNEL_DIR/arch/arm64/boot/dts/qcom/msm8996pro-google-sailfish.dtb" "$OUT_DIR/msm8996pro-google-sailfish.dtb"

python3 "$SCRIPT_DIR/dtbtool.py" \
  -o "$OUT_DIR/dt.img" \
  -s 4096 \
  -p "$KERNEL_DIR/scripts/dtc/" \
  -3 \
  "$OUT_DIR/"

cat "$KERNEL_DIR/arch/arm64/boot/Image.lz4" "$OUT_DIR/msm8996pro-google-sailfish.dtb" > "$OUT_DIR/Image.lz4-dtb"

echo "=== 5. Packaging boot.img ==="
python3 "$SCRIPT_DIR/mkbootimg.py" \
  --kernel "$KERNEL_DIR/arch/arm64/boot/Image.lz4" \
  --ramdisk "$OUT_DIR/initramfs.cpio.gz" \
  --dt "$OUT_DIR/dt.img" \
  --base 0x80000000 \
  --kernel_offset 0x00080000 \
  --ramdisk_offset 0x02800000 \
  --tags_offset 0x02600000 \
  --pagesize 4096 \
  --cmdline "console=tty0 androidboot.hardware=sailfish root=/dev/ram0 rw init=/init clk_ignore_unused pd_ignore_unused" \
  -o "$OUT_DIR/boot.img"

echo "=== Build Complete: out/boot.img ($(ls -lh "$OUT_DIR/boot.img" | awk '{print $5}')) ==="
