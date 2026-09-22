#!/bin/bash
# test.sh — the GPU-composited-desktop experiment, bounded and reversible.
#
# What it does: turns KWin's own compositing ON (kwinrc) and restarts KWin with the
# OpenGL ES backend forced (KWIN_COMPOSE=O2ES, KWIN_OPENGL_INTERFACE=egl) so the desktop's
# compositing runs on the PowerVR GPU through the vendor EGL/GLES instead of picom on the CPU.
# It then reports what KWin chose and what the GPU did.
#
# Safety: touches a sentinel first. If the kernel hangs, the HW watchdog reboots (~96 s) and
# gpu-test-guard.service runs revert.sh before the desktop comes up, so the board cannot
# boot-loop on a broken compositor. Run ./revert.sh to go back by hand.
set -u
H=/home/radxa; D=$H/gpu-desktop-test; KR=$H/.config/kwinrc
export DISPLAY=:0 XAUTHORITY=$H/.crd-xauth XDG_RUNTIME_DIR=/run/user/1000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
R(){ timeout 15 qdbus6 org.kde.KWin /Compositor "org.kde.kwin.Compositing.$1" 2>&1 | head -1; }
irq(){ grep -i pvrsrvkm /proc/interrupts | awk '{s=0;for(i=2;i<=NF-2;i++)s+=$i;print s}'; }

echo "=== BEFORE ==="
printf 'active=%s  type=%s  possible=%s  notPossible="%s"  glBroken=%s\n' "$(R active)" "$(R compositingType)" "$(R compositingPossible)" "$(R compositingNotPossibleReason)" "$(R openGLIsBroken)"
printf 'supportedOpenGLPlatformInterfaces: %s\n' "$(R supportedOpenGLPlatformInterfaces)"
ps -eo pid,pcpu,cmd | grep -E "kwin_x11|picom" | grep -v grep

touch "$D/IN-PROGRESS"
cp -f "$KR" "$D/kwinrc.bak-gles-test"
kwriteconfig6 --file kwinrc --group Compositing --key Enabled true && echo "kwinrc: [Compositing] Enabled=true"
pkill -x picom && echo "picom stopped (KWin will composite instead)"

echo; echo "=== starting kwin_x11 --replace with KWIN_COMPOSE=O2ES ==="
a=$(irq)
KWIN_COMPOSE=O2ES KWIN_OPENGL_INTERFACE=egl setsid nohup kwin_x11 --replace >"$D/kwin-gles.log" 2>&1 &
sleep 15

echo; echo "=== AFTER ==="
printf 'active=%s  type=%s  possible=%s  notPossible="%s"  glBroken=%s\n' "$(R active)" "$(R compositingType)" "$(R compositingPossible)" "$(R compositingNotPossibleReason)" "$(R openGLIsBroken)"
ps -eo pid,pcpu,etime,cmd | grep -E "kwin_x11|picom" | grep -v grep
echo "--- kwin log ---"; tail -15 "$D/kwin-gles.log"
echo "--- GPU interrupts over 5 s (nonzero = the GPU is compositing) ---"
sleep 5; b=$(irq); echo "pvr IRQs: +$((b-a))"
echo "--- kernel log (expect 0) ---"; sudo journalctl -k -b 0 --no-pager 2>/dev/null | grep -cE "Oops|BUG:|Call trace"
echo "--- alive: $(uptime | sed 's/.*up //;s/,.*load/ load/') ---"
