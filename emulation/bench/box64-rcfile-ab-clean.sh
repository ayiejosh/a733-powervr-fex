#!/bin/bash
# box64-rcfile-ab-clean.sh — the A/B, with a purge that actually empties the table.
#
# Third attempt, and the reason the first two were wrong:
#   attempt 1 concluded "the rcfile hangs cmd.exe" -- but orphaned Wine service
#             processes from an earlier hung run were holding the prefix.
#   attempt 2 purged with SIGTERM, which a stuck winedevice.exe ignores, so every
#             trial still started with one leftover and the result was the same
#             confounded correlation.
# This one escalates to SIGKILL and asserts an empty process table before each
# trial, so "rcfile present" is the only thing that varies.
set -u
export WINEPREFIX=/home/radxa/.wine-dxvk WINEDEBUG=-all DISPLAY=
R=$WINEPREFIX/drive_c/users/radxa/.box64rc
FULL=$R.full
CMD='C:\windows\syswow64\cmd.exe'
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

trial() { # trial <label> <src|ABSENT>
  local label=$1 src=$2 s e rc before
  if [ "$src" = "ABSENT" ]; then rm -f "$R"; else cp "$src" "$R"; fi
  purge
  before=$(alive)
  s=$(date +%s.%N)
  timeout 40 env HODLL=wowbox64.dll wine "$CMD" /c ver >/dev/null 2>&1
  rc=$?
  e=$(date +%s.%N)
  awk -v a="$s" -v b="$e" -v r="$rc" -v l="$label" -v lo="$before" \
    'BEGIN{printf "%-18s %7.2f s  exit=%-3d %-5s leftovers=%s\n", l, b-a, r, (r==0?"ok":(r==124?"HANG":"fail")), lo}'
}

# Never leave the prefix without a usable rcfile.
trap 'cp "$FULL" "$R" 2>/dev/null || rm -f "$R"' EXIT

echo "initial purge: leftovers now $(purge; alive)"
for rep in $(seq 1 "$REPS"); do
  echo "--- rep $rep ---"
  trial "no rcfile"   ABSENT
  trial "full tuned"  "$FULL"
done
