#!/bin/bash
# cpu-oc-sweep.sh — prove (again) whether this BSP can be pushed past its spec clock.
#
# Runtime only: it writes scaling_max_freq above the vendor maximum and re-measures.  The
# persistent config is never touched, and the spec maximum is restored on exit — a crash
# during the sweep therefore reboots back to the shipped, safe configuration.
#
# Result on this BSP (2026-09-22, with overlays/experiments/cpu-oc-rejected.dts installed):
# the extra OPPs are NEVER OFFERED.  The BSP builds its frequency list from the chip's
# factory speed grade (efuse vf bin), not from the DT, so the sweep finds nothing to test
# and the reported maximum can even move DOWN (2002 -> 1992 MHz).  That is the wall in
# docs/PERFORMANCE-2026-09-22.md §4 — this script is how it was demonstrated.
#
#   sudo bench/cpu-oc-sweep.sh
set -u
D="$(cd "$(dirname "$0")" && pwd)"
MEMBW=${MEMBW:-$D/membw}
SUDO=${SUDO:-sudo}

{
echo "=== CPU OC SWEEP $(date -Is) ==="
echo "boost cap in service: $(grep -o 'max-floor [0-9]*' /etc/systemd/system/cpu-boost.service 2>/dev/null || echo '(none)')"
echo "little avail: $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies 2>/dev/null | tr ' ' '\n' | tail -4 | tr '\n' ' ')"
echo "big avail   : $(cat /sys/devices/system/cpu/cpufreq/policy6/scaling_available_frequencies 2>/dev/null | tr ' ' '\n' | tail -6 | tr '\n' ' ')"
$SUDO systemctl stop cpu-boost.service 2>/dev/null

echo
echo "--- BIG cluster (cpu6-7): throughput + all-core stress at each candidate level ---"
printf '%-10s %-12s %-16s %-10s %s\n' LEVEL actual_cur_1t sha256_1t temp kernel_errors
for f in 2002000 2100000 2200000 2300000 2400000; do
  echo "$f" | $SUDO tee /sys/devices/system/cpu/cpufreq/policy6/scaling_max_freq >/dev/null 2>&1
  sleep 1
  MARK=$(date +%s)
  cur=$(cat /sys/devices/system/cpu/cpu6/cpufreq/scaling_cur_freq 2>/dev/null)
  r=$(taskset -c 6 openssl speed -seconds 6 sha256 2>/dev/null | tail -1 | awk '{print $NF}')
  timeout 20 openssl speed -multi 8 -seconds 4 sha256 >/dev/null 2>&1
  [ -x "$MEMBW" ] && timeout 20 taskset -c 0-7 "$MEMBW" 1 128 >/dev/null 2>&1
  errs=$($SUDO journalctl -k --since "@$MARK" --no-pager 2>/dev/null \
         | grep -icE 'Oops|BUG:|Call trace|Unable to handle|watchdog|hung task|thermal.*critical')
  printf '%-10s %-12s %-16s %-10s %s\n' "$f" "$cur" "${r:-FAIL}" \
         "$(( $(cat /sys/class/thermal/thermal_zone1/temp) / 1000 ))C" "$errs"
  if [ -z "$r" ] || [ "$errs" -gt 0 ]; then echo "   ^^ INSTABILITY at $f — stopping sweep"; break; fi
  sleep 2
done

echo
echo "--- restore spec maximum and hand the clocks back to the boost controller ---"
echo 2002000 | $SUDO tee /sys/devices/system/cpu/cpufreq/policy6/scaling_max_freq >/dev/null
echo 1794000 | $SUDO tee /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq >/dev/null
$SUDO systemctl start cpu-boost.service 2>/dev/null
echo "final: big=$(cat /sys/devices/system/cpu/cpufreq/policy6/scaling_max_freq) little=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq)"
echo "=== DONE $(date -Is) ==="
} > "$D/cpu-oc-sweep.log" 2>&1
echo "written: $D/cpu-oc-sweep.log"
