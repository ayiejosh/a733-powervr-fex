#!/bin/bash
# fwtime-wl.sh DRIVER SCENE: median TA / 3D / frame period in firmware ticks, glmark2 on headless
# weston (Wayland client) - the X11 glbench path cannot load the native driver in this state.
cd ~/open-perf
DRV=$1; SCENE=$2
. /home/radxa/gpu-open-stack/env26.sh
sudo sh -c "echo 0xffffffff > /sys/kernel/debug/dri/1/pvr_params/fw_trace_mask"
rm -rf /tmp/wroot; mkdir -p /tmp/wroot; chmod 700 /tmp/wroot
export XDG_RUNTIME_DIR=/tmp/wroot
printf "[core]\nrequire-input=false\n" > /tmp/wsmoke.ini
weston --config=/tmp/wsmoke.ini --backend=headless-backend.so --renderer=gl --socket=wayland-open --idle-time=0 --log=/tmp/ws-fw.log >/dev/null 2>&1 &
W=$!
sleep 3
rm -f /tmp/gs.txt
WAYLAND_DISPLAY=wayland-open MESA_LOADER_DRIVER_OVERRIDE=$DRV timeout 60 /tmp/glm2w/usr/bin/glmark2-es2-wayland --off-screen -b "$SCENE:duration=8" > /tmp/gs.txt 2>&1 &
C=$!
sleep 10; sudo cat /sys/kernel/debug/dri/1/pvr_fw/trace_0 > /tmp/fwt-$DRV.txt
wait $C 2>/dev/null
kill $W 2>/dev/null
python3 - /tmp/fwt-$DRV.txt <<'PY'
import re,sys
ev=[(int(m.group(1)),m.group(2)) for m in (re.match(r"\[(\d+)\] : (.*)",l) for l in open(sys.argv[1])) if m]
ta=[];d3=[];per=[];k=None;k3=None;p3=None
for t,s in ev:
    if s.startswith("Kick TA"): k=t
    elif s.startswith("TA finished") and k: ta.append(t-k); k=None
    elif s.startswith("Kick 3D"):
        if p3: per.append(t-p3)
        p3=t; k3=t
    elif s.startswith("3D finished") and k3: d3.append(t-k3); k3=None
med=lambda a: sorted(a)[len(a)//2] if a else 0
print("n",len(d3),"TA",med(ta),"3D",med(d3),"period",med(per))
PY
grep -oE "FPS: [0-9]+" /tmp/gs.txt
