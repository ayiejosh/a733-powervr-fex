#!/bin/sh
# One-command health/status check for the A733 PowerVR open stack work. Safe: read-only, no GPU submissions.
cd "$(dirname "$0")/../.." || exit 1
fail=0
echo "== driver bound =="
drv=$(basename "$(readlink /sys/bus/platform/devices/1800000.gpu/driver 2>/dev/null)" 2>/dev/null)
echo "bound: $drv (expect pvrsrvkm when the desktop is up, powervr for open-stack tests)"
echo "== open firmware integrity =="
want=1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2
got=$(sha256sum /lib/firmware/powervr/rogue_36.56.104.183_v1.fw 2>/dev/null | cut -d' ' -f1)
[ "$got" = "$want" ] && echo "firmware: OK ($got)" || { echo "firmware: MISMATCH got=$got want=$want"; fail=1; }
echo "== kernel faults since boot =="
n=$(sudo dmesg | grep -icE "unable to handle|Internal error|Oops|GPU has locked up")
[ "$n" = 0 ] && echo "faults: 0" || { echo "faults: $n (inspect)"; fail=1; }
echo "== shim probe (no hardware) =="
if cd /home/radxa/mesa/mesa-main 2>/dev/null; then
  out=$(PVR_SHIM_DEVICE_BVNC=36.56.104.183 LD_PRELOAD=$PWD/build/src/imagination/drm-shim/libpowervr_noop_drm_shim.so ./build/src/gallium/drivers/pvr_native/pvr-probe 2>&1 | tail -1)
  echo "shim: $out"
  case "$out" in *"ALL PASS"*) ;; *) fail=1 ;; esac
  cd - >/dev/null
else
  echo "mesa tree not found"; fail=1
fi
echo "== published open firmware for this BVNC =="
sh tools/firmware/check-open-firmware.sh | tail -1
echo "== desktop =="
pgrep -x kwin_x11 >/dev/null && echo "kwin: alive" || echo "kwin: not running (expected while testing on the open driver)"
[ "$fail" = 0 ] && echo "RESULT: OK" || echo "RESULT: ATTENTION NEEDED"
