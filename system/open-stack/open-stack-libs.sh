#!/bin/sh
# open-stack-libs.sh on|off|status - make the open Mesa installed in /opt/mesa-open the GL, EGL, GBM and
# Vulkan the system finds while the open powervr kernel driver is bound, and leave no trace of it while
# the vendor pvrsrvkm is. Called by switch-safe.sh on each switch and by mesa-open-boot-check.service.
#
# Programs with file capabilities, such as kwin_wayland (cap_sys_nice), run in secure mode: the dynamic
# linker ignores LD_LIBRARY_PATH and the Vulkan loader ignores VK_ICD_FILENAMES. So the switch is made in
# the system configuration, not in environment variables:
#   - /etc/ld.so.conf.d/00-mesa-open.conf puts the open Mesa, then the system library directories, ahead
#     of the vendor's /usr/local/lib (the vendor entry is 00_xserver-xorg-img-bxm.conf; '-' sorts before
#     '_'), so the vendor libEGL/libGLESv2/libgbm/libvulkan there are not used;
#   - the open Vulkan driver's ICD goes to /etc/vulkan/icd.d;
#   - /etc/environment.d/90-mesa-open.conf sets PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 for user sessions
#     (the open Vulkan driver refuses this not-yet-conformant GPU without it).
set -eu
P=/opt/mesa-open
LIB=$P/lib/aarch64-linux-gnu
CONF=/etc/ld.so.conf.d/00-mesa-open.conf
ICD=/etc/vulkan/icd.d/powervr_mesa_open_icd.aarch64.json
ENVD=/etc/environment.d/90-mesa-open.conf
case "${1:-status}" in
on)
   [ -e $LIB/libgbm.so.1 ] && [ -e $P/share/vulkan/icd.d/powervr_mesa_icd.aarch64.json ] ||
      { echo "open-stack-libs: no open Mesa installed in $P" >&2; exit 1; }
   printf '%s\n' $LIB /lib/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu > $CONF
   mkdir -p /etc/vulkan/icd.d
   cp $P/share/vulkan/icd.d/powervr_mesa_icd.aarch64.json $ICD
   printf 'PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1\n' > $ENVD
   ldconfig
   ;;
off)
   rm -f $CONF $ICD $ENVD
   ldconfig
   ;;
status)
   for f in $CONF $ICD $ENVD; do
      if [ -e $f ]; then echo "on  $f"; else echo "off $f"; fi
   done
   ;;
*)
   echo "usage: open-stack-libs.sh on|off|status" >&2
   exit 2
   ;;
esac
