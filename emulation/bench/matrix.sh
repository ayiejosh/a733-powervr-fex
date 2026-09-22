#!/bin/bash
# matrix.sh — interleaved A/B benchmark driver for the x86-64 emulator stack.
#
# Runs every configuration once per repetition, round-robin. That matters more
# than repetition count on this board: syncthing, the harness and udev workers
# create background load, and a straight "run A x5 then B x5" schedule charges
# all the drift to whichever configuration ran last. Round-robin gives every
# configuration the same exposure, and the summariser takes the MINIMUM, which
# is the run least disturbed by a neighbour.
#
# Single-threaded tests are pinned to one Cortex-A76 (CPU 6); the threaded
# tests get a 4-core group (2x A55 + 2x A76) so they measure scaling instead
# of time-slicing one core.
#
# usage:            ./matrix.sh
#   REPS=5          repetitions per configuration (default 3)
#   ONLY=fex,box64  run a subset of configurations
#   OUT=file        results file (default ./results-<epoch>.csv)
#   TAG=suffix      appended to every label, to keep A/B campaigns separable
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$HERE/build
ROOTFS=/home/radxa/crd-rootfs
CPU_ST=${BENCH_CPU_ST:-6}
CPU_MT=${BENCH_CPU_MT:-4-7}
REPS=${REPS:-3}
ST=alu,branch,fp_double,fp_x87,memcpy,memloop,syscall
MT=atomics,threads
OUT=${OUT:-$HERE/results-$(date +%s).csv}
TAG=${TAG:-}

# label | guest binary | emulator prefix ('' = run the binary directly) | extra env
configs=(
  "native|$BIN/fexbench_arm64||"
  "fex|$BIN/fexbench_x64|/opt/fex/bin/FEXInterpreter|FEX_ROOTFS=$ROOTFS"
  "box64|$BIN/fexbench_x64|box64|"
)

ONLY=${ONLY:-}

# Sweep variants without editing this file:
#   EXTRA_CONFIGS='label|binary|prefix|ENV=1 ENV2=0;label2|...'
if [ -n "${EXTRA_CONFIGS:-}" ]; then
  IFS=';' read -r -a _extra <<< "$EXTRA_CONFIGS"
  for _e in "${_extra[@]}"; do
    [ -n "$_e" ] && configs+=("$_e")
  done
fi
wanted() {
  [ -z "$ONLY" ] && return 0
  case ",$ONLY," in *",$1,"*) return 0 ;; *) return 1 ;; esac
}

: > "$OUT"
echo "# fexbench matrix  host=$(uname -r)  reps=$REPS  st_cpu=$CPU_ST  mt_cpu=$CPU_MT" >> "$OUT"

run_one() { # run_one <label> <cpu> <tests> <binary> <prefix> <extra-env>
  local label=$1 cpu=$2 tests=$3 bin=$4 prefix=$5 extra=$6
  local line
  # shellcheck disable=SC2086
  line=$(taskset -c "$cpu" env $extra $prefix "$bin" "$tests" 2>/dev/null | grep '^CSV' || true)
  [ -z "$line" ] && line="CSV FAILED"
  printf 'r%s\t%s%s\t%s\n' "$REP" "$label" "$TAG" "$line" >> "$OUT"
}

for REP in $(seq 1 "$REPS"); do
  for entry in "${configs[@]}"; do
    IFS='|' read -r label bin prefix extra <<< "$entry"
    wanted "$label" || continue
    run_one "$label" "$CPU_ST" "$ST" "$bin" "$prefix" "$extra"
    run_one "$label-mt" "$CPU_MT" "$MT" "$bin" "$prefix" "$extra"
  done
done

echo "wrote $OUT"
