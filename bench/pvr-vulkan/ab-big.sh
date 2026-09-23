#!/bin/bash
# ab-big.sh - is the large-render failure a regression from the BDA change?
#
# build/ was last linked at 14:57, before the buffer-device-address work, and
# build-x11/ at 16:59, after it. Same source tree, same compiler, same machine,
# one variable: whether the driver advertises buffer device addresses and carries
# the pack/unpack lowering fix. Both are run in one boot of the module, three
# times per size, so a flaky result shows up as a flaky result.
#
# Note: build/powervr_mesa_icd.aarch64.json points at an install path that does
# not exist on this board, so the devenv json is the one to use.
set -u
cd "$(dirname "$0")"

OLD=/home/radxa/mesa/mesa-main/build/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json
NEW=/home/radxa/mesa/mesa-main/build-x11/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json

run() {
  local label="$1" icd="$2"; shift 2
  local out rc
  out=$(VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
        timeout 300 "$@" 2>&1)
  rc=$?
  printf '  %-18s rc=%-3d %s\n' "$label" "$rc" \
    "$(printf '%s' "$out" | grep -E 'RESULT|FAIL:' | head -1 | cut -c1-64)"
}

sweep() {
  local icd="$1" tag="$2"
  echo "=== $tag ==="
  run "$tag 512" "$icd" ./vkrender 512 1
  for s in 4096 6144 8192; do
    for i in 1 2 3; do
      run "$tag ${s} #$i" "$icd" ./vkrender "$s" 1
    done
  done
  run "$tag bda" "$icd" ./bda
}

sweep "$OLD" "pre-change"
echo
sweep "$NEW" "post-change"
