#!/bin/bash
# compat-open.sh - BATCH B: the open driver's compatibility/GL run, with a guaranteed return.
#
# Batch A (vendor) runs with the normal desktop up. Batch B has to bring its own display,
# because the desktop is kwin/X on pvrsrvkm. Weston on the open stack is what this project
# already uses for that (w26x.sh + env26.sh).
#
#   sudo ./compat-open.sh [size]
#
# ponytail: one file, one trap, no state carried between runs. If it dies, EXIT still restores
# the vendor driver and the desktop.
set -u
SIZE=${1:-800x600}
OUT=/tmp/compat
mkdir -p $OUT
SW=/home/radxa/gpu-open-stack/switch-open.sh
W26=/home/radxa/gpu-open-stack/w26x.sh
ENV26=/home/radxa/gpu-open-stack/env26.sh

restore() {
  local d
  d=$(readlink -f /sys/bus/platform/devices/1800000.gpu/driver 2>/dev/null | xargs basename)
  pkill -x weston 2>/dev/null; sleep 2
  if [ "$d" != "pvrsrvkm" ]; then
    echo 1800000.gpu > /sys/bus/platform/drivers/powervr/unbind 2>/dev/null; sleep 2
    rmmod powervr 2>/dev/null; sleep 2
    modprobe pvrsrvkm 2>/dev/null; sleep 4
  fi
  systemctl start display-manager >/dev/null 2>&1
  sleep 12
  echo "[restore] bound=$(readlink -f /sys/bus/platform/devices/1800000.gpu/driver | xargs basename) \
kwin=$(pgrep -x kwin_x11 >/dev/null && echo ALIVE || echo absent)"
}
trap 'echo "[trap] restoring the vendor driver and the desktop"; restore' EXIT

die() { echo "ABORT: $*" >&2; exit 1; }
[ -x "$SW" ] || die "switch script not found: $SW"

echo "=== switching to the OPEN driver ==="
systemctl stop display-manager >/dev/null 2>&1; sleep 3
pkill -x weston 2>/dev/null; pkill -x Xwayland 2>/dev/null; pkill -x X 2>/dev/null; sleep 2
pgrep -x kwin_x11 >/dev/null && die "kwin still alive - refusing to unbind the GPU"
"$SW" >/dev/null 2>&1 || true          # exits non-zero on a redundant write; the NAME is the check
sleep 5
d=$(readlink -f /sys/bus/platform/devices/1800000.gpu/driver 2>/dev/null | xargs basename)
[ "$d" = "powervr" ] || die "switch failed (bound: $d)"
echo "  bound: powervr   chain_frag=$(cat /sys/module/powervr/parameters/chain_frag) \
combine_pr=$(cat /sys/module/powervr/parameters/combine_pr)"

echo "=== bringing up weston on the open stack (via w26x.sh, which owns the env) ==="
# w26x.sh sources env26.sh (XDG_RUNTIME_DIR, WAYLAND_DISPLAY=wayland-open, the Mesa 26 lib paths,
# the open ICD) and execs weston with --xwayland. Re-deriving that here is how the first attempt
# failed: env26.sh is a source-me file, not executable, and sudo strips XDG_RUNTIME_DIR.
[ -r "$W26" ] || die "missing: $W26"
setsid "$W26" >$OUT/weston-open.log 2>&1 &
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-open
for i in $(seq 1 25); do
  [ -e "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && break
  sleep 1
done
echo "  WAYLAND_DISPLAY=$WAYLAND_DISPLAY  socket: $([ -e "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && echo present || echo MISSING)"
sleep 4

# glmark2 runs in THIS shell, so it needs env26.sh too - w26x.sh only set it for weston.
# Without this, glmark2 picks the system Mesa 24.0.1 and falls back to softpipe (score 11, meaningless).
. "$ENV26"

echo "=== glmark2-es2 on the OPEN driver ($SIZE) ==="
timeout 1200 glmark2-es2 -s "$SIZE" >$OUT/open-fresh.log 2>&1
echo "  exit: $? (124 = timeout)"

echo "=== result ==="
grep -E "GL_RENDERER|GL_VERSION|glmark2 Score" $OUT/open-fresh.log 2>/dev/null | sed 's/^/  /'
n=$(grep -c "FPS:" $OUT/open-fresh.log 2>/dev/null)
e=$(grep -c "Error:" $OUT/open-fresh.log 2>/dev/null)
echo "  scenes with FPS: $n   errors: $e"
grep -E "Error:" $OUT/open-fresh.log 2>/dev/null | head -5 | sed 's/^/    /'
