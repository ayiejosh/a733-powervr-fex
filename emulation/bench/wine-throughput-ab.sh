#!/bin/bash
# wine-throughput-ab.sh — measure the Windows path on a real emulated workload.
#
# Why: `cmd /c ver` measures launch cost, not emulation. This uses an emulated
# i386 PE builtin (`syswow64\find.exe`) scanning a large file, so the measurement
# is dominated by guest code executing under the emulator rather than by process
# setup.
#
# Questions:
#   1. Do the BOX64_* environment variables (the only rcfile-free route on this
#      build, and the one box64 reports as applied) actually change throughput?
#   2. Is the FEX Windows backend faster than box64's for emulated work? Both
#      ship; only the environment variable HODLL switches them.
#
# Discipline: a full SIGKILL purge and an asserted-empty process table before
# every run. A timed-out Wine run leaves orphans that make every later launch
# stall, which is exactly how the earlier rcfile investigation fooled itself.
#
# usage: REPS=3 ./wine-throughput-ab.sh
set -u
export WINEPREFIX=${WINEPREFIX:-/home/radxa/.wine-dxvk}
export WINEDEBUG=${WINEDEBUG:--all}
export DISPLAY=${DISPLAY:-}
FIND='C:\windows\syswow64\find.exe'
TARGET=${TARGET:-'C:\bench200.txt'}
NEEDLE=${NEEDLE:-the}
REPS=${REPS:-3}
PROCS="wine wine64 wineserver services.exe winedevice.exe plugplay.exe explorer.exe cmd.exe conhost.exe rpcss.exe svchost.exe start.exe winemenubuilder.exe"

alive() { ps -eo comm 2>/dev/null | grep -cE '^(wineserver|services\.exe|winedevice\.exe|plugplay\.exe|cmd\.exe)$' || true; }
purge() {
  wineserver -k 2>/dev/null || true; sleep 1
  for p in $PROCS; do pkill -x "$p" 2>/dev/null || true; done; sleep 1
  for p in $PROCS; do pkill -9 -x "$p" 2>/dev/null || true; done
  for _ in $(seq 1 20); do
    [ "$(alive)" = "0" ] && break
    pkill -9 -x winedevice.exe 2>/dev/null || true
    sleep 0.5
  done
}

# label | HODLL | extra env (space separated, may be empty)
variants=(
  "box64-default|wowbox64.dll|"
  "box64-callret|wowbox64.dll|BOX64_DYNAREC_CALLRET=1"
  "box64-tuned|wowbox64.dll|BOX64_DYNAREC_CALLRET=1 BOX64_DYNAREC_BIGBLOCK=3 BOX64_DYNAREC_FORWARD=512"
  "fex-backend|libwow64fex.dll|"
)

run_one() { # run_one <label> <hodll> <extra>
  local label=$1 hodll=$2 extra=$3 s e rc before
  purge
  before=$(alive)
  if [ "$before" != "0" ]; then echo "$label REFUSING ($before leftovers)"; return; fi
  s=$(date +%s.%N)
  # shellcheck disable=SC2086
  timeout 300 env HODLL="$hodll" $extra wine "$FIND" /C "$NEEDLE" "$TARGET" >/tmp/wt-$label.txt 2>&1
  rc=$?
  e=$(date +%s.%N)
  local count
  count=$(grep -oE ': [0-9]+' /tmp/wt-$label.txt | tail -1 | tr -d ': ')
  awk -v a="$s" -v b="$e" -v r="$rc" -v l="$label" -v c="${count:-none}" \
    'BEGIN{printf "%-14s %8.2f s exit=%-3d lines=%-8s %s\n", l, b-a, r, c, (r==0?"ok":(r==124?"HANG":"fail"))}'
}

echo "# wine throughput A/B  target=$TARGET reps=$REPS"
for rep in $(seq 1 "$REPS"); do
  echo "--- rep $rep ---"
  for v in "${variants[@]}"; do
    IFS='|' read -r label hodll extra <<< "$v"
    run_one "$label" "$hodll" "$extra"
  done
done
purge
