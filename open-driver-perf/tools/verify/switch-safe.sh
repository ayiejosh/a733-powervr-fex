#!/bin/sh
# switch-safe.sh open|vendor - switch GPU driver only with no userspace clients holding /dev/dri.
# Order matters: the vendor driver crashes (psConnectionPriv NULL) if loaded/unloaded while X/kwin still
# has GPU connections, and repeated hot swaps have rebooted this board. Verify each step; abort loudly.
set -u
target=${1:?usage: switch-safe.sh open|vendor}
drv() { readlink /sys/bus/platform/devices/1800000.gpu/driver 2>/dev/null | xargs basename 2>/dev/null; }
stop_clients() {
   sudo systemctl stop display-manager 2>/dev/null
   sudo systemctl stop kmsconvt@tty1 2>/dev/null   # console KMS client also holds /dev/dri/card0
   sleep 2
   for p in kwin_x11 kwin_wayland startplasma-x11 ksmserver plasmashell sddm sddm-helper X Xorg Xwayland weston; do
      sudo pkill -x "$p" 2>/dev/null
   done
   sleep 2
   # kmscon is fine to kill; anything else (X/kwin/plasma) is a blocker for the vendor driver race.
   if pgrep -x X >/dev/null || pgrep -x kwin_x11 >/dev/null || pgrep -x startplasma-x11 >/dev/null; then
      echo "ABORT: X/kwin/plasma still alive"; exit 1
   fi
   # A GPU connection still open while pvrsrvkm is removed NULL-derefs in the vendor kernel module
   # (LinuxServicesConnectionFromFile, seen 2026-10-30 03:08 -> Oops -> hard reset). Wait for every
   # client fd on the DRM nodes to close, then give the driver's connection teardown a moment.
   i=0
   while [ $i -lt 20 ]; do
      busy=$(sudo fuser /dev/dri/card0 /dev/dri/card1 /dev/dri/renderD128 /dev/pvr 2>/dev/null | tr -d ' ')
      [ -z "$busy" ] && break
      sleep 0.5; i=$((i+1))
   done
   if [ -n "${busy:-}" ]; then
      echo "ABORT: GPU device still open: $(sudo fuser -v /dev/dri/card0 /dev/dri/card1 2>&1 | tail -2)"
      exit 1
   fi
   sync; sleep 1
}
case "$target" in
open)
   stop_clients
   sudo rmmod powervr 2>/dev/null   # a stale unbound module would make insmod fail with "File exists"
   if lsmod | grep -q '^pvrsrvkm '; then
      sudo rmmod pvrsrvkm || { echo "ABORT: rmmod pvrsrvkm failed"; exit 1; }
      sleep 1
   fi
   lsmod | grep -q '^pvrsrvkm ' && { echo "ABORT: pvrsrvkm still loaded"; exit 1; }
   sudo insmod /lib/modules/$(uname -r)/extra/powervr/powervr.ko || { echo "ABORT: powervr insmod failed"; exit 1; }
   sleep 1
   if [ "$(drv)" != powervr ]; then
      # Repeated unbind/rebind/rmmod cycles can leave the GPU clock/power state such that the open probe
      # reads BVNC 0.0.0.0 (firmware heuristic path fails). Vendor probe re-initialises the clocks; try it.
      echo "powervr did not bind; recovering via a vendor probe cycle"
      sudo dmesg | tail -20 | grep -q "rogue_0.0.0.0" && echo "  (saw rogue_0.0.0.0 probe attempt)"
      sudo rmmod powervr 2>/dev/null
      sudo sh -c 'echo auto > /sys/bus/platform/devices/1800000.gpu/power/control' 2>/dev/null
      sudo modprobe pvrsrvkm 2>/dev/null
      sleep 3
      if [ "$(drv)" = pvrsrvkm ]; then
         sudo rmmod pvrsrvkm
         sleep 1
         sudo insmod /lib/modules/$(uname -r)/extra/powervr/powervr.ko 2>/dev/null
         sleep 2
      fi
   fi
   if [ "$(drv)" != powervr ]; then
      # Do not leave the board without a driver/desktop: restore the vendor stack and fail loudly.
      sudo rmmod powervr 2>/dev/null
      sudo modprobe pvrsrvkm 2>/dev/null
      sleep 2
      sudo systemctl start display-manager 2>/dev/null
      echo "ABORT: open driver will not bind after a vendor probe cycle (left board on vendor+desktop); a clean reboot is needed for the open stack"
      exit 1
   fi
   ;;
vendor)
   stop_clients
   if lsmod | grep -q '^powervr '; then
      sudo rmmod powervr || { echo "ABORT: rmmod powervr failed"; exit 1; }
      sleep 1
   fi
   lsmod | grep -q '^powervr ' && { echo "ABORT: powervr still loaded"; exit 1; }
   sudo modprobe pvrsrvkm || { echo "ABORT: modprobe pvrsrvkm failed"; exit 1; }
   # Let the vendor module finish init (firmware load, drm registration) before any client opens the
   # GPU: a client opening during init has NULL-derefed in LinuxServicesConnectionFromFile and
   # hard-reset this board twice (03:08 and 04:15 on 2026-10-30, both on the restore path).
   i=0
   while [ $i -lt 40 ]; do
      sudo dmesg | tail -40 | grep -q "Initialized pvr" && break
      sleep 0.5; i=$((i+1))
   done
   sleep 3
   [ "$(drv)" = pvrsrvkm ] || { echo "ABORT: not bound to pvrsrvkm ($(drv))"; exit 1; }
   sudo systemctl start display-manager
   ;;
*) echo "usage: switch-safe.sh open|vendor"; exit 2 ;;
esac
echo "switched to $target (bound $(drv))"
