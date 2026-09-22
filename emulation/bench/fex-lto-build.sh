#!/bin/bash
# fex-lto-build.sh — build FEX with LTO into a SIDE directory and benchmark it
# against the installed binary. Nothing here installs anything.
#
# Why LTO is the only build lever left: the installed FEX already carries
# -mcpu=cortex-a76 (verified in build/CMakeFiles/*/flags.make — CMakeCache is NOT
# where those flags live, and an earlier check of mine looked there and wrongly
# concluded the build was generic ARMv8). What differs from the source's own
# default is ENABLE_LTO: upstream defaults it TRUE, this build has it FALSE.
#
# The A/B runs with FEX_DISKCACHE=0 for both binaries on purpose: the disk cache
# would hide exactly the difference being measured (compile speed), and both
# binaries would otherwise share one cache directory.
set -u
SRC=/home/radxa/FEX-2609
BUILD=$SRC/build-lto
INSTALLED=/opt/fex/bin/FEX
OUT=/home/radxa/fex-tune
LOG=$OUT/fex-lto-build.log
JOBS=${JOBS:-6}
ROOTFS=/home/radxa/crd-rootfs
PY=$ROOTFS/usr/bin/python3
BENCH=/home/radxa/Desktop/Projects/Radxa-A7A/fex-tuning/build/fexbench_x64
HEAVY=(-c 'import json,re,argparse,logging,collections,dataclasses,typing,enum,functools,io,os,sys,textwrap,hashlib,base64,csv,datetime,decimal,fractions,random,statistics,traceback,warnings')

mkdir -p "$OUT"
exec > >(tee "$LOG") 2>&1

echo "=== configure $(date -Is) ==="
# FEX requires clang ("FEX doesn't support GCC"), and a fresh build dir has no
# cached compiler, so it must be named explicitly or cmake picks cc/c++ = gcc.
rm -rf "$BUILD"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=ON -DTUNE_CPU=native -DENABLE_CCACHE=ON \
  -DBUILD_TESTS=OFF -DBUILD_THUNKS=ON -DENABLE_GDB_SYMBOLS=ON -DENABLE_ASSERTIONS=OFF \
  || { echo "CONFIGURE FAILED"; exit 1; }

echo "=== build -j$JOBS started $(date -Is) ==="
cmake --build "$BUILD" -j"$JOBS" || { echo "BUILD FAILED"; exit 1; }
echo "=== build finished $(date -Is) ==="

NEW=$(find "$BUILD" -maxdepth 3 -type f -name FEX | head -1)
echo "built binary: ${NEW:-NOT FOUND}"
[ -z "$NEW" ] && { echo "no binary, stopping"; exit 1; }
echo "--- LTO present in the new build's flags? ---"
grep -rhoE '\-flto[^ ]*' "$BUILD"/CMakeFiles/*/flags.make 2>/dev/null | sort -u | head -3
echo "--- same check on the installed build (expect nothing) ---"
grep -rhoE '\-flto[^ ]*' "$SRC"/build/CMakeFiles/*/flags.make 2>/dev/null | sort -u | head -3

tm() { local s e; s=$(date +%s.%N); taskset -c 6 "$@" >/dev/null 2>&1; e=$(date +%s.%N)
       awk -v a="$s" -v b="$e" 'BEGIN{printf "%.3f", b-a}'; }

echo
echo "=== A/B: compile-bound (python import start-up, DiskCache forced off) ==="
for rep in 1 2 3; do
  printf 'rep%d  installed %s s   lto %s s\n' "$rep" \
    "$(tm env FEX_ROOTFS=$ROOTFS FEX_DISKCACHE=0 "$INSTALLED" "$PY" "${HEAVY[@]}")" \
    "$(tm env FEX_ROOTFS=$ROOTFS FEX_DISKCACHE=0 "$NEW" "$PY" "${HEAVY[@]}")"
done

echo "=== A/B: throughput + checksums (fexbench alu,branch,fp_double) ==="
echo "installed: $(taskset -c 6 env FEX_ROOTFS=$ROOTFS FEX_DISKCACHE=0 "$INSTALLED" "$BENCH" alu,branch,fp_double 2>/dev/null | grep '^CSV')"
echo "lto      : $(taskset -c 6 env FEX_ROOTFS=$ROOTFS FEX_DISKCACHE=0 "$NEW" "$BENCH" alu,branch,fp_double 2>/dev/null | grep '^CSV')"

echo "=== done with the LTO build + A/B $(date -Is) ==="

# ---------------------------------------------------------------------------
# Second build: the attribution build. FEXCore's gpuvis backend writes its
# events to ftrace's trace_marker (a plain text ring buffer, read back from
# /sys/kernel/tracing/trace), so no GUI viewer is needed to read the result --
# which is why this is worth building at all.
#
# It is instrumented, so its TIMINGS are not comparable with anything above.
# The clean LTO build is the one that answers "is it faster".
# ---------------------------------------------------------------------------
echo
echo "=== profiler (instrumented) build $(date -Is) ==="
PBUILD=$SRC/build-lto-prof
rm -rf "$PBUILD"
cmake -S "$SRC" -B "$PBUILD" -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=ON -DTUNE_CPU=native -DENABLE_CCACHE=ON \
  -DBUILD_TESTS=OFF -DBUILD_THUNKS=ON -DENABLE_ASSERTIONS=OFF \
  -DENABLE_FEXCORE_PROFILER=ON -DFEXCORE_PROFILER_BACKEND=gpuvis \
  || { echo "PROFILER CONFIGURE FAILED (the LTO result above still stands)"; exit 0; }
cmake --build "$PBUILD" -j"$JOBS" || { echo "PROFILER BUILD FAILED (the LTO result above still stands)"; exit 0; }
PROF=$(find "$PBUILD" -maxdepth 3 -type f -name FEX | head -1)
echo "profiler binary: ${PROF:-NOT FOUND}"
echo "capture: tracing_on=1, run guest with FEX_ENABLEGPUVISPROFILING=1, then read"
echo "         /sys/kernel/tracing/trace   (trace_marker is the write side)"

echo "=== all done $(date -Is) ==="
