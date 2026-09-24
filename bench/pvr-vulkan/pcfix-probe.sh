#!/bin/bash
# pcfix-probe.sh - does translating the 64-bit split ops natively fix push constants?
set -u
cd "$(dirname "$0")"

echo "=== pctest (expect 6/6 PASS) ==="
./pctest 2>&1 | grep -vE "^MESA: warning|^WARNING:"
echo
echo "=== bda (expect 9/9 PASS) ==="
./bda 2>&1 | grep -E "correct|PASS|FAIL" | tail -12
echo
echo "=== regressions ==="
./vktest 3 262144 2>&1 | tail -2
for s in 512 1024; do ./vkrender "$s" 4 2>&1 | grep -E "RESULT|VERDICT" | tail -2; done
for n in 1 2 4; do SAMPLES=$n ./vkrender 512 4 2>&1 | grep -E "RESULT" | tail -1; done
echo
echo "=== GL / zink ==="
GL_PREFIX=/home/radxa/mesa/inst-gl/usr/local/lib/aarch64-linux-gnu
LD_LIBRARY_PATH=$GL_PREFIX LIBGL_DRIVERS_PATH=/home/radxa/mesa/gldri \
  GBM_BACKENDS_PATH=$GL_PREFIX/gbm MESA_LOADER_DRIVER_OVERRIDE=zink EGL_PLATFORM=gbm \
  DRM_RENDER_NODE=/dev/dri/renderD128 MESA_GLES_VERSION_OVERRIDE=3.2 \
  ./glheadless 512 20 2>&1 | grep -E "GL_RENDERER|RESULT|VERDICT"
echo
echo "=== was the 'Infinite opt loop' warning triggered? ==="
for t in pctest bda; do
  n=$(./$t 2>&1 | grep -c "Infinite opt loop" || true)
  echo "  $t: $n warnings"
done
