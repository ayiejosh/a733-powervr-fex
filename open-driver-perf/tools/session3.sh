#!/bin/bash
# session3.sh: attribution of the pc2 compiler fixes, NSI table with FPS, thread-hop probe, glmark2 validate, then the CTS gate for pc2.
cd ~/open-perf
T=pc2; NC="MESA_SHADER_CACHE_DISABLE=1"
FT="VK_ICD_FILENAMES=$HOME/open-perf/icd-$T.json PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 MESA_SHADER_CACHE_DISABLE=1"
echo "## bsession start $(date +%T)"; tools/bsession.sh start 2>&1 | tail -n 2
[ "$(basename $(readlink /sys/bus/platform/devices/1800000.gpu/driver))" = powervr ] || { echo "NOT ON OPEN STACK"; echo "SESSION3 DONE"; exit 1; }
echo "## compile stats raw $(date +%T)"
for s in "jellyfish" "effect2d:kernel=0,1,0;1,-4,1;0,1,0;" "shading:shading=phong" "bump:bump-render=normals" "desktop:effect=shadow:windows=4" "shadow"; do
  n=${s%%:*}
  for arm in default off; do
    E="MESA_LOADER_DRIVER_OVERRIDE=zink PCO_DEBUG_PRINT=fs,stats $NC"; [ $arm = off ] && E="$E PCO_NO_NMAX_FIX=1 PCO_NO_SHRINK_FIX=1 PCO_NO_PREFENCE_FIX=1"
    CLIENT_ENV="$E" taskset -c 6-7 ./glbench.sh $PWD/icd-$T.json /tmp/dump/$T-$n-$arm.txt --off-screen -b "$s:duration=1" >/dev/null 2>&1
    echo "$n $arm: $(grep -E '^FS shader' /tmp/dump/$T-$n-$arm.txt | sed -E 's/FS shader: ([0-9]+) code size, [0-9]+ scratch size, ([0-9]+) spill count, ([0-9]+) temp count.*, ([0-9]+) inst group.*/code \1 spill \2 temps \3 groups \4/' | tr '\n' ';')"
  done
done
echo "## attribution: one fix off at a time vs all on $(date +%T)"
echo "-- PCO_NO_SHRINK_FIX=1 (default -> shrink fix off)"; tools/ab.sh $T "$NC" "$NC PCO_NO_SHRINK_FIX=1" 3 2>&1 | tail -n 13
echo "-- PCO_NO_NMAX_FIX=1"; tools/ab.sh $T "$NC" "$NC PCO_NO_NMAX_FIX=1" 3 2>&1 | tail -n 13
echo "-- PCO_NO_PREFENCE_FIX=1"; tools/ab.sh $T "$NC" "$NC PCO_NO_PREFENCE_FIX=1" 3 2>&1 | tail -n 13
echo "## NSI table open (with FPS) $(date +%T)"
NSI_OUT=/tmp/o.txt python3 tools/nsitable.py open 10 "CLIENT_ENV='MESA_LOADER_DRIVER_OVERRIDE=zink $NC' taskset -c 6-7 ./glbench.sh $PWD/icd-$T.json /tmp/o.txt --off-screen -b {S}" 2>&1 | tail -n 12
echo "## thread-hop probe loop: default vs GALLIUM_THREAD=0 $(date +%T)"
for thr in default nothread; do
  X="MESA_LOADER_DRIVER_OVERRIDE=zink $NC"; [ $thr = nothread ] && X="$X GALLIUM_THREAD=0"
  echo "--- loop $thr"
  SP_OUT=/tmp/sp.txt python3 tools/sysprobe.py --window 11:15 -- bash -c "CLIENT_ENV='$X' taskset -c 6-7 ./glbench.sh $PWD/icd-$T.json /tmp/sp.txt --off-screen -b loop:fragment-steps=5:fragment-uniform=false:duration=17" 2>&1 | grep -E "window|context|interrupts|pvr|zfq|gdrv|glmark2|WFI" | head -n 14
done
echo "## glmark2 --validate $T $(date +%T)"
CLIENT_ENV="MESA_LOADER_DRIVER_OVERRIDE=zink $NC" bash ./glvalidate.sh $PWD/icd-$T.json /tmp/val-$T.txt 2>&1 | tail -n 6
echo "## CTS gate $T $(date +%T)"
cd ~/open-perf/cts && python3 ~/open-perf/tools/gate.py --new $T --lists nullimg,robust_list,ubo_list,cwe2_list,dr_list,pco_list,caselist,caselist2,caselist3,loops_list,rp_extra,compute_list,cf2_list,cf_fast,tfbc2_list --workers 4 > /tmp/gate-$T.log 2>&1
grep -E "^CTS|GATE|all runs|REAL" /tmp/gate-$T.log | cut -c1-190
cd ~/open-perf
echo "## bsession stop $(date +%T)"; tools/bsession.sh stop 2>&1 | tail -n 2
echo "SESSION3 DONE $(date +%T)"
