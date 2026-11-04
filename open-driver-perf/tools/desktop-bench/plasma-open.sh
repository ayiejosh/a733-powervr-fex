#!/bin/bash
# Plasma Wayland on the open PowerVR stack, run by daily.sh (DESKTOP=plasma) in a transient logind
# session on tty2. The open Mesa comes from the system install (/opt/mesa-open, switched on by
# switch-safe.sh open through open-stack-libs.sh), not from environment variables, so the real
# /usr/bin/kwin_wayland (cap_sys_nice, secure mode) loads it the same way a daily session would.
# kwin opens GBM on the display device (card0, sunxi-drm); with no driver override Mesa falls back to
# kmsro, which renders with zink on the PowerVR and scans out from KMS dumb buffers.
export KWIN_DRM_DEVICES=/dev/dri/card0
export KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1
export PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1
export XDG_RUNTIME_DIR=/run/user/$(id -u)
# The vendor-zink X11 session leaves its settings in the systemd user manager, and startplasma imports
# that environment; with MESA_LOADER_DRIVER_OVERRIDE=zink, GBM on card0 fails. daily.sh restores the
# manager's environment at teardown.
VENDOR_VARS="MESA_LOADER_DRIVER_OVERRIDE GALLIUM_DRIVER LIBGL_KOPPER_DRI2 VK_ICD_FILENAMES VK_INSTANCE_LAYERS VK_LAYER_PATH PVR_FAKE_GS PVR_FAKE_FILL QT_XCB_GL_INTEGRATION LD_LIBRARY_PATH LIBGL_DRIVERS_PATH"
unset WAYLAND_DISPLAY DISPLAY LIBSEAT_BACKEND $VENDOR_VARS
systemctl --user unset-environment $VENDOR_VARS
systemctl --user set-environment PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
   KWIN_DRM_DEVICES="$KWIN_DRM_DEVICES" KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1
# Plasma 6 normally starts kwin_wayland and plasmashell as systemd user services; a throwaway config
# dir ahead of /etc/xdg turns that off for this session only, so they are children of this script and
# daily.sh can stop them with the unit.
CFG=/tmp/daily-plasma-xdg; mkdir -p $CFG
printf '[General]\nsystemdBoot=false\n' > $CFG/startkderc
export XDG_CONFIG_DIRS=$CFG:/etc/xdg
env -0 > /tmp/daily-open.env
exec /usr/lib/aarch64-linux-gnu/libexec/plasma-dbus-run-session-if-needed /usr/bin/startplasma-wayland
