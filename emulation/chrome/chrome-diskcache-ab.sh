#!/bin/bash
# chrome-diskcache-ab.sh — is the Chrome launch win really DiskCache, or warm-up?
#
# The first measurement showed nocache 262 s vs DiskCache-cold 59 s, which cannot be
# right on its face: both runs had to translate Chrome from scratch, and writing a
# 554 MB cache while compiling is overhead, not a 4x speedup. The likely confound is
# run order -- the first Chrome launch of a session also pays for reading a 276 MB
# binary and its libraries off slow storage, for the FEXServer's own warm-up, and for
# Chrome creating its profile.
#
# So: everything is warm now, the cache exists, and this alternates no-cache and
# warm-cache runs. The difference that survives has to be the disk cache.
#
# usage: REP=2 ./chrome-diskcache-ab.sh
set -u
ROOTFS=/home/radxa/crd-rootfs
GHOME=$ROOTFS/home/crd
CACHE=$GHOME/.cache/fex-emu
CHROME=$ROOTFS/opt/google/chrome/chrome
OUTDIR=/home/radxa/fex-tune/chrome-runs
REP=${REP:-2}
mkdir -p "$OUTDIR" "$GHOME/.fex-emu"

export FEX_ROOTFS=$ROOTFS
export HOME=$GHOME USER=crd XDG_RUNTIME_DIR=/tmp/fexrun
export FEX_ENABLECODECACHINGWIP=0 FEX_ENABLELAZYCODECACHINGWIP=0
mkdir -p /tmp/fexrun /home/radxa/chrome-data; chmod 700 /tmp/fexrun
pgrep -x FEXServer >/dev/null || { /opt/fex/bin/FEXServer & disown; sleep 2; }

echo "# cache present: $(du -sh "$CACHE" 2>/dev/null | cut -f1)"
run_one() { # run_one <label> <diskcache>
  local label=$1 dc=$2 s e rc
  s=$(date +%s)
  timeout 900 env FEX_DISKCACHE="$dc" taskset -c 6,7 "$CHROME" \
    --headless --no-sandbox --no-zygote --disable-gpu --in-process-gpu \
    --disable-dev-shm-usage --ipc-connection-timeout=3600 \
    --no-first-run --disable-extensions --disable-background-networking --no-pings \
    --metrics-recording-only --disable-default-apps --disable-sync \
    --user-data-dir=/home/radxa/chrome-data \
    --dump-dom file:///tmp/heavy.html > "$OUTDIR/$label.dom" 2> "$OUTDIR/$label.err"
  rc=$?
  e=$(date +%s)
  printf '%-16s rc=%-3d wall=%4ds dombytes=%s\n' "$label" "$rc" "$((e-s))" "$(wc -c < "$OUTDIR/$label.dom" 2>/dev/null || echo 0)"
}

for rep in $(seq 1 "$REP"); do
  echo "--- rep $rep (interleaved, everything warm) ---"
  run_one "r${rep}-nocache" 0
  run_one "r${rep}-cache"   1
done
