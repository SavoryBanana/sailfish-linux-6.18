#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

BOOT_IMG="$KERNEL_DIR/out/boot.img"

if [ ! -f "$BOOT_IMG" ]; then
    echo "Error: $BOOT_IMG not found. Run ./tools/sailfish/build.sh first."
    exit 1
fi

echo "=============================================="
echo " Google Pixel (sailfish) Kernel 6.18 Boot Test"
echo "=============================================="

echo "Waiting for fastboot device..."
fastboot wait-for-device

echo "Found device in fastboot mode:"
fastboot devices

echo ""
echo "Sending out/boot.img into RAM (one-shot boot)..."
fastboot boot "$BOOT_IMG"

echo ""
echo "Boot image transferred! Watch the phone screen."
echo "If the phone reboots, you can extract the ramoops log with:"
echo "  fastboot oem ramoops"
echo "=============================================="
