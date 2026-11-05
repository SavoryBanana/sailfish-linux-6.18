#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
clang -O2 -shared -fPIC -ffreestanding -nostdlib \
    "$DIR/libEGL_adreno_shim.c" \
    -o "$DIR/libEGL_adreno_shim.so" \
    --target=aarch64-linux-android34 -fuse-ld=lld
gzip -c "$DIR/libEGL_adreno_shim.so" | base64 -w0 > "$DIR/shim.b64"
echo "[+] Built $DIR/libEGL_adreno_shim.so and $DIR/shim.b64"

