#!/bin/bash
# bench31.sh: glmark2 with the SAME x11-glesv2 (EGL/X11) binary on all three stacks.
# Native/zink run under Xwayland on the open stack; vendor runs under bare Xorg on pvrsrvkm.
# Stack identity is verified from the glmark2 log itself:
#   native  GL_RENDERER must contain "(36.56.104.183)" (PowerVR native, Mesa)
#   zink    GL_RENDERER must contain "zink"
#   vendor  GL_VERSION must contain "build 24.2" (IMG GLES); vendor logs contain no "Mesa"
# Every exit (including stack errors) restores the board through the EXIT trap.
OP=/home/radxa/open-perf; R=$OP/bench31-results; mkdir -p $R
SW=/home/radxa/Desktop/Projects/Radxa-A7A/open-driver-perf/tools/verify/switch-safe.sh
GLM=$OP/glm2x11/bin/glmark2-es2; ICD=$OP/icd-build.json
# --data-path, not the compiled-in /tmp/glm2egl: /tmp is cleaned periodically on this box and the
# missing symlink has silently produced invalid runs before.
DATA=$OP/glm2x11/share/glmark2
drv() { basename "$(readlink /sys/bus/platform/devices/1800000.gpu/driver)"; }
score() { grep -o 'glmark2 Score: [0-9]*' "$1" | grep -o '[0-9]*$'; }
gov() { for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo $1 | sudo tee $f >/dev/null; done; }
WP=""
restore() {
   rc=$?
   [ -n "$WP" ] && kill $WP 2>/dev/null
   for q in $(pgrep -x Xorg); do sudo kill $q 2>/dev/null; done
   sleep 2
   sh $SW vendor > $R/sw-restore.log 2>&1
   sleep 5
   sudo systemctl set-property --runtime system.slice AllowedCPUs= >/dev/null 2>&1
   gov schedutil
   sudo systemctl start display-manager; sleep 8
   # The first start after the benchmark's Plasma kill occasionally leaves no session (kwin
   # missing); one restart brings it up reliably.
   sleep 4
   pgrep -x kwin_x11 >/dev/null || { sudo systemctl restart display-manager; sleep 12; }
   echo "## restored bound=$(drv) dm=$(systemctl is-active display-manager) gov=$(cat /sys/devices/system/cpu/cpu6/cpufreq/scaling_governor) faults=$(sudo dmesg | grep -ciE 'mmu fault|page fault')"
   echo "$(date) bench31 done" >> $OP/session.log
   exit $rc
}
trap restore EXIT
echo "$(date) bench31 start" >> $OP/session.log
sudo systemctl set-property --runtime system.slice AllowedCPUs=0-5; gov performance
echo "## open stack $(date +%T)"; sh $SW open > $R/sw-open.log 2>&1; sleep 3; echo "  bound=$(drv)"
. /home/radxa/gpu-open-stack/env26.sh 2>/dev/null
rm -rf /tmp/wroot; mkdir -p /tmp/wroot; chmod 700 /tmp/wroot; export XDG_RUNTIME_DIR=/tmp/wroot
printf "[core]\nrequire-input=false\n" > /tmp/wsmoke.ini
weston --config=/tmp/wsmoke.ini --backend=headless-backend.so --renderer=gl --xwayland --socket=wayland-open --idle-time=0 --log=$R/weston.log >/dev/null 2>&1 & WP=$!; sleep 4
for i in 1 2; do
   DISPLAY=:0 MESA_LOADER_DRIVER_OVERRIDE=pvr taskset -c 6-7 timeout 900 $GLM --off-screen --data-path $DATA > $R/native-$i.txt 2>&1
   grep -q "(36.56.104.183)" $R/native-$i.txt || { echo "STACK ERROR: native run $i did not use the PowerVR native driver (GL_RENDERER)" >&2; exit 1; }
   echo "  native $i: $(score $R/native-$i.txt) $(date +%T)"
done
for i in 1 2; do
   DISPLAY=:0 MESA_LOADER_DRIVER_OVERRIDE=zink MESA_SHADER_CACHE_DISABLE=1 VK_ICD_FILENAMES=$ICD taskset -c 6-7 timeout 900 $GLM --off-screen --data-path $DATA > $R/zink-$i.txt 2>&1
   grep -q "zink" $R/zink-$i.txt || { echo "STACK ERROR: zink run $i did not use zink (GL_RENDERER)" >&2; exit 1; }
   echo "  zink $i: $(score $R/zink-$i.txt) $(date +%T)"
done
kill $WP 2>/dev/null; WP=""
echo "## vendor, bare X $(date +%T)"; sh $SW vendor > $R/sw-vendor.log 2>&1; sleep 5; echo "  bound=$(drv)"
sudo systemctl stop display-manager; sleep 3
for p in kwin_x11 plasmashell startplasma-x11 Xorg; do for q in $(pgrep -x $p); do sudo kill $q 2>/dev/null; done; done; sleep 3
sudo Xorg :1 -ac -nolisten tcp vt8 > $R/xorg1.log 2>&1 & sleep 6
# The IMG stack loads its own DRI driver (/usr/local/lib/dri/pvr_dri.so is IMG's Mesa-24.0.1 fork);
# this env is what selects it (line-31 env kept from the working bench31).
for i in 1 2; do
   sudo env -u LD_LIBRARY_PATH DISPLAY=:1 MESA_LOADER_DRIVER_OVERRIDE=pvr LIBGL_DRIVERS_PATH=/usr/local/lib/dri \
      taskset -c 6-7 timeout 900 $GLM --off-screen --data-path $DATA > $R/vendor-$i.txt 2>&1
   grep -q "build 24.2" $R/vendor-$i.txt || { echo "STACK ERROR: vendor run $i did not use the IMG driver (GL_VERSION)" >&2; exit 1; }
   if grep -q "Mesa" $R/vendor-$i.txt; then
      echo "STACK ERROR: vendor run $i loaded a Mesa driver" >&2
      exit 1
   fi
   echo "  vendor $i: $(score $R/vendor-$i.txt) $(date +%T)"
done
