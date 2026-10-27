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
}
case "$target" in
open)
   stop_clients
   if lsmod | grep -q '^pvrsrvkm '; then
      sudo rmmod pvrsrvkm || { echo "ABORT: rmmod pvrsrvkm failed"; exit 1; }
      sleep 1
   fi
   lsmod | grep -q '^pvrsrvkm ' && { echo "ABORT: pvrsrvkm still loaded"; exit 1; }
   sudo insmod /lib/modules/$(uname -r)/extra/powervr/powervr.ko || { echo "ABORT: powervr insmod failed"; exit 1; }
   sleep 1
   [ "$(drv)" = powervr ] || { echo "ABORT: not bound to powervr ($(drv))"; exit 1; }
   ;;
vendor)
   stop_clients
   if lsmod | grep -q '^powervr '; then
      sudo rmmod powervr || { echo "ABORT: rmmod powervr failed"; exit 1; }
      sleep 1
   fi
   lsmod | grep -q '^powervr ' && { echo "ABORT: powervr still loaded"; exit 1; }
   sudo modprobe pvrsrvkm || { echo "ABORT: modprobe pvrsrvkm failed"; exit 1; }
   sleep 2
   [ "$(drv)" = pvrsrvkm ] || { echo "ABORT: not bound to pvrsrvkm ($(drv))"; exit 1; }
   sudo systemctl start display-manager
   ;;
*) echo "usage: switch-safe.sh open|vendor"; exit 2 ;;
esac
echo "switched to $target (bound $(drv))"
