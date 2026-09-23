#!/bin/bash
# Run any command against the open (mainline powervr + Mesa pvr) stack, safely.
#
# Swaps the GPU module, runs the command, restores the desktop - the same shape as
# the benchmark script but with no display phase at all, so it is safe to use for
# pure compute/memory probes. Restore is armed on a timer so a failure here cannot
# leave the desktop down.
#
#   ./open-run.sh <command> [args...]
set -u

SPIKE=/home/radxa/kspike
LOG=$SPIKE/open-run-$(date +%Y%m%d-%H%M%S).log
MESA_ICD=${MESA_ICD:-/home/radxa/mesa/mesa-main/build-x11/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json}

say() { echo "[open-run $(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

restore() {
    say "--- RESTORE ---"
    rmmod powervr 2>/dev/null
    rmmod drm_gpuvm 2>/dev/null
    sleep 1
    modprobe pvrsrvkm
    for _ in $(seq 1 15); do sleep 2; [ -e /dev/dri/card1 ] && break; done
    if [ ! -e /dev/dri/card1 ]; then
        say "card1 missing - reloading the vendor module cleanly"
        rmmod pvrsrvkm 2>/dev/null; sleep 2; modprobe pvrsrvkm
        for _ in $(seq 1 15); do sleep 2; [ -e /dev/dri/card1 ] && break; done
    fi
    if ! systemctl is-active --quiet kmsconvt@tty1; then systemctl start kmsconvt@tty1 2>/dev/null; fi
    systemctl start display-manager
    for _ in $(seq 1 30); do sleep 2; pgrep -x kwin_x11 >/dev/null && break; done
    if ! pgrep -x kwin_x11 >/dev/null; then
        systemctl restart display-manager
        for _ in $(seq 1 20); do sleep 2; pgrep -x kwin_x11 >/dev/null && break; done
    fi
    say "desktop: X=$(pgrep -c -x X) kwin=$(pgrep -c kwin_x11) plasmashell=$(pgrep -c plasmashell)"
    say "evidence: $LOG"
}
trap restore EXIT

say "log: $LOG"
say "command: $*"
say "pre-state: pvrsrvkm refs=$(awk '$1=="pvrsrvkm"{print $3}' /proc/modules) X=$(pgrep -c -x X)"

if systemctl is-active --quiet kmsconvt@tty1; then systemctl stop kmsconvt@tty1; fi
systemctl stop display-manager
for _ in $(seq 1 30); do
    if ! pgrep -x X >/dev/null && ! pgrep -x kwin_x11 >/dev/null; then break; fi
    sleep 1
done
for _p in X kwin_x11 plasmashell picom; do pkill -x "$_p" 2>/dev/null; done
sleep 2

rmmod pvrsrvkm || { say "ABORT: rmmod failed"; exit 3; }
for m in drm_exec gpu-sched drm_shmem_helper; do modprobe "$m" || say "WARNING: modprobe $m failed"; done
insmod "$SPIKE/mod/drm_gpuvm.ko" 2>/dev/null
insmod "$SPIKE/img/powervr.ko" 2>/dev/null
sleep 3
say "open driver: powervr=$(grep -c '^powervr ' /proc/modules) dri=$(ls /dev/dri | tr '\n' ' ')"

GL_PREFIX=/home/radxa/mesa/inst-gl/usr/local/lib/aarch64-linux-gnu
if [ "${1:-}" = "--gl" ]; then
    size=${2:-512}; frames=${3:-20}
    say "--- zink GL test over the open driver (${size}x${size}, $frames frames) ---"
    ( cd /home/radxa/trixie-prep/bench/pvr-vulkan && runuser -u radxa -- env HOME=/home/radxa \
        LD_LIBRARY_PATH="$GL_PREFIX" LIBGL_DRIVERS_PATH=/home/radxa/mesa/gldri \
        GBM_BACKENDS_PATH="$GL_PREFIX/gbm" \
        MESA_LOADER_DRIVER_OVERRIDE=zink EGL_PLATFORM=gbm \
        DRM_RENDER_NODE=/dev/dri/renderD128 MESA_GLES_VERSION_OVERRIDE=3.2 \
        PVR_TIMING=1 \
        VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
        PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
        timeout 300 ./glheadless "$size" "$frames" 2>&1 \
        | grep -E 'GL_RENDERER|GL_VERSION|timing ms/frame|frame\(s\)|RESULT|VERDICT|FAIL' \
        | sed 's/^/    /' | tee -a "$LOG" )
    say "command exit: ${PIPESTATUS[0]}"
    exit 0
fi

say "--- command output ($MESA_ICD) ---"
VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
    PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 "$@" 2>&1 | sed 's/^/    /' | tee -a "$LOG"
say "command exit: ${PIPESTATUS[0]}"
