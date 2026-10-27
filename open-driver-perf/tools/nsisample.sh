#!/bin/bash
# nsisample.sh "CMD..." [WINDOW_US] [DELAY_S]: run CMD (a glmark2 launcher) in the background, open the NSI PMU window DELAY_S seconds in,
# print the per-master read/write bandwidth and read latency columns (gpu = column 7, total = column 22), then close the window.
# Only pmu_timer is written (PMU window); port_prio/port_qos/port_mode are never touched. Undo: echo 0 > pmu_timer.
P=/sys/devices/platform/soc@3000000/2020000.nsi-controller/nsi-pmu/hwmon0
CMD=$1; W=${2:-1000000}; D=${3:-3}
bash -c "$CMD" > /tmp/nsi-glm.txt 2>&1 &
sleep $D
echo $W | sudo tee $P/pmu_timer >/dev/null
sleep $(python3 -c "print($W/1e6+0.4)")
echo "names: $(sudo cat $P/available_pmu | tr -s ' ' | cut -c1-200)"
for f in pmu_bandwidth_rd pmu_bandwidth_wr pmu_latency_rd; do echo "$f: $(sudo cat $P/$f | tr -s ' ')"; done
echo "timer after: $(sudo cat $P/pmu_timer)"
wait
grep -o "FPS: [0-9]*" /tmp/nsi-glm.txt | tail -n 1
echo 0 | sudo tee $P/pmu_timer >/dev/null
