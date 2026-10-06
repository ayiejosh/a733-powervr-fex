#!/bin/bash
# check-record.sh - does one offscreen frame still cost more to RECORD than it should?
#
# vkrender already proves the pixels are right; this proves the CPU side is not
# paying for a buffer allocation it should not be paying for. The number it
# guards is the one that moved when the end-of-tile program cache landed:
#
#   record = time inside vkBeginCommandBuffer..vkEndCommandBuffer
#
# The CSB (control stream) allocates a fresh 4 KiB GEM BO per render pass, which
# costs CREATE_BO + GET_BO_MMAP_OFFSET + mmap + heap_alloc + VM_MAP. A warmed
# 512x512 frame with the EOT cache sits at ~0.24-0.34 ms; with the control stream
# suballocated it should drop below the threshold below.
#
# Warmup is discarded: the first run after process start is dominated by
# first-frame costs and is 2-3x the steady state (measured).
#
#   ./check-record.sh [threshold_ms] [frames]
#
# ICD defaults to the open stack. Do NOT inherit VK_ICD_FILENAMES: this profile
# exports the vendor ICD into every shell, which silently measures the wrong
# driver. Override with ICD=<path> when the vendor stack is the one bound.
set -u
cd "$(dirname "$0")"

THRESHOLD=${1:-0.20}
FRAMES=${2:-60}
ICD=${ICD:-/home/radxa/pvr_gen_icd.json}
export PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1
export VK_ICD_FILENAMES="$ICD"
export VK_DRIVER_FILES="$ICD"

# Minimum, not median: this box shows 2-3x swings in record between identical
# runs depending on what else is running (syncthing indexing /home is the usual
# culprit). The question is "how cheap can a frame be recorded", so the fastest
# of N is the signal and the slow ones are scheduler noise.
minimum() { sort -g | head -1; }

# Discard the first run: it carries first-frame costs.
timeout 300 ./vkrender 512 "$FRAMES" >/dev/null 2>&1 || { echo "FAIL: warmup run errored"; exit 1; }

records=""
verdicts=""
for i in 1 2 3 4 5; do
   out=$(PVR_TIMING=1 timeout 300 ./vkrender 512 "$FRAMES" 2>&1)
   records="$records$(printf '%s' "$out" | grep -oE 'record=[0-9.]+' | head -1 | cut -d= -f2)"$'\n'
   verdicts="$verdicts$(printf '%s' "$out" | grep -oE 'VERDICT: [A-Z]+' | head -1 | awk '{print $2}')"$'\n'
done

best=$(printf '%s' "$records" | grep -v '^$' | minimum)
printf 'record ms (5 warmed runs): %s\n' "$(printf '%s' "$records" | grep -v '^$' | tr '\n' ' ')"
printf 'best: %s ms   threshold: %s ms\n' "$best" "$THRESHOLD"

# Correctness gates the performance claim: a fast wrong frame is not a result.
if printf '%s' "$verdicts" | grep -qv '^PASS$'; then
   echo "FAIL: a run did not verify (verdicts: $(printf '%s' "$verdicts" | tr '\n' ' '))"
   exit 1
fi
echo "ok    all runs verified their pixels"

if awk -v m="$best" -v t="$THRESHOLD" 'BEGIN{exit !(m < t)}'; then
   echo "PASS: best record ${best} ms is under ${THRESHOLD} ms"
   exit 0
fi
echo "FAIL: best record ${best} ms is not under ${THRESHOLD} ms"
exit 1
