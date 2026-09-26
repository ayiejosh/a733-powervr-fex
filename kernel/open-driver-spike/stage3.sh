#!/bin/bash
# stage3.sh - bind test: open PowerVR driver (powervr) against the vendor A733 GPU node.
# PRECONDITION: pvrsrvkm must be unloaded - it currently owns the node (img,gpu).
#   sudo systemctl disable pvrsrvkm-load.service && sudo reboot     # then run this
# Revert: rmmod powervr; rmmod drm_gpuvm; modprobe pvrsrvkm
set -u
S=/home/radxa/kspike; cd $S
say(){ echo "[stage3 $(date +%H:%M:%S)] $*"; }

if grep -q "^pvrsrvkm " /proc/modules; then
  say "ABORT: pvrsrvkm is loaded (refs $(awk '$1=="pvrsrvkm"{print $3}' /proc/modules)) - it owns the GPU node"
  say "       disable pvrsrvkm-load.service and reboot, then re-run this script"
  exit 2
fi
if [ "$(id -u)" != 0 ]; then say "ABORT: run with sudo"; exit 2; fi

say "loading dependencies (drm_exec is CONFIG_DRM_EXEC=m on this kernel)"
modprobe drm_exec            || { say "modprobe drm_exec FAILED"; exit 1; }
insmod $S/mod/drm_gpuvm.ko   || { say "insmod drm_gpuvm FAILED"; exit 1; }
say "deps ok: drm_exec + backported drm_gpuvm"

say "binding attempt: insmod powervr.ko"
insmod $S/img/powervr.ko 2>&1 | sed 's/^/           /'
sleep 3

if grep -q "^powervr " /proc/modules; then say "module loaded"; else say "module did NOT load"; fi
say "--- kernel log ---"
dmesg | grep -iE "powervr|pvr" | tail -25 | sed 's/^/    /'
say "--- DRM nodes ---"
for d in /sys/kernel/debug/dri/*/name; do printf '    %s: %s\n' "$(basename $(dirname $d))" "$(cat $d)"; done 2>/dev/null
say "--- ACCEPTANCE ---"
if dmesg | grep -q "Initialized powervr"; then
  say "PASS: the open driver bound the GPU node"
  say "      next: Mesa with -Dvulkan-drivers=imagination (Debian ships none)"
else
  say "NOT BOUND - read the log above (missing DT clocks/power-domains is the likely cause)"
fi
say "revert: rmmod powervr; rmmod drm_gpuvm; modprobe pvrsrvkm"
