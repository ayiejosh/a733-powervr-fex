#!/bin/bash
# Rebuild the vendor GL path from scratch: Mesa 22.1.7 + Imagination's PVR Gallium frontend.
# Result: a Mesa DRI driver (pvr_dri.so) that forwards GL to the VENDOR's GLES, so GL runs at
# vendor speed without zink. Needs pvrsrvkm bound - this path cannot use the open powervr driver.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-$HOME/mesa-pvr}
PREFIX=${PREFIX:-$SRC/install}
PATCH="$HERE/0001-mesa-22.1.7-pvr-gallium-frontend-full.patch"

mkdir -p "$SRC"; cd "$SRC"
if [ ! -d mesa-22.1.7 ]; then
  git clone --depth 1 --branch mesa-22.1.7 https://gitlab.freedesktop.org/mesa/mesa.git mesa-22.1.7
fi
cd mesa-22.1.7

# The patch adds the pvr driver to meson_options.txt, so it must land BEFORE configure. The bundled
# zlib subproject does not exist until configure downloads it, so its hunks are applied after - see
# below. Excluding them here is deliberate, not an oversight.
if git apply --check --exclude='subprojects/zlib-*' "$PATCH" 2>/dev/null; then
  git apply --exclude='subprojects/zlib-*' "$PATCH"
  echo "[1/4] patch applied"
else
  echo "[1/4] patch already applied (or conflicts - check manually)"
fi

# Mesa 22.1.7's bundled zlib 1.2.11 misses <unistd.h> and gcc 14 makes implicit declarations errors.
# Use the system zlib if its headers are present; apt is broken on this board, so patch the bundled one.
zlib_fix() {
  for f in gzlib.c gzread.c gzwrite.c; do
    [ -f "$f" ] || continue
    grep -q 'include <unistd.h>' "$f" || sed -i '0,/#include/s//#include <unistd.h>\n#include/' "$f"
  done
}

[ -d build-pvr ] || meson setup build-pvr --prefix="$PREFIX" \
  -Dgallium-drivers=pvr -Dvulkan-drivers= -Dplatforms=x11,null \
  -Dglx=disabled -Dgbm=enabled -Dopengl=true -Degl=true \
  -Dgles1=disabled -Dgles2=true -Dllvm=disabled \
  -Dgallium-pvr-alias=pvr_alt
echo "[2/4] configured"

# only now does the zlib subproject exist
for d in subprojects/zlib-*; do [ -d "$d" ] && (cd "$d" && zlib_fix); done
echo "[3/4] bundled zlib patched for gcc 14"

ninja -C build-pvr
ninja -C build-pvr install
echo "[4/4] built and installed to $PREFIX"
echo
echo "Use it:  . $HERE/gl-wrapper-env.sh && DISPLAY=:0 glmark2-es2 --size 800x600"
echo "Check:   the renderer must read 'PowerVR B-Series BXM-4-64', NOT 'zink'."
