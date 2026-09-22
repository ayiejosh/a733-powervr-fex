#!/bin/bash
# clean-measure.sh — the two measurements that need an uncontended machine.
#
# Everything here runs SERIALLY inside one job on purpose. The first attempt at
# the pinning comparison was taken while a disk-cache test was running on the
# other cluster and produced the impossible result "A55 faster than A76 by 1.8x".
# On a box with syncthing, udev workers and a web harness in the background, a
# concurrent benchmark measures the neighbour, not the configuration.
#
# 1. Core selection: A55 (cpu0) vs A76 (cpu6) for single-threaded x86-64 code,
#    and pinned-vs-unpinned for the threaded tests.
# 2. FEX DiskCache: does the new-in-2609 JIT disk cache reduce launch cost?
#
# usage: ./clean-measure.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$HERE/build
FEX=/opt/fex/bin/FEXInterpreter
ROOTFS=/home/radxa/crd-rootfs
CACHE=${HOME}/.cache/fex-emu
ST=alu,branch,fp_double,memcpy
GUEST=/home/radxa/crd-rootfs/bin/bash
# A fixed guest workload: bash start-up (which JIT-compiles a lot) plus a loop.
WORK='i=0; while [ $i -lt 20000 ]; do i=$((i+1)); done'

csv() { grep '^CSV' | sed 's/^CSV [a-z0-9_]*//'; }
secs() { local s e; s=$(date +%s.%N); "$@" >/dev/null 2>&1; e=$(date +%s.%N)
         awk -v a="$s" -v b="$e" 'BEGIN{printf "%.3f", b-a}'; }

echo "### 1. core selection (single-threaded x86-64 under FEX)"
for rep in 1 2 3; do
  for cpu in 0 6; do
    line=$(taskset -c "$cpu" env FEX_ROOTFS=$ROOTFS "$FEX" "$BIN/fexbench_x64" "$ST" 2>/dev/null | csv)
    echo "st rep=$rep cpu=$cpu ($([ "$cpu" = 0 ] && echo A55 || echo A76)) $line"
  done
done

echo "### 2. threaded tests: unpinned vs pinned to 2 big cores"
for rep in 1 2 3; do
  for set in 0-7 6,7; do
    line=$(taskset -c "$set" env FEX_ROOTFS=$ROOTFS "$FEX" "$BIN/fexbench_x64" atomics,threads 2>/dev/null | csv)
    echo "mt rep=$rep cpuset=$set $line"
  done
done

echo "### 3. FEX DiskCache: cold vs warm launch of a fixed guest workload"
for rep in 1 2 3; do
  for mode in 0 1; do
    [ "$rep" = 1 ] && rm -rf "$CACHE"
    s=$(secs taskset -c 6 env FEX_ROOTFS=$ROOTFS FEX_DISKCACHE="$mode" "$FEX" "$GUEST" -c "$WORK")
    echo "cache rep=$rep diskcache=$mode seconds=$s size=$(du -sh "$CACHE" 2>/dev/null | cut -f1)"
  done
done
rm -rf "$CACHE"
echo "### done"
