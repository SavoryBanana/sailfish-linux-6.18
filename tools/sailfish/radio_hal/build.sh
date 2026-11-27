#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SAILFISH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$SAILFISH_DIR/../.." && pwd)"
CC="$REPO_ROOT/mesa/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang"

"$CC" -Os -s -pie -fPIE -Wall -Wextra -Werror \
    -o "$SCRIPT_DIR/sailfish_qmi_test" \
    "$SCRIPT_DIR/sailfish_qmi_test.c"

"$CC" -Os -s -pie -fPIE -Wall -Wextra -Werror \
    -Wl,--as-needed \
    -o "$SCRIPT_DIR/sailfish_radio_hal" \
    "$SCRIPT_DIR/sailfish_radio_hal.c" \
    -ldl
