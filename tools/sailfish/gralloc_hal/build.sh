#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SAILFISH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$SAILFISH_DIR/../.." && pwd)"
NDK_BIN="$REPO_ROOT/mesa/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC64="$NDK_BIN/aarch64-linux-android34-clang"
CC32="$NDK_BIN/armv7a-linux-androideabi34-clang"

"$CC64" -Os -s -pie -fPIE -Wall -Wextra -Werror \
    -Wl,--as-needed \
    -o "$SCRIPT_DIR/allocator_hal" \
    "$SCRIPT_DIR/allocator_hal.c" \
    -ldl

"$CC64" -Os -s -shared -fPIC -Wall -Wextra -Werror \
    -Wl,-soname,mapper.sailfish.so \
    -o "$SCRIPT_DIR/mapper.sailfish.so" \
    "$SCRIPT_DIR/mapper_sailfish.c"

"$CC32" -Os -s -shared -fPIC -Wall -Wextra -Werror \
    -Wl,-soname,mapper.sailfish.so \
    -o "$SCRIPT_DIR/mapper.sailfish.so.32" \
    "$SCRIPT_DIR/mapper_sailfish.c"

STUB_INC="$REPO_ROOT/mesa/mesa-src/include/android_stub"
"$CC64" -Os -s -shared -fPIC -Wall -Wextra -Werror \
    -I"$STUB_INC" \
    -Wl,-soname,gralloc.default.so \
    -o "$SCRIPT_DIR/gralloc.default.so" \
    "$SCRIPT_DIR/gralloc_default_yuv.c" -llog

"$CC32" -Os -s -shared -fPIC -Wall -Wextra -Werror \
    -I"$STUB_INC" \
    -Wl,-soname,gralloc.default.so \
    -o "$SCRIPT_DIR/gralloc.default.so.32" \
    "$SCRIPT_DIR/gralloc_default_yuv.c" -llog
