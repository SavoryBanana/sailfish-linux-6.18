#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SAILFISH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$SAILFISH_DIR/../.." && pwd)"
NDK_BIN="$REPO_ROOT/mesa/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC64="$NDK_BIN/aarch64-linux-android34-clang"

"$CC64" -Os -s -pie -fPIE -Wall -Wextra -Werror \
    -Wl,--as-needed \
    -o "$SCRIPT_DIR/audio_hal" \
    "$SCRIPT_DIR/audio_hal.c" \
    -ldl
