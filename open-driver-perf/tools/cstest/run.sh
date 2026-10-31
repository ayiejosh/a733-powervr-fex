#!/bin/bash
# run.sh [ICD] [shaders...]: compare each compute shader on the PowerVR driver against lavapipe.
cd /home/radxa/open-perf/cstest; ICD=${ICD:-/home/radxa/open-perf/icd-pr12.json}
for s in ${@:-*.spv}; do
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./cstest $s /tmp/cs-ref.bin > /dev/null 2>&1
  PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 MESA_SHADER_CACHE_DISABLE=1 VK_ICD_FILENAMES=$ICD timeout 60 ./cstest $s /tmp/cs-got.bin > /tmp/cs-run.txt 2>&1 || { echo "$s: RUN FAILED"; tail -2 /tmp/cs-run.txt; continue; }
  case $s in b10_*|gen*) F=float;; *) F=;; esac
  echo "$s: $(python3 cmp.py /tmp/cs-ref.bin /tmp/cs-got.bin $F | head -4 | tr "\n" " ")"
done
