#!/bin/bash
# session4.sh: compile statistics of all 33 scenes for pc3 default / PCO_FORCE_FAST / PCO_SCHED_THRESHOLD, then A/Bs.
cd ~/open-perf
T=pc3; NC="MESA_SHADER_CACHE_DISABLE=1"
echo "## bsession start $(date +%T)"; tools/bsession.sh start 2>&1 | tail -n 2
[ "$(basename $(readlink /sys/bus/platform/devices/1800000.gpu/driver))" = powervr ] || { echo "NOT ON OPEN STACK"; echo "SESSION4 DONE"; exit 1; }
echo "## dump33 default $(date +%T)"; python3 tools/dump33.py $T /tmp/d33-default > /tmp/d33-default.txt 2>&1; tail -n 3 /tmp/d33-default.txt | cut -c1-150
echo "## dump33 FORCE_FAST $(date +%T)"; DUMP_EXTRA="PCO_FORCE_FAST=1" python3 tools/dump33.py $T /tmp/d33-fast > /tmp/d33-fast.txt 2>&1
echo "## dump33 SCHED=20 $(date +%T)"; DUMP_EXTRA="PCO_SCHED_THRESHOLD=20" python3 tools/dump33.py $T /tmp/d33-s20 > /tmp/d33-s20.txt 2>&1
python3 tools/tempcmp.py default=/tmp/d33-default.txt fast=/tmp/d33-fast.txt s20=/tmp/d33-s20.txt > /tmp/d33-compare.txt 2>&1
echo "## A/B FORCE_FAST (timing-only upper bound of a two-binary fast variant) $(date +%T)"; tools/ab.sh $T "$NC" "$NC PCO_FORCE_FAST=1" 3 2>&1 | tail -n 13
echo "## A/B SCHED=20 $(date +%T)"; tools/ab.sh $T "$NC" "$NC PCO_SCHED_THRESHOLD=20" 3 2>&1 | tail -n 13
echo "## bsession stop $(date +%T)"; tools/bsession.sh stop 2>&1 | tail -n 2
echo "SESSION4 DONE $(date +%T)"
