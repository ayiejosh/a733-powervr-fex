#!/bin/sh
# apercel-stack.sh up|down - switch between the vendor stack and the apercel out-of-tree open driver
# (CheezeCS/apercel branch upstream-fixes) used to test the newest drm/imagination + the patched Mesa.
# Kernel module built for this board's 6.6.98 kernel in /home/radxa/apercel/driver/apercel.ko;
# firmware goes to /lib/firmware/apercel/rogue_36.56.104.183_v1.fw (the genuine IMG file, sha256 1db1c399...).
set -u
FW=/lib/firmware/apercel/rogue_36.56.104.183_v1.fw
case "${1:?usage: apercel-stack.sh up|down}" in
up)
   sh "$(dirname "$0")/switch-safe.sh" open >/dev/null 2>&1 || true
   sudo systemctl stop display-manager 2>/dev/null
   sudo systemctl stop kmsconvt@tty1 2>/dev/null
   sleep 3
   for p in kwin_x11 X startplasma-x11; do sudo pkill -x "$p" 2>/dev/null; done
   sleep 2
   sudo rmmod powervr 2>/dev/null
   sudo rmmod pvrsrvkm 2>/dev/null
   sleep 1
   sudo install -d /lib/firmware/apercel
   [ -f "$FW" ] || { echo "missing $FW (copy the genuine IMG firmware there)"; exit 1; }
   sudo insmod /home/radxa/apercel/driver/apercel.ko
   sleep 1
   readlink /sys/bus/platform/devices/1800000.gpu/driver
   ;;
down)
   sudo rmmod apercel 2>/dev/null
   sleep 1
   sudo modprobe pvrsrvkm
   sleep 2
   sudo systemctl start display-manager
   sleep 10
   readlink /sys/bus/platform/devices/1800000.gpu/driver
   pgrep -x kwin_x11 >/dev/null && echo kwin-alive || echo no-kwin
   ;;
*) echo "usage: apercel-stack.sh up|down"; exit 2 ;;
esac
