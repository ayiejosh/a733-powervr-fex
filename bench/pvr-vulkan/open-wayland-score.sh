#!/bin/bash
# open-wayland-score.sh - the open driver's GL score, via weston + Xwayland, gate-checked.
#
# glmark2-es2 is the X11 variant, so this speaks the SAME protocol as the vendor arm:
#   vendor : X server on pvrsrvkm                       (glmark2-es2 -s 800x600, DISPLAY=:0)
#   open   : Xwayland on weston on powervr              (same command, through wclient26.sh)
#
# RUN AS ROOT. Refuses to switch unless pvrsrvkm's refcount is 0 - a non-zero refcount makes
# rmmod fail silently inside switch-open.sh and the driver never changes.
set -u
GS=/home/radxa/gpu-open-stack
OUT=/tmp/compat; mkdir -p $OUT
SIZE=${1:-800x600}

refcnt() { awk '$1=="pvrsrvkm"{print $3}' /proc/modules 2>/dev/null; }

restore() {
  pkill -x glmark2-es2 2>/dev/null; pkill -x weston 2>/dev/null; sleep 2
  local d; d=$(readlink -f /sys/bus/platform/devices/1800000.gpu/driver 2>/dev/null | xargs basename)
  if [ "$d" != "pvrsrvkm" ]; then
    for i in $(seq 1 30); do [ "$(awk '$1=="powervr"{print $3}' /proc/modules)'"''" ]" = 0 ] && break; sleep 1; done
    echo 1800000.gpu > /sys/bus/platform/drivers/powervr/unbind 2>/dev/null; sleep 2
    rmmod powervr 2>/dev/null; sleep 2; modprobe pvrsrvkm 2>/dev/null; sleep 4
  fi
  systemctl start display-manager >/dev/null 2>&1; sleep 16
  echo "[restore] bound=$(readlink -f /sys/bus/platform/devices/1800000.gpu/driver | xargs basename) kwin=$(pgrep -x kwin_x11 >/dev/null && echo ALIVE || echo absent)"
}
trap restore EXIT

echo "=== stop the desktop ==="
systemctl stop display-manager >/dev/null 2>&1; sleep 3
pkill -x kwin_x11 2>/dev/null; pkill -x weston 2>/dev/null; pkill -x Xwayland 2>/dev/null; pkill -x X 2>/dev/null
for i in $(seq 1 20); do pgrep -x kwin_x11 >/dev/null || break; sleep 1; done
pgrep -x kwin_x11 >/dev/null && { echo "ABORT: kwin alive"; exit 1; }

echo "=== GATE: wait for pvrsrvkm's refcount to reach 0 ==="
for i in $(seq 1 60); do
  rc=$(refcnt); rc=${rc:-0}
  [ "$rc" = "0" ] && { echo "  refcount 0 after ${i}s"; break; }
  [ $((i % 10)) -eq 0 ] && echo "  ${i}s: refcount $rc"
  sleep 1
done
RC=$(refcnt); RC=${RC:-0}
if [ "$RC" != "0" ]; then
  echo "ABORT: pvrsrvkm refcount is $RC after 60s - rmmod would fail silently."
  echo "       A stuck refcount clears on REBOOT. Not switching."
  exit 1
fi

echo "=== switch to OPEN ==="
"$GS/switch-open.sh" 2>&1 | tail -3 | sed 's/^/  /'
sleep 5
d=$(readlink -f /sys/bus/platform/devices/1800000.gpu/driver | xargs basename)
[ "$d" = "powervr" ] || { echo "ABORT: bound=$d after switch"; exit 1; }
echo "  bound=powervr"

echo "=== weston + Xwayland on the open stack (w26x.sh needs root) ==="
rm -f $GS/w26x.log 2>/dev/null
setsid "$GS/w26x.sh" >/dev/null 2>&1 &
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-open
for i in $(seq 1 30); do [ -e "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && break; sleep 1; done
echo "  wayland socket : $([ -e "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && echo present || echo MISSING)"
for i in $(seq 1 25); do [ -e /tmp/.X11-unix/X0 ] && break; sleep 1; done
echo "  Xwayland :0    : $([ -e /tmp/.X11-unix/X0 ] && echo present || echo MISSING)"
sleep 4

echo "=== glmark2-es2 on the OPEN driver, via Xwayland ($SIZE) ==="
# env26.sh sets MESA_LOADER_DRIVER_OVERRIDE=sun4i-drm, which is right for WESTON (kmsro display)
# and wrong for a CLIENT. wclient.sh - the client wrapper - sets GALLIUM_DRIVER=zink and no override.
# Try the client-appropriate env first, then fall back to the wclient26 wrapper.
( . "$GS/env26.sh"
  unset MESA_LOADER_DRIVER_OVERRIDE
  export GALLIUM_DRIVER=zink DISPLAY=:0
  echo "# client env: GALLIUM_DRIVER=zink, no loader override" >&2
  timeout 1200 glmark2-es2 -s "$SIZE" ) >$OUT/open-xwayland.log 2>&1
RC=$?
if ! grep -q "glmark2 Score" $OUT/open-xwayland.log; then
  echo "  (retrying via wclient26.sh)" >&2
  DISPLAY=:0 timeout 1200 "$GS/wclient26.sh" glmark2-es2 -s "$SIZE" >>$OUT/open-xwayland.log 2>&1
  RC=$?
fi
true
echo "  exit=$RC"
grep -E "GL_RENDERER|GL_VERSION|glmark2 Score" $OUT/open-xwayland.log | sed 's/^/  /'
echo "  scenes with FPS: $(grep -c 'FPS:' $OUT/open-xwayland.log)   errors: $(grep -c 'Error:' $OUT/open-xwayland.log)"
grep -E "Error:" $OUT/open-xwayland.log | head -4 | sed 's/^/    /'
