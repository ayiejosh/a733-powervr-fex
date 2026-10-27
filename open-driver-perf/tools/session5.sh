#!/bin/bash
# session5.sh: verify the clean pc4 build (three pco commits + probes off): NaN matrix on pc2 (old swap) vs pc4, foldtest, compile stats,
# validate, a full 33-scene glmark2 run vs vendor, the assert build checks (if built), and the final CTS gate for pc4.
cd ~/open-perf
T=pc4; NC="MESA_SHADER_CACHE_DISABLE=1"
ftenv() { echo "VK_ICD_FILENAMES=$HOME/open-perf/icd-$1.json PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 MESA_SHADER_CACHE_DISABLE=1"; }
echo "## bsession start $(date +%T)"; tools/bsession.sh start 2>&1 | tail -n 2
[ "$(basename $(readlink /sys/bus/platform/devices/1800000.gpu/driver))" = powervr ] || { echo "NOT ON OPEN STACK"; echo "SESSION5 DONE"; exit 1; }
echo "## nantest (vec store shares the constants): tx1 / pc2 / pc4"
for t in tx1 pc2 pc4; do echo "-- $t"; (cd foldtest; env $(ftenv $t) timeout 60 ./nantest 2>&1 | grep -E "FAIL|PASSED|FAILED" | head -n 8); done
echo "## foldtest pc4 $(date +%T)"; (cd foldtest; env $(ftenv $T) timeout 150 ./foldtest 2>&1 | tail -n 2)
echo "## compile stats pc4 default $(date +%T)"
for s in "effect2d:kernel=0,1,0;1,-4,1;0,1,0;" "shading:shading=phong" "bump:bump-render=normals" "jellyfish"; do
  n=${s%%:*}
  CLIENT_ENV="MESA_LOADER_DRIVER_OVERRIDE=zink PCO_DEBUG_PRINT=fs,stats $NC" taskset -c 6-7 ./glbench.sh $PWD/icd-$T.json /tmp/dump/$T-$n.txt --off-screen -b "$s:duration=1" >/dev/null 2>&1
  echo "$n: $(grep -E '^FS shader' /tmp/dump/$T-$n.txt | sed -E 's/FS shader: ([0-9]+) code size, [0-9]+ scratch size, ([0-9]+) spill count, ([0-9]+) temp count.*, ([0-9]+) inst group.*/code \1 spill \2 temps \3 groups \4/' | tr '\n' ';')"
done
echo "## glmark2 --validate $T $(date +%T)"; CLIENT_ENV="MESA_LOADER_DRIVER_OVERRIDE=zink $NC" bash ./glvalidate.sh $PWD/icd-$T.json /tmp/val-$T.txt 2>&1 | tail -n 5
echo "## full glmark2 run $T $(date +%T)"
CLIENT_ENV="MESA_LOADER_DRIVER_OVERRIDE=zink $NC" taskset -c 6-7 ./glbench.sh $PWD/icd-$T.json $HOME/open-perf/full-$T.txt --off-screen 2>&1 | tail -n 1
python3 tools/glmcmp.py glm-vendor-off-0123.txt full-tx1.txt full-$T.txt
AB=~/mesa/mesa-main/build-assert2/src/imagination/vulkan/libvulkan_powervr_mesa.so
if [ -f $AB ]; then
  echo "## assert build checks $(date +%T)"
  cp $AB ~/open-perf/libvulkan_powervr_mesa.pc4a.so; sed "s/tx1/pc4a/g" ~/open-perf/icd-tx1.json > ~/open-perf/icd-pc4a.json
  (cd foldtest; env $(ftenv pc4a) timeout 200 ./foldtest 2>&1 | tail -n 2; env $(ftenv pc4a) timeout 60 ./nantest 2>&1 | grep -E "FAIL|PASSED|FAILED" | head -n 5)
  CLIENT_ENV="MESA_LOADER_DRIVER_OVERRIDE=zink $NC" bash ./glvalidate.sh $PWD/icd-pc4a.json /tmp/val-pc4a.txt 2>&1 | tail -n 5
  echo "## CTS subset with asserts (pc4a) $(date +%T)"
  (cd cts && python3 ~/open-perf/tools/gate.py --new pc4a --lists caselist3,loops_list,cwe2_list,pco_list --workers 4 > /tmp/gate-pc4a.log 2>&1; grep -E "^CTS|GATE|all runs|REAL|Assert|assert" /tmp/gate-pc4a.log | cut -c1-190)
else echo "## assert build not available yet"; fi
echo "## CTS gate $T (final) $(date +%T)"
(cd cts && python3 ~/open-perf/tools/gate.py --new $T --lists nullimg,robust_list,ubo_list,cwe2_list,dr_list,pco_list,caselist,caselist2,caselist3,loops_list,rp_extra,compute_list,cf2_list,cf_fast,tfbc2_list --workers 4 > /tmp/gate-$T.log 2>&1; grep -E "^CTS|GATE|all runs|REAL" /tmp/gate-$T.log | cut -c1-190)
echo "## bsession stop $(date +%T)"; tools/bsession.sh stop 2>&1 | tail -n 2
echo "SESSION5 DONE $(date +%T)"
