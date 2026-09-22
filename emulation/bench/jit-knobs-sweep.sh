#!/bin/bash
# jit-knobs-sweep.sh — the codegen-affecting FEX knobs that were never swept,
# plus a proper re-measurement of DiskCache.
#
# From the local source (FEXCore/Source/Interface/Config/Config.json.in):
#   MaxInst                 default 5000, AffectsCodeGen TRUE  "max instructions per block"
#   EnableCodeCachingWIP    default false, AffectsCodeGen TRUE "code caching subsystem"
#   EnableLazyCodeCachingWIP default false
# The earlier sweep missed all three because they are compile-time-shaped knobs
# measured with a steady-state benchmark.
#
# Two metrics, because they answer different questions:
#   compile-bound : import-heavy python start-up with no disk cache (every launch
#                   recompiles) — this is what stutter and slow launches are made of
#   throughput    : fexbench alu/branch/fp_double — emitted-code quality
set -u
FEX=/opt/fex/bin/FEXInterpreter
ROOTFS=/home/radxa/crd-rootfs
PY=$ROOTFS/usr/bin/python3
CACHE=${HOME}/.cache/fex-emu
BIN=/home/radxa/Desktop/Projects/Radxa-A7A/fex-tuning/build
CPU=${BENCH_CPU:-6}
HEAVY=(-c 'import json,re,argparse,logging,collections,dataclasses,typing,enum,functools,io,os,sys,textwrap,hashlib,base64,csv,datetime,decimal,fractions,random,statistics,traceback,warnings')
REPS=${REPS:-2}

tm() { local s e; s=$(date +%s.%N); taskset -c "$CPU" "$@" >/dev/null 2>&1; e=$(date +%s.%N)
       awk -v a="$s" -v b="$e" 'BEGIN{printf "%.3f", b-a}'; }

# label | env overrides
variants=(
  "baseline|"
  "maxinst-1000|FEX_MAXINST=1000"
  "maxinst-20000|FEX_MAXINST=20000"
  "codecache-wip|FEX_ENABLECODECACHINGWIP=1"
  "codecache-lazy|FEX_ENABLECODECACHINGWIP=1 FEX_ENABLELAZYCODECACHINGWIP=1"
)

echo "# JIT knob sweep  cpu=$CPU reps=$REPS"
for rep in $(seq 1 "$REPS"); do
  echo "--- rep $rep : compile-bound (python import start-up, no disk cache) ---"
  for v in "${variants[@]}"; do
    IFS='|' read -r label extra <<< "$v"
    # shellcheck disable=SC2086
    printf '%-16s %s s\n' "$label" "$(tm env FEX_ROOTFS=$ROOTFS $extra "$FEX" "$PY" "${HEAVY[@]}")"
  done
  echo "--- rep $rep : throughput (fexbench alu,branch,fp_double) ---"
  for v in "${variants[@]}"; do
    IFS='|' read -r label extra <<< "$v"
    # shellcheck disable=SC2086
    printf '%-16s %s\n' "$label" "$(taskset -c "$CPU" env FEX_ROOTFS=$ROOTFS $extra "$FEX" "$BIN/fexbench_x64" alu,branch,fp_double 2>/dev/null | grep '^CSV' | sed 's/^CSV x86_64//')"
  done
done

echo "--- DiskCache, properly: build cache once, then alternate no-cache vs warm ---"
rm -rf "$CACHE"
tm env FEX_ROOTFS=$ROOTFS FEX_DISKCACHE=1 "$FEX" "$PY" "${HEAVY[@]}" >/dev/null
echo "cache built: $(du -sh "$CACHE" 2>/dev/null | cut -f1)"
for rep in 1 2 3; do
  printf 'rep%d  no-cache %s s   warm %s s\n' "$rep" \
    "$(tm env FEX_ROOTFS=$ROOTFS "$FEX" "$PY" "${HEAVY[@]}")" \
    "$(tm env FEX_ROOTFS=$ROOTFS FEX_DISKCACHE=1 "$FEX" "$PY" "${HEAVY[@]}")"
done
echo "--- same on the trivial workload that misled the first measurement ---"
for rep in 1 2; do
  printf 'rep%d  pass no-cache %s s   pass warm %s s\n' "$rep" \
    "$(tm env FEX_ROOTFS=$ROOTFS "$FEX" "$PY" -c pass)" \
    "$(tm env FEX_ROOTFS=$ROOTFS FEX_DISKCACHE=1 "$FEX" "$PY" -c pass)"
done
