#!/bin/bash
# disk-cache-test.sh — does FEX-2609's DiskCache buy back JIT compile time?
#
# The microbenchmark cannot answer this: it measures steady-state throughput of
# already-compiled blocks. JIT compile cost is paid once per program launch, so
# it shows up as launch latency — the thing that makes a 30 s game load feel
# like 90 s and makes the first minute of play stutter.
#
# Method: launch a JIT-compile-heavy guest (python3 start-up compiles a lot of
# x86-64 before it prints anything) three times per mode, clearing the cache
# before the first launch so run 1 is genuinely cold and 2-3 are warm.
#
# usage: ./disk-cache-test.sh
set -u
FEX=/opt/fex/bin/FEXInterpreter
ROOTFS=/home/radxa/crd-rootfs
GUEST=/home/radxa/crd-rootfs/usr/bin/python3
CACHE=${HOME}/.cache/fex-emu
CPU=${BENCH_CPU:-6}

timeit() {
  local s e
  s=$(date +%s.%N)
  "$@" >/dev/null 2>&1
  e=$(date +%s.%N)
  awk -v a="$s" -v b="$e" 'BEGIN{printf "%.2f", b-a}'
}

echo "# disk cache: $CACHE"
for mode in 0 1; do
  for run in 1 2 3; do
    [ "$run" = 1 ] && rm -rf "$CACHE"
    secs=$(timeit taskset -c "$CPU" env FEX_ROOTFS="$ROOTFS" FEX_DISKCACHE="$mode" \
             "$FEX" "$GUEST" -c pass)
    size=$(du -sh "$CACHE" 2>/dev/null | cut -f1)
    echo "diskcache=$mode run=$run seconds=$secs cached=${size:-none}"
  done
done
