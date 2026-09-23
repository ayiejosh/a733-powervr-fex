#!/bin/bash
# stackbench.sh - one comparable set of metric lines for any Vulkan ICD.
#
# The point is to be able to put three stacks side by side in one table:
#   * the vendor/default stack        (libVK_IMG, what the board ships with)
#   * the open driver before our work (mesa-main build/, linked before the BDA and
#     extent-limit changes)
#   * the open driver now             (build-x11 + the kernel module patches)
#
# Every line is "metric: value" so two runs can be diffed directly, and the
# compatibility cases report PASS/FAIL rather than being skipped, because "it did
# not work at all before" is a result the table has to be able to show.
#
#   VK_ICD_FILENAMES=<icd.json> ./stackbench.sh <label>
#
# Everything here is Vulkan-only. GL is measured separately (glbench, see
# glstack.sh) because it needs its own loader environment per stack.
set -u
cd "$(dirname "$0")"

LABEL=${1:-unknown}
ICD=${VK_ICD_FILENAMES:-<unset>}
export VK_ICD_FILENAMES VK_DRIVER_FILES="$ICD"
export PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1

num() { grep -oE "$1" | grep -oE '[0-9.]+' | head -1; }
# Numbers must come from the END of the match: "maxImageDimension2D 8192" would
# otherwise yield the 2 out of "Dimension2D".
tailnum() { grep -oE "$1" | grep -oE '[0-9]+$' | head -1; }
hexval() { grep -oE "$1" | grep -oE '0x[0-9a-f]+' | head -1; }
m() { printf '%-30s %s\n' "$1:" "$2"; }
# bda/pctest end with "PASS (0 failures)"; the other tools end with "VERDICT: PASS".
# stdin is buffered first: several greps cannot share one pipe.
verdict() {
  local out v; out=$(cat)
  v=$(printf '%s' "$out" | grep -oE 'VERDICT: [A-Z]+' | head -1 | awk '{print $2}')
  if [ -z "$v" ]; then
    if printf '%s' "$out" | grep -qE '^PASS \(0 failures\)'; then v=PASS
    elif printf '%s' "$out" | grep -qE '\([0-9]+ failures\)'; then v=FAIL
    else v=NOOUTPUT
    fi
  fi
  printf '%s' "$v"
}

echo "stack: $LABEL"
echo "icd:   $ICD"
echo "date:  $(date -u +%Y-%m-%dT%H:%MZ)"

# --- advertised capability ----------------------------------------------------
AUD=$(timeout 120 ./vkaudit 2>/dev/null)
m icd.apiVersion "$(printf '%s' "$AUD" | num 'apiVersion[ ]+[0-9.]+' | head -1)"
m icd.maxImageDimension2D "$(printf '%s' "$AUD" | tailnum 'maxImageDimension2D[ \t]+[0-9]+')"
m icd.bufferDeviceAddress "$(printf '%s' "$AUD" | grep -oE 'vk12.bufferDeviceAddress=[01]' | head -1 | cut -d= -f2)"
m icd.colorSampleCounts "$(printf '%s' "$AUD" | hexval 'framebufferColorSampleCounts[ \t]+0x[0-9a-f]+')"

# --- compatibility: the things that used to not work --------------------------
printf 'compat.bda: %s\n' "$(timeout 300 ./bda 2>&1 | verdict)"
printf 'compat.pctest: %s\n' "$(timeout 300 ./pctest 2>&1 | verdict)"

# --- compute ----------------------------------------------------------------
VK=$(timeout 600 ./vktest 5 262144 2>&1)
m compute.ms_dispatch "$(printf '%s' "$VK" | num '[0-9.]+ ms/dispatch')"
m compute.GBps "$(printf '%s' "$VK" | num '[0-9.]+ GB/s')"
printf 'compat.compute: %s\n' "$(printf '%s' "$VK" | verdict)"

# --- render at increasing sizes ---------------------------------------------
# 512/1024 with enough frames to be stable, 2048..8192 with fewer because the
# point there is "does it work at all", and the pass/fail is the metric.
for spec in "512 20" "1024 8" "2048 4"; do
  set -- $spec
  OUT=$(timeout 900 ./vkrender "$1" "$2" 2>&1)
  m "render$1.ms_frame" "$(printf '%s' "$OUT" | num '[0-9.]+ ms/frame')"
  m "render$1.Mpix_s" "$(printf '%s' "$OUT" | num '[0-9.]+ Mpix/s')"
  printf 'compat.render%s: %s\n' "$1" "$(printf '%s' "$OUT" | verdict)"
done
for s in 4096 8192; do
  printf 'compat.render%s: %s\n' "$s" "$(timeout 900 ./vkrender "$s" 1 2>&1 | verdict)"
done

# --- memory / readback --------------------------------------------------------
MT=$(timeout 300 ./memtypes 2>&1)
m readback.total_ms "$(printf '%s' "$MT" | grep -A99 'readback' | grep 'type 2:' | num 'total[ ]+[0-9.]+ ms')"
m readback.cpu_ms "$(printf '%s' "$MT" | grep -A99 'readback' | grep 'type 2:' | num 'cpu read[ ]+[0-9.]+')"
m alloc.ms "$(printf '%s' "$MT" | grep 'type 2:' | num 'allocate\+bind[ ]+[0-9.]+ ms')"
