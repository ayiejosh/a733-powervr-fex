#!/bin/bash
# order-probe.sh - are the late failures in regress.sh ordering artefacts?
#
# regress.sh runs every test back-to-back and the later, heavier cases failed
# (6144/8192 renders, zink GL) while the same cases passed when run on their own.
# Either the suite accumulates GPU-side state that the driver does not release
# between processes, or the cases genuinely fail and the earlier passes were luck.
#
# This runs the sensitive cases first on a fresh driver, then again after heavy
# GPU work, so the comparison is within one boot of the module and one process
# per case - the only variable is what ran before.
set -u
cd "$(dirname "$0")"

GL_PREFIX=/home/radxa/mesa/inst-gl/usr/local/lib/aarch64-linux-gnu
gl_env() {
  env LD_LIBRARY_PATH="$GL_PREFIX" \
      LIBGL_DRIVERS_PATH=/home/radxa/mesa/gldri \
      GBM_BACKENDS_PATH="$GL_PREFIX/gbm" \
      MESA_LOADER_DRIVER_OVERRIDE=zink \
      EGL_PLATFORM=gbm \
      DRM_RENDER_NODE=/dev/dri/renderD128 \
      MESA_GLES_VERSION_OVERRIDE=3.2 \
      "$@"
}

run() {
  local label="$1"; shift
  local out rc
  out=$(timeout 300 "$@" 2>&1)
  rc=$?
  printf '%-26s rc=%-3d %s\n' "$label" "$rc" \
    "$(printf '%s' "$out" | grep -E 'VERDICT|RESULT' | head -1)"
}

echo "=== fresh driver ==="
run "glheadless 512x20" gl_env ./glheadless 512 20
run "vkrender 6144" ./vkrender 6144 1
run "vkrender 8192" ./vkrender 8192 1
run "vkrender 512 r8" env FORMAT=r8 ./vkrender 512 4
run "vkrender 512 r16" env FORMAT=r16 ./vkrender 512 4
run "vkrender 512" ./vkrender 512 4

echo
echo "=== after heavy GPU work ==="
./vktest 10 1048576 >/dev/null 2>&1
echo "  vktest 10x1M elements done"
for s in 1024 2048 4096; do ./vkrender "$s" 2 >/dev/null 2>&1; done
echo "  renders 1024/2048/4096 done"

run "vkrender 6144 again" ./vkrender 6144 1
run "vkrender 8192 again" ./vkrender 8192 1
run "glheadless again" gl_env ./glheadless 512 20
run "vkrender 512 again" ./vkrender 512 4
run "vktest again" ./vktest 3 262144

echo
echo "=== memory ==="
free -m | head -2
