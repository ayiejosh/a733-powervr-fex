#!/bin/bash
# stocklane.sh - measure the board as it is WITHOUT any of the repo's overlays.
#
# The repo's first win was a device-tree overlay: pvrsrvkm reads a plain clk_rate
# off the GPU node and fell back to 600 MHz because Radxa never set one, and the
# DSU sat at the bootloader's 780 MHz. Both are settable at runtime through the
# clkctl debugfs module, so the "no repo" column can be measured in the same boot
# as the repo column instead of being quoted from history.
#
#   sudo bench/pvr-vulkan/stocklane.sh [gpu_hz] [dsu_hz]
#
# Leaves the clocks back at the repo's settings on exit, including on failure.
set -u
cd "$(dirname "$0")"

GPU=${1:-600000000}
DSU=${2:-780000000}
REPO_GPU=1104000000
REPO_DSU=1027000000
CLKCTL=/sys/kernel/debug/clkctl
VENDOR_ICD=/usr/share/vulkan/icd.d/img_icd.json

setclk() { echo "$2" | sudo tee "$CLKCTL/$1" >/dev/null; printf '%s=%s ' "$1" "$(sudo cat "$CLKCTL/$1")"; }

sudo modprobe clkctl 2>/dev/null || sudo insmod /home/radxa/trixie-prep/bench/clkctl/clkctl.ko
[ -e "$CLKCTL/gpu_clk" ] || { echo "clkctl unavailable - cannot measure the stock clock"; exit 2; }
restore() { echo; echo "--- restoring the repo clocks ---"; setclk gpu_clk $REPO_GPU; setclk dsu $REPO_DSU; echo; }
trap restore EXIT

echo "=== stock clocks (no overlay) ==="
setclk gpu_clk "$GPU"
setclk dsu "$DSU"
echo

echo "########## Vulkan, vendor driver, stock clocks ##########"
VK_ICD_FILENAMES=$VENDOR_ICD VK_DRIVER_FILES=$VENDOR_ICD ./stackbench.sh stock-600mhz

echo
echo "########## GL 1280x720, stock clocks ##########"
for L in 4 16 64 256; do
  case $L in 4) F=60 ;; 16) F=40 ;; 64) F=20 ;; *) F=8 ;; esac
  printf 'gl.vendor-native loop%-4s ' "$L"
  timeout 300 env -i PATH=/usr/bin:/bin HOME=/home/radxa /tmp/glbench none $L $F 2>&1 |
    grep -E "GL_RENDERER|RESULT" | tr '\n' ' '
  echo
done

echo
echo "--- clock actually held? ---"
printf 'gpu_clk=%s dsu=%s\n' "$(sudo cat $CLKCTL/gpu_clk)" "$(sudo cat $CLKCTL/dsu)"
