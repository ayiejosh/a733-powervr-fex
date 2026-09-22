#!/bin/bash
# sweep-fex.sh — A/B the FEX configuration surface through FEX_* env overrides.
#
# Env overrides are used instead of editing ~/.fex-emu/Config.json because they
# are per-process: no shared state to restore if a run is interrupted, and the
# live configuration of the board is never at risk during a sweep.
#
# The list deliberately mixes CANDIDATE knobs with CONTROL knobs whose direction
# is already known (TSO on, multiblock off, full-precision x87 must all be
# slower). If a control fails to move, the apparatus is broken and no candidate
# result can be trusted — that check is what makes the rest of the table usable.
#
# usage: REPS=3 ./sweep-fex.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOTFS=/home/radxa/crd-rootfs
FEX=/opt/fex/bin/FEXInterpreter
REPS=${REPS:-3}
OUT=${OUT:-$HERE/results-fex-sweep.csv}

# label | env overrides applied on top of the live config
variants=(
  "tso1|FEX_TSOENABLED=1"                       # CONTROL: must be slower
  "mb0|FEX_MULTIBLOCK=0"                        # CONTROL: must be slower
  "x87full|FEX_X87REDUCEDPRECISION=0"           # CONTROL: fp_x87 must be slower
  "l1c-off|FEX_DYNAMICL1CACHE=0"
  "l1c-on|FEX_DYNAMICL1CACHE=1"
  "smc0|FEX_SMCCHECKS=0"
  "l2-off|FEX_DISABLEL2CACHE=1"
  "volmeta0|FEX_EXTENDEDVOLATILEMETADATA=0"
  "volmeta0b|FEX_VOLATILEMETADATA=0"
  "memtsoset0|FEX_MEMCPYSETTSOENABLED=0"
  "halfbarrier0|FEX_HALFBARRIERTSOENABLED=0"
  "vectortso0|FEX_VECTORTSOENABLED=0"
  "unalignedatomics1|FEX_KERNELUNALIGNEDATOMICBACKPATCHING=1"
  "hybrid-hidden|FEX_HIDEHYBRID=1"
  "indirect-call-off|FEX_DISABLE_VIXL_INDIRECT_RUNTIME_CALLS=1"
)

extra=""
only="fex"
for v in "${variants[@]}"; do
  label=${v%%|*}
  envs=${v#*|}
  extra+="fex-$label|$HERE/build/fexbench_x64|$FEX|FEX_ROOTFS=$ROOTFS $envs;"
  only+=",fex-$label"
done

REPS="$REPS" OUT="$OUT" ONLY="$only" EXTRA_CONFIGS="$extra" "$HERE/matrix.sh"
