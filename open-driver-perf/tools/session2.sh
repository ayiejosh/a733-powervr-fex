#!/bin/bash
# session2.sh: wait for the vendor NSI table, take vendor system probes, then ONE open-stack block (bsession) for the pc2 compiler experiments
# and the NSI table / system probe on the open stack. Progress markers "## step" in the log; "SESSION2 DONE" at the end; board is restored at the end.
cd ~/open-perf
T=pc2
NC="MESA_SHADER_CACHE_DISABLE=1"
OFF="PCO_NO_NMAX_FIX=1 PCO_NO_SHRINK_FIX=1 PCO_NO_PREFENCE_FIX=1"
FT="VK_ICD_FILENAMES=$HOME/open-perf/icd-$T.json PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 MESA_SHADER_CACHE_DISABLE=1"
until ! pgrep -f "[n]sitable.py vendor" >/dev/null; do sleep 3; done
echo "## vendor system probes $(date +%T)"
for sc in "loop:fragment-steps=5:fragment-uniform=false" "ideas:speed=duration"; do
  echo "--- vendor $sc"
  python3 tools/sysprobe.py --window 5:9 -- bash -c "DISPLAY=:0 taskset -c 6-7 glmark2-es2 --off-screen -b $sc:duration=11" 2>&1 | head -n 40
done
echo "## bsession start $(date +%T)"
tools/bsession.sh start 2>&1 | tail -n 3
[ "$(basename $(readlink /sys/bus/platform/devices/1800000.gpu/driver))" = powervr ] || { echo "NOT ON OPEN STACK: stop"; echo "SESSION2 DONE"; exit 1; }
echo "## foldtest $(date +%T)"; (cd foldtest; env $FT timeout 150 ./foldtest 2>&1 | tail -n 3)
echo "## nantest default"; (cd foldtest; env $FT timeout 60 ./nantest 2>&1 | tail -n 14)
echo "## nantest all fixes off"; (cd foldtest; env $FT $OFF timeout 60 ./nantest 2>&1 | tail -n 6)
echo "## compile stats (FS lines) default vs all fixes off"
for s in "jellyfish" "effect2d:kernel=0,1,0;1,-4,1;0,1,0;" "shading:shading=phong" "bump:bump-render=normals"; do
  n=${s%%:*}
  for arm in default off; do
    E="MESA_LOADER_DRIVER_OVERRIDE=zink PCO_DEBUG_PRINT=fs,stats $NC"; [ $arm = off ] && E="$E $OFF"
    CLIENT_ENV="$E" taskset -c 6-7 ./glbench.sh $PWD/icd-$T.json /tmp/dump/$T-$n-$arm.txt --off-screen -b "$s:duration=1" >/dev/null 2>&1
    echo "$n $arm: $(grep -E '^FS shader' /tmp/dump/$T-$n-$arm.txt | sed -e 's/ code size.*temp count/ code, temps/' -e 's/, 0 vtxin.*inst gr[a-z]* count/ , groups/' | cut -c1-90 | tr '\n' ';')"
  done
done
echo "## A/B1 all fixes: off -> on $(date +%T)"; tools/ab.sh $T "$OFF $NC" "$NC" 3 2>&1 | tail -n 14
echo "## A/B2 PCO_DITR_MODE=3 (no fence, wdf kept) $(date +%T)"; tools/ab.sh $T "$NC" "$NC PCO_DITR_MODE=3" 3 2>&1 | tail -n 14
echo "## A/B3 PCO_DITR_MODE=0 (unsafe upper bound) $(date +%T)"; tools/ab.sh $T "$NC" "$NC PCO_DITR_MODE=0" 3 2>&1 | tail -n 14
echo "## NSI table open $(date +%T)"
python3 tools/nsitable.py open 10 "CLIENT_ENV='MESA_LOADER_DRIVER_OVERRIDE=zink $NC' taskset -c 6-7 ./glbench.sh $PWD/icd-$T.json /tmp/o.txt --off-screen -b {S}" 2>&1 | tail -n 14
echo "## open system probes $(date +%T)"
for sc in "loop:fragment-steps=5:fragment-uniform=false" "ideas:speed=duration"; do
  echo "--- open $sc"
  python3 tools/sysprobe.py --window 11:15 -- bash -c "CLIENT_ENV='MESA_LOADER_DRIVER_OVERRIDE=zink $NC' taskset -c 6-7 ./glbench.sh $PWD/icd-$T.json /tmp/sp.txt --off-screen -b $sc:duration=17" 2>&1 | head -n 40
done
echo "## zink env arms $(date +%T)"
echo "-- MESA_NO_ERROR=1"; tools/ab.sh $T "$NC" "$NC MESA_NO_ERROR=1" 3 2>&1 | tail -n 14
echo "-- mesa_glthread=true"; tools/ab.sh $T "$NC" "$NC mesa_glthread=true" 3 2>&1 | tail -n 14
echo "## bsession stop $(date +%T)"
tools/bsession.sh stop 2>&1 | tail -n 3
echo "SESSION2 DONE $(date +%T)"
