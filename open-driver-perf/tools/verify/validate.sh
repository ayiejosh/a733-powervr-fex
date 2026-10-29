#!/bin/bash
R=/tmp/validate; mkdir -p $R
OP=/home/radxa/open-perf
SW=/home/radxa/Desktop/Projects/Radxa-A7A/open-driver-perf/tools/verify/switch-safe.sh
. /home/radxa/gpu-open-stack/env26.sh
sh $SW open > $R/sw-open.log 2>&1; sleep 3
export XDG_RUNTIME_DIR=/tmp/wroot; rm -rf /tmp/wroot; mkdir -p /tmp/wroot; chmod 700 /tmp/wroot
printf "[core]\nrequire-input=false\n" > /tmp/wsmoke.ini
weston --config=/tmp/wsmoke.ini --backend=headless-backend.so --renderer=gl --xwayland --socket=wayland-open --idle-time=0 --log=$R/weston.log >/dev/null 2>&1 & WP=$!
sleep 4
DISPLAY=:0 MESA_LOADER_DRIVER_OVERRIDE=pvr MESA_SHADER_CACHE_DISABLE=1 timeout 600 $OP/glm2x11/bin/glmark2-es2 --off-screen --data-path /home/radxa/open-perf/glm2x11/share/glmark2 --validate > $R/native.txt 2>&1
echo "native validate: $(grep -c 'Validation: Success' $R/native.txt) success / $(grep -c 'Validation: Failure' $R/native.txt) fail / $(grep -c 'Validation: Skipped' $R/native.txt) skipped"
DISPLAY=:0 MESA_LOADER_DRIVER_OVERRIDE=zink MESA_SHADER_CACHE_DISABLE=1 VK_ICD_FILENAMES=$OP/icd-build.json timeout 600 $OP/glm2x11/bin/glmark2-es2 --off-screen --data-path /home/radxa/open-perf/glm2x11/share/glmark2 --validate > $R/zink.txt 2>&1
echo "zink   validate: $(grep -c 'Validation: Success' $R/zink.txt) success / $(grep -c 'Validation: Failure' $R/zink.txt) fail / $(grep -c 'Validation: Skipped' $R/zink.txt) skipped"
kill $WP 2>/dev/null; sleep 2
sh $SW vendor > $R/sw-vendor.log 2>&1; sleep 10
echo "restored bound=$(basename $(readlink /sys/bus/platform/devices/1800000.gpu/driver)) dm=$(systemctl is-active display-manager)"
