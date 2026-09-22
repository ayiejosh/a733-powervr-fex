#!/bin/bash
# dsu-ab.sh — one-shot snapshot for an A/B on the DSU (L3/coherency fabric) clock.
#
# Prints the live clock tree, then memory bandwidth, CPU, GPU and thermals, then the count
# of kernel-trouble lines since boot.  Run it once per boot after changing
# overlays/dsu-clk.dts and diff the two outputs — that is how 780 -> 1027 MHz was judged:
#
#   label   dsu       l3read   l3shared  dramread   fex.tcreate
#   780     780 MHz   10.2     12.4      8.2-9.3    181951 ns/op
#   1027    1027 MHz  10.9-12.2 13.4-15.6 10.3-11.8  123820 ns/op
#   1196    1196 MHz  within noise of 1027  -> the knee is ~1027, keep the safer setting
#
#   gcc -O3 -fopenmp -march=native -ffast-math membw.c -o membw -lm
#   sudo bench/dsu-ab.sh 1027
set -u
D="$(cd "$(dirname "$0")" && pwd)"
MEMBW=${MEMBW:-$D/membw}
GLBENCH=${GLBENCH:-$D/glbench}
LABEL="${1:-run}"
SUDO=${SUDO:-sudo}

clk() { $SUDO cat /sys/kernel/debug/clk/clk_summary 2>/dev/null | awk -v n="$1" '$1==n{print $5}'; }
zone() { for z in /sys/class/thermal/thermal_zone*/; do
           case "$(cat "$z/type" 2>/dev/null)" in
             cpul_thermal_zone|cpub_thermal_zone|gpu_thermal_zone|ddr_thermal_zone)
               printf '%s=%sC ' "$(cat "$z/type")" "$(( $(cat "$z/temp") / 1000 ))" ;;
           esac
         done; }

echo "=== DSU TEST [$LABEL] $(date -Is) ==="
echo "dsu=$(clk cpu_dsu)  pll-cpu-dsu=$(clk pll-cpu-dsu)  gpu=$(clk gpu0)"
echo "cpu_max: little=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq) big=$(cat /sys/devices/system/cpu/cpufreq/policy6/scaling_max_freq)"

echo "--- memory bandwidth, pinned to a BIG core (3 runs; L3 read ~= DRAM read means a starved DSU) ---"
for _ in 1 2 3; do taskset -c 6 "$MEMBW" 1 256 2>/dev/null | head -4; echo "  --"; done

echo "--- multi-thread (coherency / DRAM under load) ---"
taskset -c 0-7 "$MEMBW" 8 256 2>/dev/null | head -3

echo "--- CPU: sha256 ---"
echo "  8-thread: $(openssl speed -multi 8 -seconds 8 sha256 2>/dev/null | tail -1)"
echo "  1-thread (big): $(taskset -c 6 openssl speed -seconds 5 sha256 2>/dev/null | tail -1)"

echo "--- GPU: glbench ALU loop (Mpix/s) — must NOT move with the DSU clock ---"
for L in 4 16 64; do printf '  loop%-4s ' "$L"; LD_LIBRARY_PATH=/usr/local/lib timeout 90 "$GLBENCH" /dev/dri/renderD128 "$L" 300 2>/dev/null | grep -oE '[0-9.]+ ?Mpix'; done

echo "--- temps / fan ---"; zone; echo "fan=$(cat /sys/class/hwmon/hwmon0/pwm1 2>/dev/null)"
echo "--- kernel errors since boot ---"
$SUDO journalctl -k -b 0 --no-pager 2>/dev/null | grep -icE 'Oops|BUG:|Call trace|clk.*error|failed to set'
echo "=== DONE $(date -Is) ==="
