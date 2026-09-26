#!/bin/bash
# chrome-dissect.sh — attribute a Chrome launch to processes, CPU, waits and swap.
#
# Why. The previous study measured a single number (262 s -> 32 s) and one internal
# attribution (CompileBlock ~72% of *FEX* time). Neither says where the wall clock
# goes: how much is CPU, how much is blocked, how many FEX processes a launch spawns,
# and whether each one re-pays init+relocation. This samples the whole subtree so the
# 32 s can be decomposed instead of guessed.
#
# Cost control. This board runs ~270 processes; reading every /proc/<pid>/stat every
# tick cost 43% of a core (measured), which would perturb what it measures. So:
#   - /proc/stat is read every tick (one file) -> system CPU, iowait, ambient load
#   - the pid->ppid map is rebuilt once per SCAN_EVERY ticks, then only the subtree of
#     the launched pid is sampled; everything else stays in the aggregate
#
# Output per run, in $OUTDIR/<label>.*:
#   .tsv   records appended each tick:
#            S <t_ms> <user> <nice> <system> <idle> <iowait> <irq> <softirq> <steal>
#            G <t_ms> <memfree_kb> <cached_kb> <swapcached_kb> <pswpin> <pswpout> <pgmajfault>
#            P <t_ms> <pid> <ppid> <comm> <state> <utime> <stime> <minflt> <majflt> \
#              <rss_pages> <read_bytes> <write_bytes>
#   .dom / .err / .log
#
# Recipes:
#   headless   the deterministic --dump-dom recipe used for the 262 s -> 32 s result
#   headed     what the user actually clicks: FEXInterpreter + x11 GUI launcher flags
#   raw        pass everything after the recipe straight through
#
# usage: ./chrome-dissect.sh <label> <recipe> [-- <extra args...>]
#        TICK_MS=100 SCAN_EVERY=10 ./chrome-dissect.sh warm headless
set -u

LABEL=${1:?usage: chrome-dissect.sh <label> <recipe> [-- extra args]}
RECIPE=${2:?recipe: headless|headed|raw}
shift 2
[ "${1:-}" = "--" ] && shift

ROOTFS=${ROOTFS:-/home/radxa/crd-rootfs}
GHOME=$ROOTFS/home/crd
CACHE=$GHOME/.cache/fex-emu
CHROME=$ROOTFS/opt/google/chrome/chrome
OUTDIR=${OUTDIR:-/home/radxa/fex-tune/chrome-runs}
TSV=$OUTDIR/$LABEL.tsv
STOP=/tmp/dissect.stop
ROOTFILE=/tmp/dissect.root
TICK_MS=${TICK_MS:-200}
SCAN_EVERY=${SCAN_EVERY:-5}
IO_EVERY=${IO_EVERY:-5}
TIMEOUT_S=${TIMEOUT_S:-900}
PROFILE=${PROFILE:-/home/radxa/chrome-data}
PAGE=${PAGE:-file:///tmp/heavy.html}


# --- precheck: the target binary. The x86-64 Chrome was removed from this board on
# 2026-09-22 (docs section 15). This harness is kept as the documented method -- it works
# again if Chrome is reinstalled, or if CHROME/ROOTFS are pointed at another guest binary.
[ -x "$CHROME" ] || {
  echo "target not found: $CHROME" >&2
  echo "  the x86-64 Chrome was removed 2026-09-22 -- see docs section 15" >&2
  exit 2
}

# --- precheck: without the kernel mounts the guest /proc is an empty dir and Chrome
# does not fail fast, it hangs. Warn loudly rather than burn the timeout.
for m in proc dev; do
  if ! mountpoint -q "$ROOTFS/$m"; then
    echo "WARNING: $ROOTFS/$m is NOT a mountpoint -- guest sees an empty /$m." >&2
    echo "         Chrome will hang here. Run: sudo $PWD/rootfs-mounts.sh" >&2
  fi
done

mkdir -p "$OUTDIR" "$GHOME/.fex-emu"

# --- the workload page is generated if absent. An earlier round of benchmarking was
# invalidated because a workload file under /tmp was deleted by a cleanup and the script
# silently measured nothing. Committed tooling must not depend on a file in /tmp.
if [ ! -s /tmp/heavy.html ]; then
  {
    echo '<!doctype html><html><head><meta charset=utf-8><title>heavy</title>'
    echo '<style>.row{display:block;padding:1px} b{color:#08f}</style></head><body>'
    echo '<h1>heavy dom</h1><div id=root>'
    for i in $(seq 1 4000); do
      echo "<div class=\"row\" id=\"r$i\"><span>item $i</span><b>$(( i * 7 % 97 ))</b></div>"
    done
    echo '</div></body></html>'
  } > /tmp/heavy.html
  echo "# generated workload /tmp/heavy.html ($(wc -c < /tmp/heavy.html) bytes)"
fi

# --- the guest-HOME config: DiskCache on, TSO left at default on purpose -----------
# FEX resolves ~/.fex-emu/Config.json from HOME, and these launchers override HOME,
# so writing the host config is not enough. See docs section 13.
cat > "$GHOME/.fex-emu/Config.json" <<'JSON'
{
 "Config": {
  "DiskCache": "1",
  "Multiblock": "1",
  "X87ReducedPrecision": "1"
 }
}
JSON

# --- environment. Three toggles exist because the known-good headless run and the
# user-facing GUI launchers do NOT run the same way, and that difference turned out to
# matter.  DESKTOP_ENV=0 THUNKS=0 LAUNCHER=direct reproduces the 32 s headless result:
# direct exec is routed to FEXInterpreter by the binfmt_misc FEX-x86_64 handler, with
# the FEX_* vars inherited.  The GUI launchers additionally set desktop + thunk vars.
export FEX_ROOTFS=$ROOTFS
export HOME=$GHOME USER=crd XDG_RUNTIME_DIR=/tmp/fexrun
if [ "${DESKTOP_ENV:-1}" = 1 ]; then
  export DISPLAY=:0 XAUTHORITY=/home/radxa/.Xauthority
  export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/1000/bus"
  export LD_LIBRARY_PATH=/usr/local/lib
  export FEX_SERVERSOCKETPATH=/run/user/1000/1000.FEXServer.Socket
fi
if [ "${THUNKS:-1}" = 1 ]; then
  export FEX_THUNKHOSTLIBS=/home/radxa/FEX-src/build-native/HostLibs_64
  export FEX_THUNKGUESTLIBS=/usr/share/fex-emu/GuestThunks
  export FEX_THUNKCONFIG=/home/radxa/.fex-emu/ThunkConfig.json
fi
export FEX_ENABLECODECACHINGWIP=${FEX_ENABLECODECACHINGWIP:-0}
export FEX_ENABLELAZYCODECACHINGWIP=${FEX_ENABLELAZYCODECACHINGWIP:-0}
mkdir -p /tmp/fexrun "$PROFILE"; chmod 700 /tmp/fexrun

# --- hygiene: two Chrome trees stacked on top of each other would poison the numbers
pkill -f 'opt/google/chrome/chrome' 2>/dev/null; sleep 1
pkill -9 -f 'opt/google/chrome/chrome' 2>/dev/null; sleep 1

pgrep -x FEXServer >/dev/null || { /opt/fex/bin/FEXServer >/dev/null 2>&1 & disown; sleep 2; }

case $RECIPE in
headless)
  CHROME_ARGS=(
    --headless --no-sandbox --no-zygote --disable-gpu --in-process-gpu
    --disable-dev-shm-usage --ipc-connection-timeout=3600
    --no-first-run --disable-extensions --disable-background-networking --no-pings
    --metrics-recording-only --disable-default-apps --disable-sync
    --user-data-dir="$PROFILE" --dump-dom "$PAGE"
  )
  ;;
headed)
  CHROME_ARGS=(
    --no-sandbox --ozone-platform=x11 --disable-gpu --disable-gpu-compositing
    --in-process-gpu --ipc-connection-timeout=3600
    --no-first-run --no-default-browser-check
    --disable-extensions --disable-background-networking --no-pings
    --metrics-recording-only --disable-default-apps --disable-sync
    --enable-logging=stderr --v=0
    --user-data-dir="$PROFILE"
  )
  ;;
raw) CHROME_ARGS=() ;;
*) echo "unknown recipe: $RECIPE" >&2; exit 2;;
esac

# --- the sampler ------------------------------------------------------------------
rm -f "$STOP" "$ROOTFILE" "$TSV"
: > "$TSV"; : > "$ROOTFILE"
SAMPLER_PID=

sampler() {
  local line rest pid comm state ppid ut st minf majf rss rd wr i=0
  local root= start_ns now_ns t_ms memfree cached swapcached pswpin pswpout pgmajfault
  local -a F
  local -A PARENT=() COMM=() SUB=() RD=() WR=()
  local u n sy id io irq sirq steal
  local SLEEP_S
  SLEEP_S=$(awk -v ms="$TICK_MS" 'BEGIN{printf "%.3f", ms/1000}')

  # wait for the launcher's pid, then start the clock so t=0 is the exec
  while [ ! -s "$ROOTFILE" ]; do [ -e "$STOP" ] && return; sleep 0.005; done
  root=$(cat "$ROOTFILE")
  start_ns=$(date +%s%N)

  while [ ! -e "$STOP" ]; do
    now_ns=$(date +%s%N); t_ms=$(( (now_ns-start_ns)/1000000 ))

    # --- aggregate system CPU (cheap, one read) ---
    read -r _ u n sy id io irq sirq steal _ < /proc/stat
    printf 'S\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$t_ms" "$u" "$n" "$sy" "$id" "$io" "$irq" "$sirq" "$steal" >> "$TSV"

    # --- memory / swap, one read each ---
    memfree=0; cached=0; swapcached=0
    while read -r k v _; do
      case $k in MemFree:) memfree=$v;; Cached:) cached=$v;; SwapCached:) swapcached=$v;; esac
    done < /proc/meminfo
    pswpin=0; pswpout=0; pgmajfault=0
    while read -r k v _; do
      case $k in pswpin:) pswpin=$v;; pswpout:) pswpout=$v;; pgmajfault:) pgmajfault=$v;; esac
    done < /proc/vmstat
    printf 'G\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$t_ms" "$memfree" "$cached" "$swapcached" "$pswpin" "$pswpout" "$pgmajfault" >> "$TSV"

    # --- rebuild pid->ppid and the subtree, sparsely ---
    if [ $((i % SCAN_EVERY)) -eq 0 ]; then
      PARENT=(); COMM=(); SUB=()
      for d in /proc/[0-9]*; do
        { IFS= read -r line < "$d/stat"; } 2>/dev/null || continue
        [ -n "$line" ] || continue
        pid=${line%% *}; rest=${line#*) }
        read -r -a F <<< "$rest"
        PARENT[$pid]=${F[1]}
        comm=${line#*(}; COMM[$pid]=${comm%)*}
      done
      SUB[$root]=1
      local changed=1 pass
      for ((pass=0; pass<8 && changed; pass++)); do      # fixpoint over tree depth
        changed=0
        for pid in "${!PARENT[@]}"; do
          [ -n "${SUB[$pid]:-}" ] && continue
          if [ -n "${SUB[${PARENT[$pid]}]:-}" ]; then SUB[$pid]=1; changed=1; fi
        done
      done
    fi

    # --- the subtree, every tick ---
    for pid in "${!SUB[@]}"; do
      d=/proc/$pid
      { IFS= read -r line < "$d/stat"; } 2>/dev/null || continue
      [ -n "$line" ] || continue
      rest=${line#*) }
      read -r -a F <<< "$rest"
      state=${F[0]} ppid=${F[1]} minf=${F[7]} majf=${F[9]}
      ut=${F[11]} st=${F[12]} rss=${F[21]}
      comm=${COMM[$pid]:-?}
      if [ $((i % IO_EVERY)) -eq 0 ] || [ -z "${RD[$pid]:-}" ]; then
        rd=0; wr=0
        { while read -r k v; do
            case $k in read_bytes:) rd=$v;; write_bytes:) wr=$v;; esac
          done < "$d/io"; } 2>/dev/null
        RD[$pid]=$rd; WR[$pid]=$wr
      else
        rd=${RD[$pid]}; wr=${WR[$pid]}
      fi
      printf 'P\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$t_ms" "$pid" "$ppid" "$comm" "$state" "$ut" "$st" "$minf" "$majf" "$rss" "$rd" "$wr" >> "$TSV"
    done
    i=$((i+1))
    sleep "$SLEEP_S"
  done
}

echo "# label=$LABEL recipe=$RECIPE tick=${TICK_MS}ms scan_every=${SCAN_EVERY}"
echo "# launcher=${LAUNCHER:-interp} thunks=${THUNKS:-1} desktop_env=${DESKTOP_ENV:-1}"
echo "# cache before: $(du -sh "$CACHE" 2>/dev/null | cut -f1)"
# the sampler's stderr goes to its own file: a crashed sampler must never again look
# like a hung launch (that cost one 600 s blind run).
sampler 2> "$OUTDIR/$LABEL.sampler.err" & SAMPLER_PID=$!

# --- launch: write the pid so the sampler can root its subtree there ---------------
# PIN is not a tuning experiment: it is load-bearing. Unpinned, two Chrome children
# die at sandbox/linux/services/thread_helpers.cc:41 with ENOENT and the browser then
# hangs on IPC forever (measured: rc=124 at 100 s, 2 FATALs). Pinned to the two A76
# cores the same command returns rc=0 in 33 s. See docs section 14.
RUNNER=()
[ "${PIN:-0-7}" != none ] && RUNNER=(taskset -c "${PIN:-0-7}")
case ${LAUNCHER:-interp} in
  interp) RUNNER+=(/opt/fex/bin/FEXInterpreter) ;;
  direct) : ;;                             # binfmt_misc FEX-x86_64 routes it for us
  *) echo "unknown LAUNCHER: $LAUNCHER" >&2; exit 2;;
esac
s=$(date +%s%N)
timeout "$TIMEOUT_S" "${RUNNER[@]}" "$CHROME" "${CHROME_ARGS[@]}" \
  > "$OUTDIR/$LABEL.dom" 2> "$OUTDIR/$LABEL.err" &
CHILD=$!
echo "$CHILD" > "$ROOTFILE"
wait "$CHILD"; rc=$?
e=$(date +%s%N)
wall_ms=$(( (e-s)/1000000 ))

sleep 0.3
touch "$STOP"
wait "$SAMPLER_PID" 2>/dev/null
pkill -f 'opt/google/chrome/chrome' 2>/dev/null

{
  echo "label=$LABEL recipe=$RECIPE rc=$rc wall_ms=$wall_ms"
  echo "dombytes=$(wc -c < "$OUTDIR/$LABEL.dom" 2>/dev/null || echo 0)"
  echo "cache_after=$(du -sh "$CACHE" 2>/dev/null | cut -f1)"
  echo "tsv=$TSV  ticks=$(grep -c $'^S' "$TSV" 2>/dev/null)"
  [ -s "$OUTDIR/$LABEL.sampler.err" ] && echo "SAMPLER STDERR: $(head -c 300 "$OUTDIR/$LABEL.sampler.err")"
  true
} | tee "$OUTDIR/$LABEL.log"
