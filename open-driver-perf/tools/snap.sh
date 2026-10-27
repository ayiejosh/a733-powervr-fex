#!/bin/bash
# snap.sh TAG: build the pvr ICD and snapshot it as ~/open-perf/libvulkan_powervr_mesa.TAG.so + icd-TAG.json
TAG=$1; cd ~/mesa/mesa-main || exit 1
ninja -C build src/imagination/vulkan/libvulkan_powervr_mesa.so > /tmp/snap-ninja.log 2>&1 || { tail -n 15 /tmp/snap-ninja.log; echo BUILD FAILED; exit 1; }
tail -n 2 /tmp/snap-ninja.log
cp build/src/imagination/vulkan/libvulkan_powervr_mesa.so ~/open-perf/libvulkan_powervr_mesa.$TAG.so
sed "s/tx1/$TAG/g" ~/open-perf/icd-tx1.json > ~/open-perf/icd-$TAG.json
cat ~/open-perf/icd-$TAG.json | head -n 3; ls -la ~/open-perf/libvulkan_powervr_mesa.$TAG.so | cut -c1-80
