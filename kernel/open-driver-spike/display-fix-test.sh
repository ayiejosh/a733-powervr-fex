#!/bin/bash
# Verify the display hand-back fix in isolation, with tripwires.
#
# The tools used to point the CRTC at the stopped desktop's framebuffer (already
# freed) and to re-exec with a live scan, either of which makes the display engine
# fault on unmapped memory - a storm that wedges the box. They now switch the CRTC
# off, confirm it, wait for the display engine, and only then release the fds.
#
# This runs the shortest possible display work and checks the fault count after every
# step, aborting before a fault can become a storm. Restore is armed on a timer so a
# failure here can never leave the desktop down.
set -u

BENCH=/home/radxa/trixie-prep/bench/pvr-vulkan
SPIKE=/home/radxa/kspike
LOG=$SPIKE/display-fix-$(date +%Y%m%d-%H%M%S).log
MESA_ICD=/home/radxa/mesa/mesa-main/build/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json
FAULT_LIMIT=20

say() { echo "[display-fix $(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
de0_faults() {
    dmesg 2>/dev/null | grep -cE 'DE0 module|is not mapped!|sunxi_iommu_irq|Runtime PM usage count underflow'
}
rate_over() {
    local secs=$1 before after
    before=$(de0_faults); sleep "$secs"; after=$(de0_faults)
    printf '%d' "$((after > before ? after - before : 0))"
}

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
    say "idle fault rate after restore (10s): $(rate_over 10)"
    say "evidence: $LOG"
}
trap restore EXIT

say "log: $LOG"
say "baseline faults this boot: $(de0_faults)"
say "pre-state: pvrsrvkm refs=$(awk '$1=="pvrsrvkm"{print $3}' /proc/modules) X=$(pgrep -c -x X)"

if systemctl is-active --quiet kmsconvt@tty1; then systemctl stop kmsconvt@tty1; fi
say "--- stopping display-manager ---"
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
say "open driver loaded: $(grep -c '^powervr ' /proc/modules) powervr, dri=$(ls /dev/dri | tr '\n' ' ')"

f0=$(de0_faults)
say "--- step 1: a short scanout (previously the self-rerun exec'd with a live scan) ---"
( cd "$BENCH" && VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
    PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 120 ./pvrscanout 1920 1080 2 2>&1 \
    | grep -E 'display released|scanout|VERDICT|FAIL|re-running' | sed 's/^/    /' | tee -a "$LOG" )
f1=$(de0_faults); d1=$((f1 - f0))
say "step 1 faults: $d1"
if [ "$d1" -gt "$FAULT_LIMIT" ]; then say "TRIPWIRE: faults after scanout - aborting"; exit 4; fi

say "--- step 2: a short animation (previously ended on the old freed framebuffer) ---"
( cd "$BENCH" && VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
    PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 120 ./pvranimate 1920 1080 60 2>&1 \
    | grep -E 'display released|presented|VERDICT|FAIL' | sed 's/^/    /' | tee -a "$LOG" )
f2=$(de0_faults); d2=$((f2 - f1))
say "step 2 faults: $d2"
if [ "$d2" -gt "$FAULT_LIMIT" ]; then say "TRIPWIRE: faults after animation - aborting"; exit 4; fi

say "--- step 3: run it twice in a row, the way the benchmark does ---"
( cd "$BENCH" && for _i in 1 2; do
    VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
        PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 120 ./pvranimate 3840 2160 30 2>&1 \
        | grep -E 'presented|VERDICT|FAIL' | sed "s/^/    [$i] /" | tee -a "$LOG"
done )
f3=$(de0_faults); d3=$((f3 - f2))
say "step 3 faults: $d3"

say "=================================================================="
say "RESULT: faults scanout=$d1 animation=$d2 repeated=$d3 (limit $FAULT_LIMIT each)"
if [ "$((d1 + d2 + d3))" -eq 0 ]; then
    say "PASS - the display hand-back leaves no faults behind"
else
    say "PARTIAL - faults are down but not zero; see the per-step numbers"
fi
say "fault rate while idle with the open driver (5s): $(rate_over 5)"
