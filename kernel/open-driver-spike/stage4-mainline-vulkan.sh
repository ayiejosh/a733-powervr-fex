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
WATCHDOG_UNIT=stage4-restore-watchdog-$$
RESTORED=0
RESTORE_ONLY=0
[ "${1:-}" = "--restore-only" ] && RESTORE_ONLY=1

# The presentation tools hand the CRTC back before exiting (drmModeSetCrtc with no
# framebuffer), because leaving it scanning a buffer this process owns means the
# display engine faults on every scan once that buffer is freed:
#   iommu_master de0_iommu ... 0x00000000fc000000 is not mapped!
#   Bug is in DE0 module, invalid address: ...
# That flooded the log with 16k messages and needed a forced reboot. Check for it
# after the display phases so a regression is loud instead of silent.
# Fault deltas per phase: the DE0/IOMMU warnings are worth attributing rather than
# only counting, because one of them (the display engine scanning a freed buffer)
# is a fault storm that kills the desktop, while others are transient power-up
# noise from the BSP kernel's DE/IOMMU runtime PM.
phase_faults() {
    local label=$1 before=$2 after
    after=$(de0_faults)
    say "faults during $label: $((after - before))"
    echo "$after"
}

de0_faults() {
    dmesg 2>/dev/null | grep -cE 'DE0 module|is not mapped!|sunxi_iommu_irq|Runtime PM usage count underflow'
}

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

        # The open driver's remove path trips a warning in pvr_context_device_fini
        # (pvr_remove), and after that the vendor module can load *without* creating
        # its DRM device - /dev/dri/card1 stays missing and X then cannot start at
        # all. Detect it and reload the vendor module cleanly rather than leaving a
        # dead desktop behind.
        if [ ! -e /dev/dri/card1 ]; then
            say "card1 is missing after the swap - reloading the vendor module cleanly"
            rmmod pvrsrvkm 2>/dev/null
            sleep 2
            modprobe pvrsrvkm
            for _ in $(seq 1 15); do
                sleep 2
                [ -e /dev/dri/card1 ] && break
            done
            say "after reload: /dev/dri has $(ls /dev/dri | tr '\n' ' ')"
        fi
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
            # The autologin session occasionally does not come up on the first try
            # (seen once in ~20 swaps), leaving X with no window manager. One
            # restart of the display manager fixes it.
            if ! pgrep -x kwin_x11 >/dev/null; then
                say "no window manager - restarting display-manager once"
                systemctl restart display-manager
                for _ in $(seq 1 20); do
                    sleep 2
                    pgrep -x kwin_x11 >/dev/null && break
                done
                say "after restart: kwin=$(pgrep -c kwin_x11) plasmashell=$(pgrep -c plasmashell)"
            fi
        else
            say "X did NOT come back - retrying after a clean driver reload"
            rmmod pvrsrvkm 2>/dev/null; sleep 2; modprobe pvrsrvkm; sleep 5
            systemctl restart display-manager
            for _ in $(seq 1 20); do sleep 2; pgrep -x X >/dev/null && break; done
            if pgrep -x X >/dev/null; then
                say "recovered: X=$(pgrep -c -x X) kwin=$(pgrep -c kwin_x11)"
            else
                say "X did NOT come back - reboot needed"
            fi
        fi
    else
        say "CRITICAL: pvrsrvkm did not load - a reboot is needed"
    fi
    if ! systemctl is-active --quiet kmsconvt@tty1; then
        systemctl start kmsconvt@tty1 2>/dev/null
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
# Sample the GPU core clock while a render is running: the vendor driver runs
# gpu0/pll-gpu at 1104000000 Hz, and if the open driver leaves it lower the GPU is
# proportionally slower, which is what the throughput gap looked like.
gpu_clocks() {
    grep -E '^ *(pll-gpu|gpu0[^ ]*) ' /sys/kernel/debug/clk/clk_summary 2>/dev/null \
        | awk '{printf "%s=%s ", $1, $5}'
    echo
}
sample_clocks_during() {
    local label=$1 icd=$2 size=$3 iters=$4
    ( for _ in $(seq 1 6); do gpu_clocks | sed "s/^/    [$label] /" | tee -a "$LOG"; sleep 1; done ) &
    local sampler=$!
    VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" PVR_TIMING=1 \
        PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 300 "$BENCH/vkrender" "$size" "$iters" 2>&1 \
        | grep -E 'timing|frame\(s\)' | sed "s/^/    [$label] /" | tee -a "$LOG"
    kill $sampler 2>/dev/null
    wait $sampler 2>/dev/null
}

# Per-frame cost suite: splits the workload (draw vs image->buffer copy) so a
# throughput gap can be attributed instead of guessed at.
vkrender_suite() {
    local label=$1 icd=$2
    [ -f "$icd" ] || return 0
    for _m in both render copy; do
        for _c in "512 300" "1024 150"; do
            MODE=$_m VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" PVR_TIMING=1 \
                PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 200 "$BENCH/vkrender" $_c 2>&1 \
                | grep -E 'timing|frame\(s\)' | sed "s/^/    [$label $_m $_c] /" | tee -a "$LOG"
        done
    done
}

# Render-area sweep: separates fill-proportional cost from full-surface cost.
area_sweep() {
    local label=$1 icd=$2 size=$3 iters=$4
    [ -f "$icd" ] || return 0
    for _a in "" half quarter; do
        AREA=$_a MODE=render VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" PVR_TIMING=1 \
            PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 200 "$BENCH/vkrender" "$size" "$iters" 2>&1 \
            | grep -E 'timing' | sed "s/^/    [$label area=${_a:-full} $size] /" | tee -a "$LOG"
    done
}

# Attachment load-op sweep: if a driver's per-draw cost is a full-surface clear,
# VK_ATTACHMENT_LOAD_OP_LOAD removes it while the covered pixels are unchanged.
loadop_sweep() {
    local label=$1 icd=$2 size=$3 iters=$4
    [ -f "$icd" ] || return 0
    for _l in clear load dontcare; do
        LOADOP=$_l VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" PVR_TIMING=1 \
            PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 200 "$BENCH/vkrender" "$size" "$iters" 2>&1 \
            | grep -E 'timing' | sed "s/^/    [$label loadop=$_l $size] /" | tee -a "$LOG"
    done
    # and the same with the copy removed, to see the draw in isolation
    for _l in clear load; do
        LOADOP=$_l MODE=render VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" PVR_TIMING=1 \
            PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 200 "$BENCH/vkrender" "$size" "$iters" 2>&1 \
            | grep -E 'timing' | sed "s/^/    [$label loadop=$_l render-only $size] /" | tee -a "$LOG"
    done
}

# Attachment store-op sweep: tests whether the per-frame cost is the driver writing
# the whole surface back, independent of how much was drawn.
storeop_sweep() {
    local label=$1 icd=$2 size=$3 iters=$4
    [ -f "$icd" ] || return 0
    for _s in store dontcare; do
        STOREOP=$_s MODE=render VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" PVR_TIMING=1 \
            PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 200 "$BENCH/vkrender" "$size" "$iters" 2>&1 \
            | grep -E 'timing' | sed "s/^/    [$label storeop=$_s render-only $size] /" | tee -a "$LOG"
    done
}

# What does the driver ask the kernel for, per frame? Counts by ioctl request.
ioctl_profile() {
    local label=$1 icd=$2 size=$3 iters=$4 out=$5
    [ -f "$icd" ] || return 0
    VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
        timeout 300 strace -f -e trace=ioctl -o "$out" "$BENCH/vkrender" "$size" "$iters" >/dev/null 2>&1
    echo "    [$label ioctls for $iters frame(s) at $size]" | tee -a "$LOG"
    # raw request numbers: strace's name tables do not know the PVR ioctls and
    # mislabels them as other drivers' commands, but the numbers are exact
    sed -E 's/.*ioctl\([0-9]+, (0x[0-9a-f]+|[A-Za-z_0-9]+).*/\1/' "$out" 2>/dev/null \
        | sort | uniq -c | sort -rn | head -8 \
        | awk '{printf "      %6d  %s\n", $1, $2}' | tee -a "$LOG"
}

say "Mesa ICD: $MESA_ICD"

# The GL phase needs a pvr ICD whose driver advertises VK_KHR_dynamic_rendering:
# zink requires it, 25.3.0's pvr does not have it (which is why GL failed for so
# long - zink rejects the device with a message compiled out of release builds),
# and Mesa main's pvr does. Fall back to $MESA_ICD if main is not built.
GL_ICD=${GL_ICD:-}
if [ -z "$GL_ICD" ]; then
    for _c in /home/radxa/mesa/mesa-main/build/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json; do
        [ -f "$_c" ] && { GL_ICD=$_c; break; }
    done
fi
[ -z "$GL_ICD" ] && GL_ICD=$MESA_ICD
say "GL ICD:   $GL_ICD"
say "pre-state: pvrsrvkm refs=$(awk '$1=="pvrsrvkm"{print $3}' /proc/modules) X=$(pgrep -c -x X)"

say "--- baseline: vendor ICD + vendor module (before the swap) ---"
if [ -x "$BENCH/vkrender" ]; then
    say "--- vendor baseline: per-frame cost suite ---"
    area_sweep "vendor" /usr/share/vulkan/icd.d/img_icd.json 1024 150
    loadop_sweep "vendor" /usr/share/vulkan/icd.d/img_icd.json 1024 150
    storeop_sweep "vendor" /usr/share/vulkan/icd.d/img_icd.json 1024 150
    ioctl_profile "vendor" /usr/share/vulkan/icd.d/img_icd.json 1024 10 /tmp/ioctl-vendor.txt
    vkrender_suite "vendor" /usr/share/vulkan/icd.d/img_icd.json
    sample_clocks_during "vendor-clock" /usr/share/vulkan/icd.d/img_icd.json 1024 4000
fi
( cd "$BENCH" && timeout 300 ./vktest 10 1048576 2>&1 | tail -5 | sed 's/^/    /' | tee -a "$LOG" )

say "arming watchdog (auto-restore in 8 minutes)"
systemd-run --unit="$WATCHDOG_UNIT" --on-active=8min "$0" --restore-only >>"$LOG" 2>&1 \
    || say "WARNING: watchdog could not be armed"

# A degraded previous session (X up, no window manager) leaves clients holding the
# vendor module, and then stopping display-manager does not free it. Repair first.
if ! pgrep -x kwin_x11 >/dev/null; then
    say "session looks unhealthy (no kwin) - restarting display-manager before the swap"
    systemctl restart display-manager
    for _ in $(seq 1 30); do
        sleep 2
        pgrep -x kwin_x11 >/dev/null && break
    done
fi

# kmscon (the tty1 console) renders through the vendor GL stack: it was holding
# 154 of 172 pvrsrvkm references on its own, which is enough to block the unload
# even with the desktop stopped. Stop it for the duration.
if systemctl is-active --quiet kmsconvt@tty1; then
    say "stopping kmsconvt@tty1 (it holds most of the GPU references)"
    systemctl stop kmsconvt@tty1
fi

say "--- stopping display-manager ---"
systemctl stop display-manager

# SDDM can respawn X while the swap is in progress, and a KDE session that starts
# then runs on the *open* driver by accident: kwin came up on zink, failed, and left
# the desktop dead (seen twice, once needing a card1 reload). Make sure nothing of the
# old session survives before the GPU is taken away. Note: only pkill -x here - a
# pkill -f pattern can match this script's own command line and kill the run.
for _ in $(seq 1 30); do
    if ! pgrep -x X >/dev/null && ! pgrep -x kwin_x11 >/dev/null; then
        break
    fi
    sleep 1
done
for _p in X kwin_x11 plasmashell picom; do
    pkill -x "$_p" 2>/dev/null
done
sleep 2
say "session cleared: X=$(pgrep -c -x X 2>/dev/null || echo 0) kwin=$(pgrep -c -x kwin_x11 2>/dev/null || echo 0)" 2>&1 | sed 's/^/    /' | tee -a "$LOG"
for _ in $(seq 1 30); do
    refs=$(awk '$1=="pvrsrvkm"{print $3}' /proc/modules)
    { [ -z "$refs" ] || [ "$refs" = "0" ]; } && break
    sleep 2
done
refs=$(awk '$1=="pvrsrvkm"{print $3}' /proc/modules)
say "pvrsrvkm refs after stopping the desktop: ${refs:-unloaded}"
if [ -n "$refs" ] && [ "$refs" != "0" ]; then
    say "refs still $refs - listing GPU clients and clearing session processes"
    head -20 /sys/kernel/debug/dri/128/clients 2>/dev/null | sed 's/^/    /' | tee -a "$LOG"
    for p in kwin_x11 plasmashell picom kded6 ksmserver kaccess; do pkill -x "$p" 2>/dev/null; done
    for _ in $(seq 1 15); do
        sleep 2
        refs=$(awk '$1=="pvrsrvkm"{print $3}' /proc/modules)
        { [ -z "$refs" ] || [ "$refs" = "0" ]; } && break
    done
fi
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

if [ -x "$BENCH/pvrscanout" ]; then
    _f0=$(de0_faults)
say "--- render -> dma-buf -> sunxi-drm scanout (pattern on screen for 6 s) ---"
    ( cd "$BENCH" && VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
        PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
        timeout 300 ./pvrscanout 1280 720 6 2>&1 | sed 's/^/    /' | tee -a "$LOG" )
else
    say "no $BENCH/pvrscanout - skipping the scanout test"
fi

_f1=$(phase_faults "scanout" "$_f0")
if [ -x "$BENCH/pvranimate" ]; then
    say "--- continuous presentation: page-flipped animation on screen ---"
    (
        cd "$BENCH" || exit 1
        PWR=/sys/bus/platform/devices/1800000.gpu/power
        read -r _st0 < "$PWR/runtime_status" 2>/dev/null
        read -r _act0 < "$PWR/runtime_active_time" 2>/dev/null
        read -r _sus0 < "$PWR/runtime_suspended_time" 2>/dev/null
        # A/B: the driver uses a 50 ms autosuspend delay, so if the idle timer is
        # never refreshed per job the GPU can power down in the middle of a
        # rendering session and pay a resume per frame. Pin it awake to measure.
        _ctrl=$(cat "$PWR/control" 2>/dev/null)
        for _res in "1920 1080 240" "3840 2160 120"; do
            VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
                PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 PVR_TIMING=1 \
                timeout 300 ./pvranimate $_res 2>&1 \
                | grep -E 'display:|presented|phase|pushed twice|first 6|animation|push constant|VERDICT|timing|fail|FAIL|busy|error|pace=' \
                | sed "s/^/    [$_res] /" | tee -a "$LOG"
            read -r _act1 < "$PWR/runtime_active_time" 2>/dev/null
            read -r _sus1 < "$PWR/runtime_suspended_time" 2>/dev/null
            echo "    [$_res] pm: status $_st0->$(cat $PWR/runtime_status 2>/dev/null) active +$((_act1-_act0))ms suspended +$((_sus1-_sus0))ms" \
                | tee -a "$LOG"
            _act0=$_act1; _sus0=$_sus1
        done
        # Pacing comparison at 1080p: gate on the previous flip, or issue as soon as
        # the frame is rendered and let the EBUSY retry pace it.
        say "--- 1080p pacing comparison ---"
        for _pace in gate retry; do
            PACE=$_pace VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
                PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 PVR_TIMING=1 \
                timeout 300 ./pvranimate 1920 1080 240 2>&1 \
                | grep -E 'presented|timing' | sed "s/^/    [pace=$_pace] /" | tee -a "$LOG"
        done

        if [ "$_ctrl" = "auto" ]; then
            say "--- same 1080p test with the GPU pinned awake (power/control=on) ---"
            echo on > "$PWR/control"
            read -r _a0 < "$PWR/runtime_active_time"; read -r _s0 < "$PWR/runtime_suspended_time"
            VK_ICD_FILENAMES="$MESA_ICD" VK_DRIVER_FILES="$MESA_ICD" \
                PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 PVR_TIMING=1 \
                timeout 300 ./pvranimate 1920 1080 240 2>&1 \
                | grep -E 'presented|timing|VERDICT' | sed 's/^/    [pinned] /' | tee -a "$LOG"
            read -r _a1 < "$PWR/runtime_active_time"; read -r _s1 < "$PWR/runtime_suspended_time"
            echo "    [pinned] pm: active +$((_a1-_a0))ms suspended +$((_s1-_s0))ms" | tee -a "$LOG"
            echo "$_ctrl" > "$PWR/control"
        fi
    )
else
    say "no $BENCH/pvranimate - skipping the presentation test"
fi

# ---- performance profile: where does a frame's time go? ----------------------
# Vendor reference on this board, never over 4 s of stderr in one line item:
#   512x512 : record 0.037 ms, submit 0.067 ms, gpu 0.615 ms  (365 Mpix/s)
#   4096x4096: record 0.202 ms, submit 0.168 ms, gpu 23.509 ms (703 Mpix/s)
if [ -x "$BENCH/vkrender" ]; then
    say "--- performance profile: per-frame cost on the open stack ---"
    area_sweep "open-main" "$GL_ICD" 1024 150
    loadop_sweep "open-main" "$GL_ICD" 1024 150
    storeop_sweep "open-main" "$GL_ICD" 1024 150
    ioctl_profile "open-main" "$GL_ICD" 1024 10 /tmp/ioctl-open.txt
    vkrender_suite "open-25.3" "$MESA_ICD"
    vkrender_suite "open-main" "$GL_ICD"
    sample_clocks_during "open-clock" "$MESA_ICD" 1024 4000
    (
        cd "$BENCH" || exit 1
        for _icd in "25.3:$MESA_ICD" "main:$GL_ICD"; do
            _name=${_icd%%:*}; _path=${_icd#*:}
            [ -f "$_path" ] || continue
            for _case in "512 200" "4096 20"; do
                VK_ICD_FILENAMES="$_path" VK_DRIVER_FILES="$_path" PVR_TIMING=1 \
                    PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 300 ./vkrender $_case 2>&1 \
                    | grep -E 'timing|frame\(s\)' | sed "s/^/    [$_name $_case] /" | tee -a "$LOG"
            done
            VK_ICD_FILENAMES="$_path" VK_DRIVER_FILES="$_path" PVR_TIMING=1 BATCH=8 \
                PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 300 ./vkrender 512 200 2>&1 \
                | grep -E 'timing|frame\(s\)' | sed "s/^/    [$_name 512 BATCH=8] /" | tee -a "$LOG"
        done

        # Which kernel calls does a frame make? Counts, not timings: strace adds its
        # own overhead but the histogram shows what the driver asks the kernel for.
        say "--- ioctl histogram for 20 frames at 512x512 (main ICD) ---"
        VK_ICD_FILENAMES="$GL_ICD" VK_DRIVER_FILES="$GL_ICD" PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
            timeout 300 strace -f -c -e trace=ioctl,mmap,munmap,openat ./vkrender 512 20 2>&1 \
            | tail -20 | sed 's/^/    /' | tee -a "$LOG"
    )
fi

GL_PREFIX=/home/radxa/mesa/inst-gl/usr/local/lib/aarch64-linux-gnu
if [ -x "$BENCH/glheadless" ] && [ -d /home/radxa/mesa/gldri ]; then
    say "--- zink GL (Mesa 25.3) over Mesa pvr + mainline driver ---"
    # NB: MESA_LOADER_DRIVER_OVERRIDE is only honoured for non-root users
    # (loader.c: __normal_user()), and running as root made the loader fall back to
    # the kernel driver name "powervr", for which there is no gallium driver - the
    # screen then failed to be created with no message. So run this as the desktop
    # user, who is in the render group.
    ( cd "$BENCH" && runuser -u radxa -- env HOME=/home/radxa \
        LD_LIBRARY_PATH="$GL_PREFIX" LIBGL_DRIVERS_PATH=/home/radxa/mesa/gldri \
        GBM_BACKENDS_PATH="$GL_PREFIX/gbm" \
        MESA_LOADER_DRIVER_OVERRIDE=zink EGL_PLATFORM=gbm \
        DRM_RENDER_NODE=/dev/dri/renderD128 \
        EGL_LOG_LEVEL=debug LIBGL_DEBUG=verbose ZINK_TRACE=1 \
        MESA_GLES_VERSION_OVERRIDE=3.2 \
        VK_ICD_FILENAMES="$GL_ICD" VK_DRIVER_FILES="$GL_ICD" \
        PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 PVR_TIMING=1 \
        timeout 300 ./glheadless 512 20 2>&1 | sed 's/^/    /' | tee -a "$LOG"

        # Readback scaling: is glReadPixels slow because of bytes moved, or because
        # of a fixed per-readback cost (cache flush / sync / staging allocation)?
        say "  --- GL readback scaling (128/256/512) ---"
        for _sz in 128 256 512; do
            ( cd "$BENCH" && runuser -u radxa -- env HOME=/home/radxa \
                LD_LIBRARY_PATH="$GL_PREFIX" LIBGL_DRIVERS_PATH=/home/radxa/mesa/gldri \
                GBM_BACKENDS_PATH="$GL_PREFIX/gbm" \
                MESA_LOADER_DRIVER_OVERRIDE=zink EGL_PLATFORM=gbm \
                DRM_RENDER_NODE=/dev/dri/renderD128 PVR_TIMING=1 \
                MESA_GLES_VERSION_OVERRIDE=3.2 \
                VK_ICD_FILENAMES="$GL_ICD" VK_DRIVER_FILES="$GL_ICD" \
                PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
                timeout 300 ./glheadless $_sz 30 2>&1 \
                | grep -E 'timing ms/frame|frame\(s\)' | sed "s/^/    [size=$_sz] /" | tee -a "$LOG" )
        done

        # Variants: zink's descriptor mode and GL threading both affect per-draw cost.
        for _v in "ZINK_DESCRIPTOR_MODE=cached" "MESA_GLTHREAD=true" "ZINK_DESCRIPTOR_MODE=cached MESA_GLTHREAD=true"; do
            say "  --- GL variant: $_v ---"
            ( cd "$BENCH" && runuser -u radxa -- env HOME=/home/radxa \
                LD_LIBRARY_PATH="$GL_PREFIX" LIBGL_DRIVERS_PATH=/home/radxa/mesa/gldri \
                GBM_BACKENDS_PATH="$GL_PREFIX/gbm" \
                MESA_LOADER_DRIVER_OVERRIDE=zink EGL_PLATFORM=gbm \
                DRM_RENDER_NODE=/dev/dri/renderD128 MESA_GLES_VERSION_OVERRIDE=3.2 \
                VK_ICD_FILENAMES="$GL_ICD" VK_DRIVER_FILES="$GL_ICD" \
                PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 $_v \
                timeout 300 ./glheadless 512 20 2>&1 \
                | grep -E 'GL_RENDERER|GL_VERSION|frame\(s\)|timing|RESULT|VERDICT|FAIL' \
                | sed "s/^/    [$_v] /" | tee -a "$LOG" )
        done )
else
    say "no glheadless / GL build - skipping the GL test"
fi

_f2=$(phase_faults "animation" "$_f1")
_faults_after=$_f2
say "DE0/IOMMU faults so far this boot: $_faults_after"
if [ "$((_faults_after - _faults_before))" -gt 100 ]; then
    say "WARNING: the display phases caused an IOMMU fault storm - the display engine may be"
    say "         scanning a buffer that no longer exists; a desktop restart may be needed"
fi
say "--- kernel log after the test ---"
dmesg | grep -iE 'powervr|pvr' | tail -6 | sed 's/^/    /' | tee -a "$LOG"

say "restoring now"
restore
say "evidence: $LOG"
