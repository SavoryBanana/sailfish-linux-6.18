#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

KERNEL_DIR="$PROJECT_DIR/kernel/common-6.18"

# 1. Fetch ACK if not present
if [ ! -d "$KERNEL_DIR" ]; then
    echo "=== Cloning Android 17 ACK 6.18 (shallow) ==="
    mkdir -p "$PROJECT_DIR/kernel"
    git clone --depth=1 -b android17-6.18 https://android.googlesource.com/kernel/common "$KERNEL_DIR"
fi

# 2. Sync port source files into kernel tree
echo "=== Syncing Device Tree & Configs into Kernel Tree ==="
cp "$PROJECT_DIR/dts/msm8996pro-google-sailfish.dts" "$KERNEL_DIR/arch/arm64/boot/dts/qcom/"
cp "$PROJECT_DIR/configs/sailfish_early.fragment" "$KERNEL_DIR/arch/arm64/configs/"

# Ensure Makefile has dtb entry
if ! grep -q "msm8996pro-google-sailfish.dtb" "$KERNEL_DIR/arch/arm64/boot/dts/qcom/Makefile"; then
    sed -i '/msm8996pro-xiaomi-natrium.dtb/i dtb-$(CONFIG_ARCH_QCOM)	+= msm8996pro-google-sailfish.dtb' "$KERNEL_DIR/arch/arm64/boot/dts/qcom/Makefile"
fi

# 3. Configure kernel if .config doesn't exist
mkdir -p "$PROJECT_DIR/out"
cd "$KERNEL_DIR"
if [ ! -f ".config" ]; then
    echo "=== Configuring Kernel ==="
    ARCH=arm64 ./scripts/kconfig/merge_config.sh arch/arm64/configs/defconfig arch/arm64/configs/sailfish_early.fragment
    make olddefconfig ARCH=arm64 LLVM=1
fi

# 4. Build kernel and DTB
echo "=== Building Kernel Image.gz & DTB ==="
make -j$(nproc) ARCH=arm64 LLVM=1 Image.gz qcom/msm8996pro-google-sailfish.dtb
cd "$PROJECT_DIR"

# 5. Build minimal test initramfs if not already present
if [ ! -f "$PROJECT_DIR/out/initramfs.cpio.gz" ]; then
    echo "=== Building Minimal initramfs ==="
    mkdir -p "$PROJECT_DIR/out/initramfs_root"
    clang -target aarch64-linux-gnu -static -O2 "$PROJECT_DIR/src/init.c" -o "$PROJECT_DIR/out/initramfs_root/init"
    chmod +x "$PROJECT_DIR/out/initramfs_root/init"
    (cd "$PROJECT_DIR/out/initramfs_root" && find . | cpio -o -H newc | gzip -9 > "$PROJECT_DIR/out/initramfs.cpio.gz")
fi

# 6. Package boot.img
echo "=== Packaging boot.img ==="
cp "$KERNEL_DIR/arch/arm64/boot/dts/qcom/msm8996pro-google-sailfish.dtb" "$PROJECT_DIR/out/msm8996pro-google-sailfish.dtb"
cat "$KERNEL_DIR/arch/arm64/boot/Image.gz" "$PROJECT_DIR/out/msm8996pro-google-sailfish.dtb" > "$PROJECT_DIR/out/Image.gz-dtb"

python3 "$PROJECT_DIR/tools/mkbootimg.py" \
  --kernel "$PROJECT_DIR/out/Image.gz-dtb" \
  --ramdisk "$PROJECT_DIR/out/initramfs.cpio.gz" \
  --dtb "$PROJECT_DIR/out/msm8996pro-google-sailfish.dtb" \
  --base 0x80000000 \
  --kernel_offset 0x00080000 \
  --ramdisk_offset 0x02200000 \
  --tags_offset 0x02000000 \
  --pagesize 4096 \
  --cmdline "earlycon=msm_serial_dm,0x75b0000 console=ttyMSM0,115200n8 androidboot.hardware=sailfish root=/dev/ram0 rw init=/init" \
  -o "$PROJECT_DIR/out/boot.img"

echo "=== Build Complete: out/boot.img ($(ls -lh "$PROJECT_DIR/out/boot.img" | awk '{print $5}')) ==="
