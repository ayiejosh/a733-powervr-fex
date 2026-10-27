#!/bin/bash
# chain.sh TAG [GATELOG]: wait for the gate verdict, then foldtest, glmark2 --validate, a full off-screen glmark2 run
# pinned to cpu6-7, and a per-scene comparison with the vendor run and the 696bbd6 run. Prints CHAIN DONE at the end.
TAG=$1; LOG=${2:-/tmp/gate-$TAG.log}
until grep -qE "GATE (PASSED|FAILED)" $LOG 2>/dev/null; do sleep 5; done
grep -E "GATE|REAL" $LOG
grep -q "GATE PASSED" $LOG || { echo "gate failed: no benchmark"; echo "CHAIN DONE"; exit 1; }
cd ~/open-perf
echo "== foldtest"; (cd foldtest; VK_ICD_FILENAMES=$HOME/open-perf/icd-$TAG.json PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 120 ./foldtest 2>&1 | tail -n 3)
echo "== validate"; CLIENT_ENV="MESA_LOADER_DRIVER_OVERRIDE=zink" ./glvalidate.sh $PWD/icd-$TAG.json /tmp/val-$TAG.txt 2>&1 | tail -n 6
echo "== full run"; CLIENT_ENV="MESA_LOADER_DRIVER_OVERRIDE=zink" taskset -c 6-7 ./glbench.sh $PWD/icd-$TAG.json $HOME/open-perf/full-$TAG.txt --off-screen 2>&1 | tail -n 2
python3 tools/glmcmp.py glm-vendor-off-0123.txt full-cf5.txt full-$TAG.txt
echo "CHAIN DONE"
