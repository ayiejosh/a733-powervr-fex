#!/bin/bash
# apptest.sh NAME MODE WAIT CMD... - start CMD in the held desktop session (daily.sh HOLD=1), wait WAIT
# seconds, screenshot the screen, check the picture (vcheck.py MODE: full|screen, or none), then stop
# CMD's whole process group. Prints one line: still running?, picture, GPU errors in the kernel log.
# Output goes to the held run's directory as app-NAME.{log,png}.
set -u
NAME=$1 MODE=$2 WAIT=$3; shift 3
A=$(dirname "$(readlink -f "$0")")
O=$(cat /tmp/daily-hold 2>/dev/null); [ -d "$O" ] || { echo "$NAME: no held desktop (/tmp/daily-hold)"; exit 1; }
T0=$(date "+%Y-%m-%d %H:%M:%S")
setsid $A/inenv.sh "$@" > $O/app-$NAME.log 2>&1 < /dev/null & pid=$!
sleep $WAIT
if kill -0 $pid 2>/dev/null; then run=running; else wait $pid; run="EXITED rc=$?"; fi
$A/inenv.sh python3 $A/shot.py $O/app-$NAME.png > /dev/null 2>&1
if [ ! -s $O/app-$NAME.png ]; then vis="screenshot FAILED"
elif [ $MODE = none ]; then vis="screenshot taken"
else vis=$(python3 $A/vcheck.py $MODE $O/app-$NAME.png); fi
kill -TERM -- -$pid 2>/dev/null; sleep 2; kill -KILL -- -$pid 2>/dev/null
kerr=$(sudo -n journalctl -k --since "$T0" | grep -v PVRDBG | grep -ciE "2abc0069|Job timeout|pvr_cccb|page fault|mmu fault|lockup|hard reset|Oops|segfault")
echo "$(date +%T) $NAME: $run; $vis; kernel-errors=$kerr" | tee -a $O/apps.txt
