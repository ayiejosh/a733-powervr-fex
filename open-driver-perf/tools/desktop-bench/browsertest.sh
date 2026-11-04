#!/bin/bash
# browsertest.sh NAME CMD... - run a browser on page.html in the held desktop session (daily.sh HOLD=1),
# screenshot and check each page phase (webgl, css, video) like daily.sh, and print the page's results.
# CMD must open file://$A/page.html; for Firefox pass a prepared --profile.
set -u
NAME=$1; shift
A=$(dirname "$(readlink -f "$0")")
O=$(cat /tmp/daily-hold 2>/dev/null); [ -d "$O" ] || { echo "$NAME: no held desktop (/tmp/daily-hold)"; exit 1; }
log=$O/bt-$NAME.log
setsid $A/inenv.sh "$@" > $log 2>&1 < /dev/null & pid=$!
declare -A due=(); fails=0
for i in $(seq 1 120); do sleep 1
   for ph in webgl css video; do
      [ -z "${due[$ph]:-}" ] && grep -qE "phase.{1,6}name.{1,6}$ph" $log && due[$ph]=$((i + 4))
      if [ "${due[$ph]:-}" = $i ]; then
         $A/inenv.sh python3 $A/shot.py $O/bt-$NAME-$ph.png > /dev/null 2>&1
         v=$(python3 $A/vcheck.py $( [ $ph = video ] && echo video || echo full ) $O/bt-$NAME-$ph.png) || fails=$((fails + 1))
         echo "  $ph: $v"
      fi
   done
   grep -qE 'test.{1,6}done' $log && break
done
for ph in webgl css video; do [ -n "${due[$ph]:-}" ] || { fails=$((fails + 1)); echo "  $ph: never started"; }; done
kill -TERM -- -$pid 2>/dev/null; sleep 3; kill -KILL -- -$pid 2>/dev/null
echo "$NAME: visual-fails=$fails $(grep -o 'DAILYBENCH {[^}]*}' $log | tr -d '\\' | grep -v 'phase\|"done"' | sed 's/DAILYBENCH //' | cut -c1-110 | tr '\n' ' ')" | tee -a $O/browsers.txt
