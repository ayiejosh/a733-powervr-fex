#!/usr/bin/env bash
# Canonical A733 benchmark runner — emits parseable "metric: value" lines so runs
# are directly comparable. WORKFLOW: run before a change AND after, then
#   diff <(git show HEAD:bench/baseline.txt) <(bench/run.sh)
# to see the delta. Put that diff in the commit message; update baseline.txt only
# when a change is verified to improve (or intentionally move) a number.
#
#   bench/run.sh            # software-affected suite (GPU + FEX); ~2-3 min, A76-pinned
#   bench/run.sh --hw       # also re-check CPU/RAM/UFS hardware baselines (slower; UFS writes)
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; cd "$HERE"
PIN="taskset -c 6,7"
FEXI="${FEXI:-/opt/fex/bin/FEXInterpreter}"
STOCK="$(ls /opt/fex/bin/FEXInterpreter.orig-* /opt/fex/bin/FEXInterpreter.prebuilt-* 2>/dev/null | head -1)"
LD=/usr/local/lib
num(){ grep -oE "$1" 2>/dev/null | grep -oE '[0-9.]+' | head -1; }
m(){ printf '%-34s %s\n' "$1:" "${2:-NA}"; }
# best-of-3 (max) for a volatile numeric metric: bestmax 'cmd...'
bestmax(){ local b=0 v; for i in 1 2 3; do v=$(eval "$1"); awk "BEGIN{exit !($v>$b)}" && b=$v; done; echo "$b"; }
bestmin(){ local b=0 v; for i in 1 2 3; do v=$(eval "$1"); [ "$b" = 0 ] && b=$v; awk "BEGIN{exit !($v<$b)}" && b=$v; done; echo "$b"; }

# build microbenches (x86 + native where applicable)
x86_64-linux-gnu-gcc -O2 -static          uatomic.c -o /tmp/uatomic_x86  2>/dev/null
x86_64-linux-gnu-gcc -O2 -static -pthread tcreate.c -o /tmp/tcreate_x86  2>/dev/null
gcc                  -O2 -pthread         tcreate.c -o /tmp/tcreate_arm  2>/dev/null
x86_64-linux-gnu-gcc -O2 -static          x87l.c    -o /tmp/x87l_x86     2>/dev/null
gcc glbench.c -o /tmp/glbench -lEGL -lGLESv2 -lgbm -ldl 2>/dev/null

echo "# a733-bench  kernel=$(uname -r)  fex=$(basename "$FEXI")  date=$(date -u +%Y-%m-%dT%H:%MZ)"

# --- CPU sanity (1-thread is stable; full re-check needs --hw) ---
command -v sysbench >/dev/null && \
  m cpu.1thread.evps "$(bestmax "$PIN sysbench cpu --cpu-max-prime=20000 --threads=1 --time=6 run 2>/dev/null | num 'events per second:[ ]*[0-9.]+'")"

# --- GPU (PowerVR GLES throughput) ---
for L in 4 16 64 256; do
  m gpu.glbench.loop$L.Mpix "$(LD_LIBRARY_PATH=$LD timeout 90 /tmp/glbench /dev/dri/renderD128 $L 300 2>/dev/null | num '[0-9.]+ ?Mpix')"
done

# --- FEX: unaligned atomic (off0 aligned / off2 unaligned / off14 split-lock), patched + stock ---
A0=$($PIN "$FEXI" /tmp/uatomic_x86 30000000 0 2>/dev/null | num '[0-9.]+ Mops'); m fex.atomic.off0.Mops "$A0"
A2=$($PIN "$FEXI" /tmp/uatomic_x86 30000000 2 2>/dev/null | num '[0-9.]+ Mops'); m fex.atomic.off2.Mops "$A2"
m fex.atomic.off14.Mops "$($PIN "$FEXI" /tmp/uatomic_x86 30000000 14 2>/dev/null | num '[0-9.]+ Mops')"
AS=$([ -n "$STOCK" ] && timeout 120 $PIN "$STOCK" /tmp/uatomic_x86 3000000 2 2>/dev/null | num '[0-9.]+ Mops'); m fex.atomic.off2.STOCK.Mops "${AS:-NA}"

# --- FEX: thread create+join (native + patched + stock) ---
m native.tcreate.nsop "$(bestmin "$PIN /tmp/tcreate_arm 10000 2>/dev/null | num '[0-9]+ ns/op'")"
FT=$(bestmin "$PIN \"$FEXI\" /tmp/tcreate_x86 10000 2>/dev/null | num '[0-9]+ ns/op'"); m fex.tcreate.nsop "$FT"
ST=$([ -n "$STOCK" ] && timeout 120 $PIN "$STOCK" /tmp/tcreate_x86 10000 2>/dev/null | num '[0-9]+ ns/op'); m fex.tcreate.STOCK.nsop "${ST:-NA}"

# --- FEX: x87 ReducedPrecision (fldl/faddl), config-file toggle in a clean HOME ---
mkdir -p /tmp/fexbenchHOME/.fex-emu
for r in 0 1; do
  printf '{"Config":{"X87ReducedPrecision":"%s"}}' "$r" > /tmp/fexbenchHOME/.fex-emu/Config.json
  V=$(timeout 120 env HOME=/tmp/fexbenchHOME $PIN "$FEXI" /tmp/x87l_x86 20000000 2>/dev/null | num '[0-9.]+ ns/iter'); eval "X$r=$V"; m fex.x87.RP$r.nsiter "$V"
done

# --- optional hardware baselines ---
if [ "${1:-}" = "--hw" ]; then
  command -v sysbench >/dev/null && m cpu.8thread.evps "$(sysbench cpu --cpu-max-prime=20000 --threads=8 --time=8 run 2>/dev/null | num 'events per second:[ ]*[0-9.]+')"
  command -v sysbench >/dev/null && m ram.8thread.read.MiBs "$(sysbench memory --memory-block-size=1M --memory-total-size=20G --threads=8 --memory-oper=read run 2>/dev/null | num '[0-9.]+ MiB/sec')"
  if command -v fio >/dev/null; then
    dd if=/dev/zero of=/tmp/fiob bs=1M count=2048 conv=fsync 2>/dev/null
    m ufs.seqread.QD8.MBs "$(fio --name=r --rw=read --bs=1M --direct=1 --iodepth=8 --ioengine=libaio --size=2G --filename=/tmp/fiob 2>/dev/null | num 'READ: bw=[0-9.]+MiB/s \([0-9.]+MB')"
    m ufs.seqwrite.MBs "$(fio --name=w --rw=write --bs=1M --direct=1 --size=2G --filename=/tmp/fiob 2>/dev/null | num 'WRITE: bw=[0-9.]+MiB/s \([0-9.]+MB')"
    rm -f /tmp/fiob
  fi
fi

# ===== RATIOS — the reliable change-signals (A/B run back-to-back, load-independent) =====
r(){ awk "BEGIN{if($2>0)printf \"%.1f\",$1/$2; else print \"NA\"}"; }
echo "# --- ratios (compare THESE across commits; absolutes above are load-sensitive ±20%) ---"
[ -n "${AS:-}" ] && m ratio.atomic.patch_speedup "$(r "${A2:-0}" "${AS:-0}")x   # THE WIN: unaligned op, patched vs stock (same op)"
[ -n "${AS:-}" ] && m info.atomic.stock_penalty_vs_aligned "$(r "${A0:-0}" "${AS:-0}")x   # info: stock unaligned vs aligned"
m info.atomic.patched_penalty_vs_aligned "$(r "${A0:-0}" "${A2:-0}")x   # info: residual unaligned vs aligned (patched)"
[ -n "${ST:-}" ] && m ratio.tcreate.stock_over_patched "$(r "${ST:-0}" "${FT:-0}")x   # thread-pooling patch win"
m ratio.x87.RP0_over_RP1 "$(r "${X0:-0}" "${X1:-0}")x   # X87ReducedPrecision win"
