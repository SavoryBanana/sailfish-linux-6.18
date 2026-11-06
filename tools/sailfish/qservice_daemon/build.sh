#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STUBS_DIR="/tmp/android_stubs"
mkdir -p "$STUBS_DIR"

cat << 'EOF' > "$STUBS_DIR/libc.c"
void write(void) {}
void pause(void) {}
void exit(int status) {}
int fork(void) { return 0; }
EOF

cat << 'EOF' > "$STUBS_DIR/libdl.c"
void dlopen(void) {}
void dlsym(void) {}
void dlerror(void) {}
EOF

clang -shared -nostdlib -fPIC -Wl,-soname,libc.so --target=aarch64-linux-android34 "$STUBS_DIR/libc.c" -o "$STUBS_DIR/libc.so" -fuse-ld=lld
clang -shared -nostdlib -fPIC -Wl,-soname,libdl.so --target=aarch64-linux-android34 "$STUBS_DIR/libdl.c" -o "$STUBS_DIR/libdl.so" -fuse-ld=lld

clang -O2 -pie -fPIE -ffreestanding -nostdlib \
    "$DIR/qservice_daemon.c" \
    -o "$DIR/qservice_daemon" \
    --target=aarch64-linux-android34 -fuse-ld=lld \
    -Wl,-dynamic-linker,/system/bin/linker64 \
    -L"$STUBS_DIR" -lc -ldl

gzip -c "$DIR/qservice_daemon" | base64 -w0 > "$DIR/qservice.b64"
echo "[+] Successfully built $DIR/qservice_daemon and $DIR/qservice.b64"

