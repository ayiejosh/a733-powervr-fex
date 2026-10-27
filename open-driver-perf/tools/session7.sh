#!/bin/bash
# session7.sh: occupancy probe (N live registers, constant work) on the open stack (pc4) and on the vendor Vulkan driver.
cd ~/open-perf/foldtest
NS="8 12 16 20 24 28 32 40"
echo "## bsession start $(date +%T)"; ~/open-perf/tools/bsession.sh start 2>&1 | tail -n 2
[ "$(basename $(readlink /sys/bus/platform/devices/1800000.gpu/driver))" = powervr ] || { echo "NOT ON OPEN STACK"; echo "SESSION7 DONE"; exit 1; }
E="VK_ICD_FILENAMES=$HOME/open-perf/icd-pc4.json PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 MESA_SHADER_CACHE_DISABLE=1"
echo "## open: temps per N"; for n in $NS; do echo "N=$n $(env $E PCO_DEBUG_PRINT=fs,stats timeout 60 ./occ $n 2>&1 | grep -E '^FS shader' | sed -E 's/FS shader: ([0-9]+) code size.*([0-9]+) temp count.*, ([0-9]+) inst group.*/code \1 temps \2 groups \3/' | head -n 1)"; done
echo "## open: ms/Mpx $(date +%T)"; env $E taskset -c 6-7 timeout 300 ./occ $NS 2>&1 | tail -n 10
echo "## open (second run)"; env $E taskset -c 6-7 timeout 300 ./occ $NS 2>&1 | tail -n 10
echo "## bsession stop $(date +%T)"; ~/open-perf/tools/bsession.sh stop 2>&1 | tail -n 2
echo "## vendor Vulkan: ms/Mpx $(date +%T)"; env VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json taskset -c 6-7 timeout 300 ./occ $NS 2>&1 | tail -n 10
echo "## vendor (second run)"; env VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json taskset -c 6-7 timeout 300 ./occ $NS 2>&1 | tail -n 10
echo "SESSION7 DONE $(date +%T)"
