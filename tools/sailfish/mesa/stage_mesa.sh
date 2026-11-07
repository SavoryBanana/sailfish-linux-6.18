#!/bin/bash
# Stage Mesa Freedreno (gallium/msm) EGL/GLES libs for the sailfish Android container.
#
# Expects a Mesa NDK build at $MESA_BUILD (default: <repo>/mesa/build-fd), configured with:
#   -Dplatforms=android -Dandroid-stub=true -Dgallium-drivers=freedreno -Dfreedreno-kmds=msm
#   -Degl-lib-suffix=_mesa -Dgles-lib-suffix=_mesa -Dvulkan-drivers=
#
# The libs are renamed to the stock Adreno vendor names so Android's libEGL loader
# (ro.hardware.egl=adreno) picks them up from /vendor/lib64/egl/ unchanged.
# DT_RUNPATH is rewritten to "$ORIGIN" so libgallium_dri.so is found next to them.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
MESA_BUILD="${MESA_BUILD:-$REPO_DIR/mesa/build-fd}"
NDK_BIN="${NDK_BIN:-$REPO_DIR/mesa/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin}"
OUT="$SCRIPT_DIR/lib64"

mkdir -p "$OUT"
cp "$MESA_BUILD/src/egl/libEGL_mesa.so"                     "$OUT/libEGL_adreno.so"
cp "$MESA_BUILD/src/mapi/es2api/libGLESv2_mesa.so"          "$OUT/libGLESv2_adreno.so"
cp "$MESA_BUILD/src/mapi/es1api/libGLESv1_CM_mesa.so"       "$OUT/libGLESv1_CM_adreno.so"
cp "$MESA_BUILD/src/gallium/targets/dri/libgallium_dri.so"  "$OUT/libgallium_dri.so"

for f in "$OUT"/*.so; do
    "$NDK_BIN/llvm-strip" --strip-unneeded "$f"
    python3 - "$f" <<'EOF'
import sys, re
p = sys.argv[1]
data = bytearray(open(p, 'rb').read())
# Find the build-tree RUNPATH string ($ORIGIN/...:...) and overwrite in place with "$ORIGIN".
m = re.search(rb'\$ORIGIN/[^\x00]*\x00', data)
if not m:
    sys.exit(f"no RUNPATH in {p}")
s, e = m.span()
new = b'$ORIGIN'
data[s:e] = new + b'\x00' * (e - s - len(new))
open(p, 'wb').write(data)
print(f"[+] {p.split('/')[-1]}: RUNPATH -> $ORIGIN")
EOF
done
"$NDK_BIN/llvm-readelf" -d "$OUT/libEGL_adreno.so" | grep -E "RUNPATH|NEEDED"
ls -lh "$OUT"
