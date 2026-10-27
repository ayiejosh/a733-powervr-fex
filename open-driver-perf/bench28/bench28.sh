#!/bin/bash
# bench28.sh: glmark2-es2 off-screen, native GL vs zink (open stack) vs vendor (bare X), performance
# governor with system.slice off cores 6-7, two runs each; restores schedutil, AllowedCPUs, vendor + desktop.
OP=/home/radxa/open-perf; ICD=/tmp/icd-build.json; R=$OP/bench28-results; mkdir -p $R
SW=/home/radxa/Desktop/Projects/Radxa-A7A/open-driver-perf/tools/verify/switch-safe.sh
drv() { basename "$(readlink /sys/bus/platform/devices/1800000.gpu/driver)"; }
score() { grep -o 'glmark2 Score: [0-9]*' "$1" | grep -o '[0-9]*$'; }
gov() { for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo $1 | sudo tee $f >/dev/null; done; }
echo "$(date) bench28 start" >> $OP/session.log
sudo systemctl set-property --runtime system.slice AllowedCPUs=0-5; gov performance
echo "## open stack $(date +%T)"; sh $SW open > $R/sw-open.log 2>&1; sleep 3; echo "  bound=$(drv)"
cd $OP
for i in 1 2; do CLIENT_ENV="MESA_LOADER_DRIVER_OVERRIDE=pvr" taskset -c 6-7 ./glbench.sh $ICD $R/native-$i.txt --off-screen > /dev/null 2>&1; echo "  native $i: $(score $R/native-$i.txt) $(date +%T)"; done
for i in 1 2; do CLIENT_ENV="MESA_LOADER_DRIVER_OVERRIDE=zink MESA_SHADER_CACHE_DISABLE=1" taskset -c 6-7 ./glbench.sh $ICD $R/zink-$i.txt --off-screen > /dev/null 2>&1; echo "  zink $i: $(score $R/zink-$i.txt) $(date +%T)"; done
echo "## vendor, bare X $(date +%T)"; sh $SW vendor > $R/sw-vendor.log 2>&1; sleep 5; echo "  bound=$(drv)"
sudo systemctl stop display-manager; sleep 3
for p in kwin_x11 plasmashell startplasma-x11 Xorg; do for q in $(pgrep -x $p); do sudo kill $q; done; done; sleep 3
sudo Xorg :1 -ac -nolisten tcp vt8 > $R/xorg1.log 2>&1 & sleep 6
for i in 1 2; do sudo env DISPLAY=:1 taskset -c 6-7 timeout 900 glmark2-es2 --off-screen > $R/vendor-$i.txt 2>&1; echo "  vendor $i: $(score $R/vendor-$i.txt) $(date +%T)"; done
for q in $(pgrep -x Xorg); do sudo kill $q; done; sleep 3
sudo systemctl set-property --runtime system.slice AllowedCPUs=; gov schedutil
sudo systemctl start display-manager; sleep 8
echo "## restored bound=$(drv) dm=$(systemctl is-active display-manager) gov=$(cat /sys/devices/system/cpu/cpu6/cpufreq/scaling_governor) faults=$(sudo dmesg | grep -ciE 'mmu fault|page fault')"
echo "$(date) bench28 done" >> $OP/session.log
echo BENCH28 DONE
