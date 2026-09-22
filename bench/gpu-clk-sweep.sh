#!/bin/bash
# gpu-clk-sweep.sh — measure the GPU clock generator's real ceiling in ONE boot.
#
# Requests a list of rates through bench/clkctl/ and reports the rate that actually came
# back plus the glbench throughput at each point.  This is how the 1104 MHz ceiling was
# established: everything above it silently reports 1104.
#
#   sudo insmod bench/clkctl/clkctl.ko     # build it first: cd bench/clkctl && make
#   sudo bench/gpu-clk-sweep.sh            # default list below
#   sudo bench/gpu-clk-sweep.sh 600000000 1008000000 1104000000
#
# SAFETY: writes are runtime-only and the *boot* rate is restored on exit (including on
# Ctrl-C), so a crash mid-sweep reboots back into the saved configuration.  Running this
# never changes what the board boots with — that lives in overlays/gpu-clk.dts.
set -u
CLKCTL=/sys/kernel/debug/clkctl/gpu_clk
GLBENCH=${GLBENCH:-/usr/local/bin/glbench}
RATES=("$@")
[ ${#RATES[@]} -eq 0 ] && RATES=(1008000000 1104000000 1152000000 1200000000 1248000000 1296000000 1344000000 1392000000)

[ -w "$CLKCTL" ] || { echo "!! $CLKCTL not writable — is clkctl.ko loaded? (see bench/clkctl/)" >&2; exit 1; }

BOOT_RATE=$(cat "$CLKCTL")
restore() { echo "$BOOT_RATE" > "$CLKCTL" 2>/dev/null; echo "restored $BOOT_RATE Hz (boot value) -> now $(cat "$CLKCTL")"; }
trap 'restore; exit 130' INT TERM

echo "=== GPU CLOCK SWEEP $(date -Is) — boot rate $BOOT_RATE Hz ==="
printf '%-12s %-12s %-11s %-11s %-6s %s\n' REQUESTED ACTUAL loop4 loop16 temp errors

for r in "${RATES[@]}"; do
    echo "$r" > "$CLKCTL" 2>/dev/null || { echo "$r: write rejected"; continue; }
    actual=$(cat "$CLKCTL")
    mark=$(date +%s)
    l4=$(LD_LIBRARY_PATH=/usr/local/lib timeout 90 "$GLBENCH" /dev/dri/renderD128 4 300 2>/dev/null | grep -oE '[0-9.]+ ?Mpix')
    l16=$(LD_LIBRARY_PATH=/usr/local/lib timeout 90 "$GLBENCH" /dev/dri/renderD128 16 300 2>/dev/null | grep -oE '[0-9.]+ ?Mpix')
    temp=$(( $(cat /sys/class/thermal/thermal_zone0/temp) / 1000 ))C
    errs=$(journalctl -k --since "@$mark" --no-pager 2>/dev/null | grep -icE 'PVR_K:.*(error|fault|fail|timeout|watchdog)|Oops|BUG:|Call trace')
    printf '%-12s %-12s %-11s %-11s %-6s %s\n' "$r" "$actual" "${l4:-FAIL}" "${l16:-FAIL}" "$temp" "$errs"
    [ "$errs" -gt 0 ] && { echo "   ^^ kernel trouble at $r — stopping sweep"; break; }
done

restore
echo "=== DONE $(date -Is) ==="
