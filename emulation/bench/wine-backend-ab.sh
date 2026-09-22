#!/bin/bash
# wine-backend-ab.sh — compare the two Windows CPU backends on this Hangover build.
#
#   wowbox64.dll     box64 0.4.4 emulating the PE code
#   libwow64fex.dll  FEX emulating the PE code
#
# Both ship; switching is one environment variable (HODLL), which makes this the
# cheapest unexplored lever on the Windows side.
#
# READ THIS BEFORE TRUSTING A RESULT FROM IT
# ------------------------------------------
# 1. A timed-out Wine run leaves orphaned services.exe / winedevice.exe /
#    plugplay.exe behind, and a stuck winedevice.exe IGNORES SIGTERM. Every later
#    launch then stalls, and a naive A/B reports whatever the leftovers dictate.
#    This script therefore purges with SIGKILL and ASSERTS an empty process table
#    before each run; if you write your own harness, do the same.
# 2. `ver` measures launch cost only (~1.0 s cold, ~0.2 s warm) and executes almost
#    no guest code, so it cannot rank the backends for emulated throughput. Use it
#    to prove a backend works and to compare launch cost; use a real game for speed.
# 3. A `cmd /c "for /L %i in (1,1,N) do ..."` one-liner HANGS on this build and was
#    the source of the orphaned processes above. Prefer a .bat file over the prefix
#    if you need a loop workload.
#
# usage: WORKLOAD='ver' REPS=3 ./wine-backend-ab.sh
set -u
export WINEPREFIX=${WINEPREFIX:-/home/radxa/.wine-dxvk}
export WINEDEBUG=${WINEDEBUG:--all}
export DISPLAY=${DISPLAY:-}
CMD='C:\windows\syswow64\cmd.exe'
WORKLOAD=${WORKLOAD:-ver}
REPS=${REPS:-3}
PROCS="wine wine64 wineserver services.exe winedevice.exe plugplay.exe explorer.exe cmd.exe conhost.exe rpcss.exe svchost.exe start.exe winemenubuilder.exe"

alive() { ps -eo comm 2>/dev/null | grep -cE '^(wineserver|services\.exe|winedevice\.exe|plugplay\.exe|cmd\.exe)$' || true; }

purge() {
  wineserver -k 2>/dev/null || true
  sleep 1
  for p in $PROCS; do pkill -x "$p" 2>/dev/null || true; done
  sleep 1
  for p in $PROCS; do pkill -9 -x "$p" 2>/dev/null || true; done
  for _ in $(seq 1 20); do
    [ "$(alive)" = "0" ] && break
    pkill -9 -x winedevice.exe 2>/dev/null || true
    sleep 0.5
  done
}

run() { # run <label> <hodll>
  local label=$1 hodll=$2 s e rc before
  purge
  before=$(alive)
  [ "$before" != "0" ] && { echo "$label: REFUSING, $before leftovers remain"; return; }
  s=$(date +%s.%N)
  timeout 120 env HODLL="$hodll" wine "$CMD" /c "$WORKLOAD" >/dev/null 2>&1
  rc=$?
  e=$(date +%s.%N)
  awk -v a="$s" -v b="$e" -v r="$rc" -v l="$label" \
    'BEGIN{printf "%-16s %7.2f s  exit=%-3d %s\n", l, b-a, r, (r==0?"ok":(r==124?"HANG":"fail"))}'
}

echo "# workdir=$WINEPREFIX workload=$WORKLOAD reps=$REPS  (cold launch each run)"
for rep in $(seq 1 "$REPS"); do
  echo "--- rep $rep ---"
  run "box64" wowbox64.dll
  run "fex"   libwow64fex.dll
done
purge
