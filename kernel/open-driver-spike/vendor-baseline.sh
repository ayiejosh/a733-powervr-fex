#!/bin/bash
# Vendor-stack baseline with the same harness the open driver is measured with.
#
# No module swap here: the vendor module stays loaded the whole time, so the only
# thing taken away is the desktop. pvranimate drives the CRTC directly (SetCrtc +
# page flips), so X must not be running while it does.
set -u

BENCH=/home/radxa/trixie-prep/bench/pvr-vulkan
LOG=/home/radxa/kspike/vendor-baseline-$(date +%Y%m%d-%H%M%S).log
VENDOR_ICD=/usr/share/vulkan/icd.d/img_icd.json

# The presentation tools hand the CRTC back before exiting (drmModeSetCrtc with no
# framebuffer), because leaving it scanning a buffer this process owns means the
# display engine faults on every scan once that buffer is freed:
#   iommu_master de0_iommu ... 0x00000000fc000000 is not mapped!
#   Bug is in DE0 module, invalid address: ...
# That flooded the log with 16k messages and needed a forced reboot. Check for it
# after the display phases so a regression is loud instead of silent.
de0_faults() {
    dmesg 2>/dev/null | grep -cE 'DE0 module|is not mapped!|sunxi_iommu_irq|Runtime PM usage count underflow'
}

say() { echo "[vendor-baseline $(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

cleanup() {
    say "--- RESTORE ---"
    if ! systemctl is-active --quiet kmsconvt@tty1; then
        systemctl start kmsconvt@tty1 2>/dev/null
    fi
    systemctl start display-manager
    for _ in $(seq 1 30); do
        sleep 2
        pgrep -x kwin_x11 >/dev/null && break
    done
    if ! pgrep -x kwin_x11 >/dev/null; then
        say "no window manager - restarting display-manager once"
        systemctl restart display-manager
        for _ in $(seq 1 20); do sleep 2; pgrep -x kwin_x11 >/dev/null && break; done
    fi
    say "desktop: X=$(pgrep -c -x X) kwin=$(pgrep -c kwin_x11) plasmashell=$(pgrep -c plasmashell)"
    say "evidence: $LOG"
}
trap cleanup EXIT

say "log: $LOG"
say "pre-state: pvrsrvkm refs=$(awk '$1=="pvrsrvkm"{print $3}' /proc/modules) X=$(pgrep -c -x X)"

if systemctl is-active --quiet kmsconvt@tty1; then
    say "stopping kmsconvt@tty1 (it holds most of the GPU references)"
    systemctl stop kmsconvt@tty1
fi
say "--- stopping display-manager ---"
systemctl stop display-manager
for _ in $(seq 1 30); do
    if ! pgrep -x X >/dev/null && ! pgrep -x kwin_x11 >/dev/null; then break; fi
    sleep 1
done
for _p in X kwin_x11 plasmashell picom; do pkill -x "$_p" 2>/dev/null; done
sleep 2
say "session cleared: X=$(pgrep -c -x X 2>/dev/null || echo 0)"

if [ -x "$BENCH/pvranimate" ]; then
    say "--- vendor presentation: page-flipped animation ---"
    (
        cd "$BENCH" || exit 1
        for _res in "1920 1080 240" "3840 2160 120"; do
            VK_ICD_FILENAMES="$VENDOR_ICD" VK_DRIVER_FILES="$VENDOR_ICD" PVR_TIMING=1 \
                PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
                timeout 300 ./pvranimate $_res 2>&1 \
                | grep -E 'display:|presented|timing|VERDICT|fail|busy|error' \
                | sed "s/^/    [$_res] /" | tee -a "$LOG"
        done
    )
fi

if [ -x "$BENCH/glheadless" ] && [ -f "$VENDOR_ICD" ]; then
    say "--- vendor GL: stage split and readback scaling ---"
    (
        cd "$BENCH" || exit 1
        for _sz in 128 256 512; do
            VK_ICD_FILENAMES="$VENDOR_ICD" VK_DRIVER_FILES="$VENDOR_ICD" PVR_TIMING=1 \
                EGL_PLATFORM=gbm DRM_RENDER_NODE=/dev/dri/renderD128 \
                PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
                timeout 200 ./glheadless $_sz 30 2>&1 \
                | grep -E 'GL_RENDERER|GL_VERSION|timing ms/frame|frame\(s\)' \
                | sed "s/^/    [gl $_sz] /" | tee -a "$LOG"
        done
    )
fi

if [ -x "$BENCH/vkrender" ]; then
    say "--- vendor Vulkan: draw / copy / area ---"
    (
        cd "$BENCH" || exit 1
        for _m in both render copy; do
            for _c in "512 300" "1024 150"; do
                MODE=$_m VK_ICD_FILENAMES="$VENDOR_ICD" VK_DRIVER_FILES="$VENDOR_ICD" PVR_TIMING=1 \
                    PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 200 ./vkrender $_c 2>&1 \
                    | grep -E 'timing|frame\(s\)' | sed "s/^/    [$_m $_c] /" | tee -a "$LOG"
            done
        done
        for _a in "" half quarter; do
            AREA=$_a MODE=render VK_ICD_FILENAMES="$VENDOR_ICD" VK_DRIVER_FILES="$VENDOR_ICD" \
                PVR_TIMING=1 PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 200 ./vkrender 1024 150 2>&1 \
                | grep -E 'timing' | sed "s/^/    [area=${_a:-full}] /" | tee -a "$LOG"
        done
    )
fi

say "DE0/IOMMU faults seen: $(de0_faults)"
say "--- kernel log ---"
dmesg | grep -iE 'pvrsrvkm|pvr' | tail -4 | sed 's/^/    /' | tee -a "$LOG"
