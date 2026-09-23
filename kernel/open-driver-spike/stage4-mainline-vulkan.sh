#!/bin/bash
# stage4-mainline-vulkan.sh - drive the MAINLINE powervr driver with Mesa's pvr
# Vulkan ICD, in place of the vendor module, and put the desktop back afterwards.
#
# Why a swap is unavoidable: only one driver can own 1800000.gpu, and the vendor
# module currently owns it (~188 open references from the X session). So this
# stops the display manager, unloads the vendor module, loads the mainline
# driver, runs the same Vulkan compute test the vendor ICD already passes, then
# restores.
#
# Safety model, because a half-done swap leaves the board with no GPU driver and
# the vendor X server cannot start without one:
#   1. a systemd watchdog re-runs this script with --restore-only after 8 minutes
#      no matter what happens to the first run;
#   2. `trap restore EXIT` covers ordinary failure and Ctrl-C;
#   3. the vendor module is never unloaded until its reference count is zero;
#   4. the display manager is a system unit (display-manager.service), so it is
#      stopped and started with systemctl rather than by killing processes.
#
# The harness that is running this session lives in its own user service
# (user@1000.service/app.slice/dsh-web.service), not in the graphical session, so
# it keeps running while the desktop is down.
#
# Usage: sudo ./stage4-mainline-vulkan.sh [--restore-only]
set -u

SPIKE=/home/radxa/kspike
BENCH=/home/radxa/trixie-prep/bench/pvr-vulkan
# NB: don't pick this with `ls` — ls sorts its operands, which silently selected
# the older Mesa build by name ("25.0.7" < "25.3.0").
MESA_ICD=${MESA_ICD:-}
if [ -z "$MESA_ICD" ]; then
    for candidate in \
        /home/radxa/mesa/mesa-25.3.0/build/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json \
        /home/radxa/mesa/mesa-25.0.7/build/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json
    do
        [ -f "$candidate" ] && { MESA_ICD=$candidate; break; }
    done
fi
LOG=/home/radxa/kspike/stage4-$(date +%Y%m%d-%H%M%S).log
WATCHDOG_UNIT=stage4-restore-watchdog
RESTORED=0
RESTORE_ONLY=0
[ "${1:-}" = "--restore-only" ] && RESTORE_ONLY=1

say() { echo "[stage4 $(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

restore() {
    [ "$RESTORED" = 1 ] && return
    RESTORED=1
    say "--- RESTORE ---"
    if grep -q '^powervr ' /proc/modules; then
        rmmod powervr 2>&1 | sed 's/^/    /' | tee -a "$LOG"
    fi
    if grep -q '^drm_gpuvm ' /proc/modules; then
        rmmod drm_gpuvm 2>&1 | sed 's/^/    /' | tee -a "$LOG"
    fi
    if ! grep -q '^pvrsrvkm ' /proc/modules; then
        modprobe pvrsrvkm 2>&1 | sed 's/^/    /' | tee -a "$LOG"
        sleep 2
    fi
    if grep -q '^pvrsrvkm ' /proc/modules; then
        say "vendor module back (refs $(awk '$1=="pvrsrvkm"{print $3}' /proc/modules))"
        if ! pgrep -x X >/dev/null; then
            say "starting display-manager"
            systemctl start display-manager 2>&1 | sed 's/^/    /' | tee -a "$LOG"
            for _ in $(seq 1 30); do
                sleep 2
                pgrep -x X >/dev/null && break
            done
        fi
        if pgrep -x X >/dev/null; then
            say "X is up; kwin=$(pgrep -c kwin_x11) plasmashell=$(pgrep -c plasmashell) picom=$(pgrep -c picom)"
        else
            say "X did NOT come back - reboot needed"
        fi
    else
        say "CRITICAL: pvrsrvkm did not load - a reboot is needed"
    fi
    if [ "$RESTORE_ONLY" != 1 ]; then
        systemctl stop "$WATCHDOG_UNIT" 2>/dev/null
    fi
    say "final module state: pvrsrvkm=$(grep -c '^pvrsrvkm ' /proc/modules) powervr=$(grep -c '^powervr ' /proc/modules) drm_gpuvm=$(grep -c '^drm_gpuvm ' /proc/modules)"
}
trap restore EXIT

[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 2; }

if [ "$RESTORE_ONLY" = 1 ]; then
    say "watchdog fired: restoring without unloading anything further"
    restore
    exit 0
fi

[ -n "$MESA_ICD" ] || { echo "no Mesa pvr ICD json - build mesa-25.0.7 first"; exit 2; }
[ -x "$BENCH/vktest" ] || { echo "no $BENCH/vktest - run its build.sh"; exit 2; }
for m in "$SPIKE/mod/drm_gpuvm.ko" "$SPIKE/img/powervr.ko"; do
    [ -f "$m" ] || { echo "missing $m"; exit 2; }
done

say "log: $LOG"
say "Mesa ICD: $MESA_ICD"
say "pre-state: pvrsrvkm refs=$(awk '$1=="pvrsrvkm"{print $3}' /proc/modules) X=$(pgrep -c -x X)"

say "--- baseline: vendor ICD + vendor module (before the swap) ---"
( cd "$BENCH" && timeout 300 ./vktest 10 1048576 2>&1 | tail -5 | sed 's/^/    /' | tee -a "$LOG" )

say "arming watchdog (auto-restore in 8 minutes)"
systemd-run --unit="$WATCHDOG_UNIT" --on-active=8min "$0" --restore-only >>"$LOG" 2>&1 \
    || say "WARNING: watchdog could not be armed"

say "--- stopping display-manager ---"
systemctl stop display-manager 2>&1 | sed 's/^/    /' | tee -a "$LOG"
for _ in $(seq 1 30); do
    refs=$(awk '$1=="pvrsrvkm"{print $3}' /proc/modules)
    { [ -z "$refs" ] || [ "$refs" = "0" ]; } && break
    sleep 2
done
refs=$(awk '$1=="pvrsrvkm"{print $3}' /proc/modules)
say "pvrsrvkm refs after stopping the desktop: ${refs:-unloaded}"
if [ -n "$refs" ] && [ "$refs" != "0" ]; then
    say "ABORT: something still holds the vendor module (refs=$refs) - not unloading"
    exit 3
fi

say "--- unloading vendor module ---"
rmmod pvrsrvkm 2>&1 | sed 's/^/    /' | tee -a "$LOG" || { say "ABORT: rmmod failed"; exit 3; }

say "--- loading mainline driver ---"
for m in drm_exec gpu-sched drm_shmem_helper; do
    modprobe "$m" || say "WARNING: modprobe $m failed"
done
insmod "$SPIKE/mod/drm_gpuvm.ko" 2>&1 | sed 's/^/    /' | tee -a "$LOG"
insmod "$SPIKE/img/powervr.ko" 2>&1 | sed 's/^/    /' | tee -a "$LOG"
sleep 3

say "--- kernel log ---"
dmesg | grep -iE 'powervr|pvr' | tail -12 | sed 's/^/    /' | tee -a "$LOG"

say "--- DRM devices as Mesa sees them ---"
"$BENCH/drmdevs" 2>&1 | sed 's/^/    /' | tee -a "$LOG"

say "--- Mesa pvr ICD + mainline powervr driver: compute ---"
( cd "$BENCH" && VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
    PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 PVR_TRACE=1 \
    timeout 300 ./vktest 10 1048576 2>&1 | sed 's/^/    /' | tee -a "$LOG" )

if [ -x "$BENCH/vkrender" ]; then
    say "--- Mesa pvr ICD + mainline powervr driver: offscreen render ---"
    # BATCH=n records n frames per command buffer: BATCH=1 is submit+fence per
    # frame (what a compositor does), the larger values show what the submit path
    # itself costs on this driver.
    for _b in 1 8 32; do
        ( cd "$BENCH" && BATCH=$_b VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
            PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
            timeout 300 ./vkrender 512 32 2>&1 | grep -E 'frame\(s\) in|RESULT|VERDICT' \
            | sed "s/^/    BATCH=$_b /" | tee -a "$LOG" )
    done
else
    say "no $BENCH/vkrender - skipping the graphics test"
fi

GL_PREFIX=/home/radxa/mesa/inst-gl/usr/local/lib/aarch64-linux-gnu
if [ -x "$BENCH/glheadless" ] && [ -d /home/radxa/mesa/gldri ]; then
    say "--- zink GL (Mesa 25.3) over Mesa pvr + mainline driver ---"
    ( cd "$BENCH" && LD_LIBRARY_PATH="$GL_PREFIX" LIBGL_DRIVERS_PATH=/home/radxa/mesa/gldri \
        MESA_LOADER_DRIVER_OVERRIDE=zink EGL_PLATFORM=device \
        DRM_RENDER_NODE=/dev/dri/renderD128 \
        EGL_LOG_LEVEL=debug LIBGL_DEBUG=verbose \
        VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
        PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
        timeout 300 ./glheadless 512 20 2>&1 | sed 's/^/    /' | tee -a "$LOG" )
else
    say "no glheadless / GL build - skipping the GL test"
fi

say "--- kernel log after the test ---"
dmesg | grep -iE 'powervr|pvr' | tail -6 | sed 's/^/    /' | tee -a "$LOG"

say "restoring now"
restore
say "evidence: $LOG"
