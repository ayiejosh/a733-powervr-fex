#!/bin/bash
# chrome-launch-test.sh — does the JIT disk cache fix Chrome's launch on this board?
#
# Context. A cold Chrome launch under FEX measured 265 s, and no JIT cache was ever
# written, because the existing chrome-fex-*.sh scripts override HOME to the guest
# rootfs home -- and FEX resolves ~/.fex-emu/Config.json from HOME. So every Chrome
# launch on this board has run with DEFAULT FEX config: DiskCache off, TSO on, no
# x87 tuning. That is what this measures the fix for.
#
# The config written for the guest HOME is deliberately a SAFE SUBSET: DiskCache on,
# Multiblock on, X87ReducedPrecision on -- but TSOEnabled is left at its default.
# Upstream calls TSO-off "highly likely to break any multithreaded application", and
# this study measured real store-order violations with it (35 in 2.15M pairs). A
# browser is exactly the heavily-threaded, lock-free code where that would bite, so
# the risk is not taken here by default; it is measured separately if wanted.
#
# Variants, all with the working headless recipe from chrome-fex-headless-test.sh:
#   cold      cache cleared first, DiskCache on      what a first-ever launch costs
#   warm      cache reused                           what a normal launch costs
#   warm2     cache reused again                     stability check
#   warm-unp  cache reused, NOT pinned to 2 cores    does pinning hurt a browser?
#
# usage: ./chrome-launch-test.sh
set -u
ROOTFS=/home/radxa/crd-rootfs
GHOME=$ROOTFS/home/crd
CACHE=$GHOME/.cache/fex-emu
CHROME=$ROOTFS/opt/google/chrome/chrome
OUTDIR=/home/radxa/fex-tune/chrome-runs
mkdir -p "$OUTDIR" "$GHOME/.fex-emu"

cat > "$GHOME/.fex-emu/Config.json" <<'JSON'
{
 "Config": {
  "DiskCache": "1",
  "Multiblock": "1",
  "X87ReducedPrecision": "1"
 }
}
JSON
echo "# guest-HOME config written to $GHOME/.fex-emu/Config.json (TSO left at default on purpose)"

export FEX_ROOTFS=$ROOTFS
export HOME=$GHOME USER=crd XDG_RUNTIME_DIR=/tmp/fexrun
export FEX_ENABLECODECACHINGWIP=0 FEX_ENABLELAZYCODECACHINGWIP=0
mkdir -p /tmp/fexrun /home/radxa/chrome-data; chmod 700 /tmp/fexrun
pgrep -x FEXServer >/dev/null || { /opt/fex/bin/FEXServer & disown; sleep 2; }

run_chrome() { # run_chrome <label> <pin|nopin> <diskcache>
  local label=$1 pin=$2 dc=$3 s e rc
  local pinner=()
  [ "$pin" = pin ] && pinner=(taskset -c 6,7)
  s=$(date +%s)
  timeout 900 env FEX_DISKCACHE="$dc" "${pinner[@]}" "$CHROME" \
    --headless --no-sandbox --no-zygote --disable-gpu --in-process-gpu \
    --disable-dev-shm-usage --ipc-connection-timeout=3600 \
    --no-first-run --disable-extensions --disable-background-networking --no-pings \
    --metrics-recording-only --disable-default-apps --disable-sync \
    --user-data-dir=/home/radxa/chrome-data \
    --dump-dom file:///tmp/heavy.html > "$OUTDIR/$label.dom" 2> "$OUTDIR/$label.err"
  rc=$?
  e=$(date +%s)
  printf '%-12s rc=%-3d wall=%4ds dombytes=%-8s cache=%s\n' \
    "$label" "$rc" "$((e-s))" "$(wc -c < "$OUTDIR/$label.dom" 2>/dev/null || echo 0)" \
    "$(du -sh "$CACHE" 2>/dev/null | cut -f1)"
}

echo "# baseline: what the existing scripts do today (defaults, no cache)"
run_chrome nocache pin 0

echo "# the fix: DiskCache on, cold then warm"
rm -rf "$CACHE"
run_chrome cold  pin 1
run_chrome warm  pin 1
run_chrome warm2 pin 1

echo "# does pinning to the two A76s hurt a multithreaded browser?"
run_chrome warm-unp nopin 1

echo "# final cache size: $(du -sh "$CACHE" 2>/dev/null | cut -f1)"
