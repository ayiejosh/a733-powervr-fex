#!/bin/bash
# repeat-probe.sh - are the large-render failures deterministic?
#
# vkrender 4096 passed in one run of regress.sh and came back all-black in
# another, with the same ICD and the same command line. If the large renders are
# flaky rather than consistently broken, then a single passing run is not
# evidence and the earlier "6144/8192 PASS" needs re-checking.
#
# Runs each size three times in a row, and does the GL case properly this time
# (the previous probe passed a shell function to timeout, which cannot work).
set -u
cd "$(dirname "$0")"

GL_PREFIX=/home/radxa/mesa/inst-gl/usr/local/lib/aarch64-linux-gnu

run() {
  local label="$1"; shift
  local out rc
  out=$(timeout 300 "$@" 2>&1)
  rc=$?
  printf '  %-22s rc=%-3d %s\n' "$label" "$rc" \
    "$(printf '%s' "$out" | grep -E 'RESULT|VERDICT' | head -1 | cut -c1-72)"
}

echo "=== three repeats per size ==="
for i in 1 2 3; do
  for s in 512 4096 6144 8192; do
    run "try$i ${s}" ./vkrender "$s" 1
  done
done

echo
echo "=== GL / zink (run properly this time) ==="
run "glheadless 512x20" \
  env LD_LIBRARY_PATH="$GL_PREFIX" \
      LIBGL_DRIVERS_PATH=/home/radxa/mesa/gldri \
      GBM_BACKENDS_PATH="$GL_PREFIX/gbm" \
      MESA_LOADER_DRIVER_OVERRIDE=zink \
      EGL_PLATFORM=gbm \
      DRM_RENDER_NODE=/dev/dri/renderD128 \
      MESA_GLES_VERSION_OVERRIDE=3.2 \
      ./glheadless 512 20

echo
echo "=== push constants + buffer device addresses ==="
run "pctest" ./pctest
run "bda" ./bda

echo
echo "=== gpu state ==="
for f in /sys/class/devfreq/*/cur_freq /sys/class/devfreq/*/governor; do
  [ -e "$f" ] && echo "  $f = $(cat "$f" 2>/dev/null)"
done
free -m | head -2 | sed 's/^/  /'
