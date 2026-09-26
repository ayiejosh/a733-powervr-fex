#!/bin/bash
# jit-cost-sizing.sh — is the JIT cost on this board compile time or run time?
#
# Why this first: "optimize the JIT" splits into two very different costs.
#
#   compile cost  - paid once per translated block. Shows up as launch delay and
#                   as in-game stutter. Attacked with the disk cache, block size,
#                   and by making the JIT compiler itself faster (LTO/march/PGO).
#   run cost      - paid on every block dispatch and every emulated instruction.
#                   Attacked by the lookup caches and by emitted-code quality.
#
# FEX's DiskCache (new in 2609, off by default) is a clean probe for the first
# one: with it disabled every launch recompiles from scratch; with it warm the
# same program skips compilation. The difference between the two IS the compile
# cost of whatever that program actually executed.
#
# A trivial `-c pass` exercises almost nothing, which is why the earlier 0.4 s
# measurement showed only ~10%. This uses a deliberate import-heavy start-up so
# a real amount of x86-64 gets translated.
set -u
FEX=/opt/fex/bin/FEXInterpreter
ROOTFS=/home/radxa/crd-rootfs
PY=$ROOTFS/usr/bin/python3
CACHE=${HOME}/.cache/fex-emu
CPU=${BENCH_CPU:-6}
HEAVY=(-c 'import json,re,argparse,logging,collections,dataclasses,typing,enum,functools,io,os,sys,textwrap,hashlib,base64,csv,datetime,decimal,fractions,random,statistics,traceback,warnings')

tm() { # tm <env...> -- <cmd...>   (env assignments first, then the command)
  local s e
  s=$(date +%s.%N)
  taskset -c "$CPU" "$@" >/dev/null 2>&1
  e=$(date +%s.%N)
  awk -v a="$s" -v b="$e" 'BEGIN{printf "%.3f", b-a}'
}

echo "# JIT cost sizing  guest=$PY  cpu=$CPU"
echo "## sanity: trivial start-up"
for i in 1 2; do
  echo "pass       run$i: $(tm env FEX_ROOTFS=$ROOTFS "$FEX" "$PY" -c pass) s"
done

echo "## import-heavy start-up, no disk cache (recompiles every launch)"
for i in 1 2 3; do
  echo "nocache    run$i: $(tm env FEX_ROOTFS=$ROOTFS "$FEX" "$PY" "${HEAVY[@]}") s"
done

echo "## same, with FEX_DISKCACHE=1 (run 1 builds the cache, 2-4 reuse it)"
rm -rf "$CACHE"
for i in 1 2 3 4; do
  printf 'cache      run%d: %s s  cache=%s\n' "$i" \
    "$(tm env FEX_ROOTFS=$ROOTFS FEX_DISKCACHE=1 "$FEX" "$PY" "${HEAVY[@]}")" \
    "$(du -sh "$CACHE" 2>/dev/null | cut -f1)"
done

echo "## does the cache also help steady-state? (repeat work inside one run)"
WORK=(-c 'import hashlib
h=hashlib.sha256()
for i in range(200000): h.update(str(i).encode())
print(h.hexdigest()[:16])')
for mode in 0 1; do
  echo "hash diskcache=$mode: $(tm env FEX_ROOTFS=$ROOTFS FEX_DISKCACHE=$mode "$FEX" "$PY" "${WORK[@]}") s"
done
rm -rf "$CACHE"
echo "# cache cleared again"
