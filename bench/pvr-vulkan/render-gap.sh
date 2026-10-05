#!/bin/bash
# render-gap.sh - why is one stack's render slower than the other's?
#
# vkrender's headline number is one frame = draw + full-surface image->buffer copy
# + fence wait, so it cannot say *where* a gap lives. vkrender already has the
# knobs; this script just turns them all in one pass:
#
#   PVR_TIMING=1    record / submit / gpu_wait - the CPU-vs-GPU split
#   MODE=           render | copy | empty - the draw, the copy, the pass setup
#   BATCH=          per-submit cost vs per-frame cost
#   AREA=           pixels covered, so fill-bound shows up as a scaling
#   LOADOP/STOREOP  whether the per-frame cost is a full-surface clear/store
#   size sweep      fixed overhead (intercept) vs per-pixel cost (slope)
#
#   VK_ICD_FILENAMES=<icd.json> ./render-gap.sh <label> [outfile]
set -u
cd "$(dirname "$0")"

LABEL=${1:-unknown}
OUT=${2:-/tmp/render-gap-$LABEL.txt}
export PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1
export VK_DRIVER_FILES="${VK_ICD_FILENAMES:-}"

line() { printf '%-30s %s\n' "$1" "$2"; }

# case_ <name> [VAR=VAL...] -- [vkrender args]
case_() {
  local name=$1; shift
  local envs=()
  while [ "$1" != "--" ]; do envs+=("$1"); shift; done
  shift
  local o ms pix v
  o=$(env "${envs[@]}" timeout 900 ./vkrender "$@" 2>&1)
  ms=$(printf '%s' "$o"  | grep -oE '[0-9.]+ ms/frame' | head -1)
  pix=$(printf '%s' "$o" | grep -oE '[0-9.]+ Mpix/s'   | head -1)
  v=$(printf '%s' "$o"   | grep -oE 'VERDICT: [A-Z]+' | head -1 | awk '{print $2}')
  line "$name" "${ms:-?}  ${pix:-?}  ${v:-n/a}"
  printf '%s' "$o" | grep -E 'timing per frame' | sed 's/^/      /'
}

{
  echo "########## render-gap: $LABEL ##########"
  echo "icd:  ${VK_ICD_FILENAMES:-<unset>}"
  echo "date: $(date -u +%Y-%m-%dT%H:%MZ)"
  echo
  echo "--- repeatability, 512x512 x60, three runs ---"
  case_ "both #1"  PVR_TIMING=1 -- 512 60
  case_ "both #2"  PVR_TIMING=1 -- 512 60
  case_ "both #3"  PVR_TIMING=1 -- 512 60

  echo
  echo "--- where the time goes, 512x512 x60 (MODE splits the frame) ---"
  case_ "empty pass (no draw/copy)" PVR_TIMING=1 MODE=empty -- 512 60
  case_ "render only (no copy)"     PVR_TIMING=1 MODE=render -- 512 60
  case_ "copy only (no render)"     PVR_TIMING=1 MODE=copy -- 512 60

  echo
  echo "--- is it fill, or full-surface work regardless of pixels? ---"
  case_ "area=quarter (512x512)"    PVR_TIMING=1 AREA=quarter -- 512 60
  case_ "area=half (512x512)"       PVR_TIMING=1 AREA=half -- 512 60
  case_ "no clear, no store"        PVR_TIMING=1 LOADOP=dontcare STOREOP=dontcare -- 512 60

  echo
  echo "--- per-submit overhead vs per-frame cost ---"
  case_ "BATCH=1 (60 submits)"      PVR_TIMING=1 BATCH=1 -- 512 60
  case_ "BATCH=60 (1 submit)"       PVR_TIMING=1 BATCH=60 -- 512 60

  echo
  echo "--- size sweep: fixed overhead (intercept) vs per-pixel (slope) ---"
  for s in 128 256 512 1024; do
    case_ "size $s" PVR_TIMING=1 -- "$s" 40
  done

  echo
  echo "########## done ##########"
} 2>&1 | tee "$OUT"
