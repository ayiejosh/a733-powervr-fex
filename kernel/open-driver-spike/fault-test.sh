#!/bin/bash
# Isolate the source of the DE0/IOMMU faults without touching the GPU modules.
#
# The display engine (DE0) faults when it scans something that is not mapped:
#   iommu_master de0_iommu ... 0x...  is not mapped!
#   Bug is in DE0 module, invalid address: ...
# A few of those are survivable; a *storm* (thousands) saturates CPU 0 in the IRQ
# handler, floods the log and wedges the box until the hardware watchdog resets it.
#
# This script only starts and stops the desktop session - no module swap, no
# dma-buf import, no direct CRTC use - so whatever faults appear here belong to X's
# own modeset transitions rather than to the benchmark tools. It also samples the
# fault *rate* afterwards, because a stopped storm is the thing that matters.
set -u

LOG=/home/radxa/kspike/fault-test-$(date +%Y%m%d-%H%M%S).log
say() { echo "[fault-test $(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

de0_faults() {
    dmesg 2>/dev/null | grep -cE 'DE0 module|is not mapped!|sunxi_iommu_irq|Runtime PM usage count underflow'
}

rate_over() { # seconds
    local secs=$1 before after
    before=$(de0_faults)
    sleep "$secs"
    after=$(de0_faults)
    # dmesg is a ring buffer: a negative or zero delta means nothing new arrived
    printf '%d' "$((after > before ? after - before : 0))"
}

restore_session() {
    systemctl start display-manager
    for _ in $(seq 1 30); do
        sleep 2
        pgrep -x kwin_x11 >/dev/null && break
    done
    if ! pgrep -x kwin_x11 >/dev/null; then
        systemctl restart display-manager
        for _ in $(seq 1 20); do sleep 2; pgrep -x kwin_x11 >/dev/null && break; done
    fi
    say "desktop: X=$(pgrep -c -x X) kwin=$(pgrep -c kwin_x11) plasmashell=$(pgrep -c plasmashell)"
}
trap restore_session EXIT

say "log: $LOG"
say "baseline faults this boot: $(de0_faults)"
say "idle fault rate over 5s: $(rate_over 5)"

say "--- stopping the desktop (this is what every swap does first) ---"
if systemctl is-active --quiet kmsconvt@tty1; then
    systemctl stop kmsconvt@tty1
fi
systemctl stop display-manager
for _ in $(seq 1 30); do
    if ! pgrep -x X >/dev/null && ! pgrep -x kwin_x11 >/dev/null; then break; fi
    sleep 1
done
for _p in X kwin_x11 plasmashell picom; do pkill -x "$_p" 2>/dev/null; done
sleep 3
say "faults caused by stopping the session: $(de0_faults) total now"
say "fault rate while the session is down (5s): $(rate_over 5)"

say "--- starting the desktop again (X does its own modeset here) ---"
restore_session
say "faults after the session came back: $(de0_faults) total"
say "fault rate after the session is up (10s): $(rate_over 10)"
say "VERDICT: idle=$(rate_over 5) faults/5s after settling"
say "evidence: $LOG"
