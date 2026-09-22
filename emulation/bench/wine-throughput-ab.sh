#!/bin/bash
# wine-throughput-ab.sh — measure the Windows path on a real emulated workload.
#
# Why: `cmd /c ver` measures launch cost, not emulation. This uses an emulated
# i386 PE builtin (`syswow64\find.exe`) scanning a large file, so the measurement
# is dominated by guest code executing under the emulator rather than by process
# setup.
#
# Questions it answers:
#   1. Do the BOX64_* environment variables (the only rcfile-free route on this
#      build, and the one box64 reports as applied) change throughput?
#   2. Is the FEX Windows backend (`HODLL=libwow64fex.dll`, the 32-bit WoW64 path)
#      faster than box64's? Both ship; only that environment variable switches them.
#
# Discipline, learned the hard way:
#   - a full SIGKILL purge and an asserted-empty process table before every run:
#     a timed-out Wine run leaves orphans that make every later launch stall;
#   - the workload file lives inside the prefix and is easy to lose to a cleanup.
#     It was lost once, every run then did startup-only work, and the output still
#     looked like data (12 rows of noise). It is now created on demand and the
#     script exits non-zero if no run actually succeeded.
#
# usage: REPS=3 ./wine-throughput-ab.sh
set -u
export WINEPREFIX=${WINEPREFIX:-/home/radxa/.wine-dxvk}
export WINEDEBUG=${WINEDEBUG:--all}
export DISPLAY=${DISPLAY:-}
FIND='C:\windows\syswow64\find.exe'
TARGET=${TARGET:-'C:\bench100.txt'}
NEEDLE=${NEEDLE:-the}
REPS=${REPS:-3}
ROWS=${ROWS:-2000000}
PROCS="wine wine64 wineserver services.exe winedevice.exe plugplay.exe explorer.exe cmd.exe conhost.exe rpcss.exe svchost.exe start.exe winemenubuilder.exe"
RC_LOG=/tmp/wt-rc.txt

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

# Create the workload if a cleanup removed it. Refuse to run without it: startup
# timings from a missing target are worse than no data, because they look like data.
host_target=$(printf '%s' "$TARGET" | sed "s|^C:|$WINEPREFIX/drive_c|; s|\\\\|/|g")
if [ ! -s "$host_target" ]; then
  echo "# creating workload $host_target (~$((ROWS/20000)) MB)"
  awk -v n="$ROWS" 'BEGIN{for(i=1;i<=n;i++) print i" the quick brown fox jumps over the lazy dog"}' > "$host_target" \
    || { echo "FATAL: cannot create workload"; exit 2; }
fi
[ -s "$host_target" ] || { echo "FATAL: workload missing at $host_target"; exit 2; }
echo "# workload: $(du -h "$host_target" | cut -f1)  rows=$ROWS"
: > "$RC_LOG"

# label | HODLL | extra env (space separated, may be empty)
variants=(
  "box64-default|wowbox64.dll|"
  "box64-callret|wowbox64.dll|BOX64_DYNAREC_CALLRET=1"
  "box64-tuned|wowbox64.dll|BOX64_DYNAREC_CALLRET=1 BOX64_DYNAREC_BIGBLOCK=3 BOX64_DYNAREC_FORWARD=512"
  "fex-backend|libwow64fex.dll|"
)

run_one() { # run_one <label> <hodll> <extra>
  local label=$1 hodll=$2 extra=$3 s e rc before count
  purge
  before=$(alive)
  if [ "$before" != "0" ]; then echo "$label REFUSING ($before leftovers)"; return; fi
  s=$(date +%s.%N)
  # shellcheck disable=SC2086
  timeout 300 env HODLL="$hodll" $extra wine "$FIND" /C "$NEEDLE" "$TARGET" >/tmp/wt-$label.txt 2>&1
  rc=$?
  e=$(date +%s.%N)
  echo "$rc" >> "$RC_LOG"
  count=$(grep -oE ': [0-9]+' /tmp/wt-$label.txt | tail -1 | tr -d ': ')
  awk -v a="$s" -v b="$e" -v r="$rc" -v l="$label" -v c="${count:-none}" -v want="$ROWS" \
    'BEGIN{printf "%-14s %8.2f s exit=%-3d lines=%-9s %s\n", l, b-a, r, c,
      (r!=0 ? "FAIL" : (c==want ? "ok" : "WRONG-COUNT"))}'
}

echo "# wine throughput A/B  target=$TARGET reps=$REPS  (cold launch each run)"
for rep in $(seq 1 "$REPS"); do
  echo "--- rep $rep ---"
  for v in "${variants[@]}"; do
    IFS='|' read -r label hodll extra <<< "$v"
    run_one "$label" "$hodll" "$extra"
  done
done
purge

ok=$(grep -c '^0$' "$RC_LOG" 2>/dev/null || echo 0)
echo "# runs with exit 0: $ok of $(wc -l < "$RC_LOG")"
if [ "$ok" = "0" ]; then
  echo "FATAL: no run succeeded — the timings above are meaningless, not results"
  exit 3
fi
