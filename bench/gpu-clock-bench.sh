#!/bin/bash
# gpu-clock-bench.sh — capture the GPU clock state and what it buys, in one file.
#
# Answers "is the overlay actually applied, and does the score follow?" — driver log
# clk_rate/voltage, the live clock tree, then glmark2 on both userspace GL paths:
#
#   label           vendor GLES   zink    (GPU clock)
#   600mhz-baseline      659       454    600 MHz  <- driver fallback, no overlay
#   1008mhz              827       578    1008 MHz
#   1104mhz-990mv        826       581    1104 MHz  <- and glmark2 stops tracking here
#
# Because glmark2 saturates the CPU/driver before the GPU on this board, the *throughput*
# number to quote is bench/glbench.c (4186/1215/315/80 -> 7392/2229/579/147 Mpix/s).  This
# script is the quick end-to-end sanity check, not the ceiling measurement.
#
#   bench/gpu-clock-bench.sh 1104mhz-990mv
#
# Needs X + `glrun` (the zink launcher).  DISPLAY/XAUTHORITY are overridable.
set -u
D="$(cd "$(dirname "$0")" && pwd)"
LABEL="${1:-run}"
OUT="$D/gpu-clock-$LABEL.txt"
export DISPLAY="${DISPLAY:-:0}"
SUDO=${SUDO:-sudo}

{
echo "================ GPU CLOCK BENCH [$LABEL] $(date -Is) ================"
echo "--- driver log: clk_rate / voltage ---"
$SUDO journalctl -k -b 0 --no-pager 2>/dev/null | grep -E 'clk_rate|sunxiSetVoltage|sunxi_set_device_clk_rate' | head -6
echo "--- live GPU clock tree ---"
$SUDO cat /sys/kernel/debug/clk/clk_summary 2>/dev/null \
  | grep -E 'pll-gpu|gpu0|clk_parent' | awk '{printf "%-24s rate=%-12s enable=%s\n", $1, $5, $4}' | head -8
echo "--- vendor (PowerVR GLES, no glrun) ---"
glmark2-es2 --off-screen 2>/dev/null | grep -E 'GL_VENDOR|GL_RENDERER|GL_VERSION|glmark2 Score'
echo "--- zink over PowerVR Vulkan ---"
glrun glmark2-es2 --off-screen 2>/dev/null | grep -E 'GL_RENDERER|glmark2 Score'
echo "--- temp / throttling ---"
for z in /sys/class/thermal/thermal_zone*/temp; do
  printf '%s %s\n' "$(basename "$(dirname "$z")")" "$(cat "$z" 2>/dev/null)"
done | head -6
echo "================ DONE $(date -Is) ================"
} 2>&1 | tee "$OUT"
