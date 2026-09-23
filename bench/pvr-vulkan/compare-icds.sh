#!/bin/bash
# compare-icds.sh - run the same Vulkan compute test through every ICD on this
# board and print one table.
#
# Why: "does the GPU have Vulkan" and "is the GPU's Vulkan usable" are different
# questions, and after building a second ICD (Mesa's pvr) the only honest way to
# compare is the same binary, same workload, same machine, one row per driver.
#
#   vendor  /usr/lib/libVK_IMG.so      Imagination DDK 24.2, talks to pvrsrvkm
#   mesa    build/src/imagination/...  Mesa pvr, talks to pvrsrvkm (srv backend)
#                                     or to the mainline powervr driver (drm)
#
# Usage: ./compare-icds.sh [iters] [elements]
set -u
cd "$(dirname "$0")"
ITERS=${1:-20}
COUNT=${2:-1048576}

[ -x ./vktest ] || ./build.sh

MESA_ICD=$(ls -1 /home/radxa/mesa/mesa-25.0.7/build/src/imagination/vulkan/*.json 2>/dev/null | head -1)

run_one() {
  local label="$1" icd="$2"
  echo "=== $label ==="
  [ -n "$icd" ] && echo "icd: $icd"
  # PVR_I_WANT_A_BROKEN_VULKAN_DRIVER: Mesa's pvr driver refuses a BVNC that is
  # not on its conformance list, and 36.56.104.183 is not on it. Radxa ships the
  # same escape hatch in /usr/lib/environment.d/99-powervr-mesa.conf.
  if [ -n "$icd" ]; then
    VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" \
      PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
      timeout 300 ./vktest "$ITERS" "$COUNT" 2>&1 | sed 's/^/  /'
  else
    timeout 300 ./vktest "$ITERS" "$COUNT" 2>&1 | sed 's/^/  /'
  fi
  echo
}

run_one "vendor ICD (libVK_IMG.so -> pvrsrvkm)" "/usr/share/vulkan/icd.d/img_icd.json"
[ -n "$MESA_ICD" ] && run_one "Mesa pvr ICD ($MESA_ICD)" "$MESA_ICD" \
  || echo "=== Mesa pvr ICD: not built yet (mesa-25.0.7/build) ==="
