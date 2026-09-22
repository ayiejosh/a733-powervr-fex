#!/bin/bash
# gpu-stress.sh — is the new GPU clock actually stable?
#
# Runs the heaviest glmark2 scenes in a loop at whatever clock is currently active,
# checking after every iteration for GPU faults (PVR_K error/fault/timeout/watchdog),
# kernel trouble, rising temperature, and whether X is still alive.  This is the harness
# behind "1104 MHz @ 990 mV: 58-61 C, zero error lines" — a throughput number without it
# is not a result.
#
#   sudo bench/gpu-stress.sh 1104mhz-990mv
#
# Needs a running X display for glmark2 unless you adapt the scene line; the X auth path
# is overridable:
#   DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority bench/gpu-stress.sh local
set -u
D="$(cd "$(dirname "$0")" && pwd)"
LABEL="${1:-stress}"
LOG="$D/gpu-stress-$LABEL.log"
export DISPLAY="${DISPLAY:-:0}"
SUDO=${SUDO:-sudo}

{
echo "================ GPU STRESS [$LABEL] $(date -Is) ================"
echo "--- active clock / voltage ---"
$SUDO cat /sys/kernel/debug/clk/clk_summary 2>/dev/null | awk '$1=="gpu0"{print "gpu0 rate="$5}'
$SUDO cat /sys/kernel/debug/regulator/regulator_summary 2>/dev/null | grep -A1 'axp8191-dcdc4' | head -2

MARK=$(date +%s); FAIL=0
for i in $(seq 1 6); do
  echo; echo "--- iteration $i @ $(date +%H:%M:%S) ---"
  # heaviest scenes: terrain, refract, jellyfish, desktop-blur
  timeout 300 glmark2-es2 --off-screen \
      -b terrain:duration=8 -b refract:duration=8 -b jellyfish:duration=8 -b desktop:duration=8 \
      2>&1 | grep -E 'FPS|Error|error' | tail -6
  NEW=$($SUDO journalctl -k --since "@$MARK" --no-pager 2>/dev/null \
        | grep -icE 'PVR_K:.*(error|fault|fail|timeout|watchdog)|Oops|BUG:|Call trace')
  TEMP=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
  XUP=$(pgrep -x X >/dev/null && echo yes || echo NO)
  echo "   new kernel GPU-fault lines: $NEW   |  temp: $(( TEMP / 1000 ))C  |  X alive: $XUP"
  [ "$NEW" -gt 0 ] && FAIL=1
  [ "$XUP" = "NO" ] && FAIL=1
done
echo; echo "================ RESULT: $([ $FAIL -eq 0 ] && echo STABLE || echo 'PROBLEMS DETECTED') $(date -Is) ================"
echo "--- any GPU messages during the whole stress ---"
$SUDO journalctl -k --since "@$MARK" --no-pager 2>/dev/null | grep -iE 'PVR|rgx|gpu' | tail -10
} 2>&1 | tee "$LOG"
