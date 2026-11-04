#!/bin/bash
# daily.sh vendor|open - daily-driver benchmark of one GPU stack in a real Plasma desktop on the HDMI
# screen, verifiable remotely (text logs + screenshots under $O).
#   vendor: the running X11 + kwin_x11 desktop on pvrsrvkm (left running; nothing switched).
#   open  : display-manager stopped, open powervr bound (switch-safe.sh also switches the system to the
#           open Mesa in /opt/mesa-open), then DESKTOP=weston or plasma started in a transient logind
#           session on tty2; torn down and the vendor desktop restored at exit.
# Steps: compositor/GL/Vulkan info + screenshot; Chromium and Firefox on page.html (WebGL, CSS layers,
# 1080p video); glmark2 windowed (open: native GLES2 and zink); 15-min soak; 2-min idle cost.
set -u
STACK=${1:?usage: daily.sh vendor|open}
DESKTOP=${DESKTOP:-weston}   # open stack only: weston, or plasma (kwin_wayland on GBM/kmsro, zink)
PL=0; [ $STACK = open ] && [ $DESKTOP = plasma ] && PL=1
KW=0; [ $STACK = open ] && [ $DESKTOP = kwin ] && KW=1 && PL=1   # bare kwin: the Plasma paths, kwin as the shell
OP=/home/radxa/open-perf; D=$OP/tasks/T-2026-11-03-daily; A=$D/assets
O=$D/runs/$(date +%m%d-%H%M)-$STACK; mkdir -p $O
SW=/home/radxa/a733-powervr-fex/open-driver-perf/tools/verify/switch-safe.sh
GLM=$OP/glm2x11/bin/glmark2-es2; DATA=$OP/glm2x11/share/glmark2
T0=$(date "+%Y-%m-%d %H:%M:%S")
drv() { basename "$(readlink /sys/bus/platform/devices/1800000.gpu/driver)"; }
hb() { echo "$(date +%T) $*" | tee -a $O/heartbeat; }
dsnap() { ps -eo pid,stat | awk '$2 ~ /D/ {print $1}' | sort; }
dst() { local a b c; a=$(dsnap); sleep 2; b=$(dsnap); sleep 2; c=$(dsnap); comm -12 <(echo "$a") <(echo "$b") | comm -12 - <(echo "$c"); }
klog() { sudo -n journalctl -k --since "$T0" -o short-precise; }
# PVRDBG debug lines mention HWR state on every kick; they are not errors.
gpuerr() { klog | grep -v PVRDBG | grep -ciE "FWCCB command 2abc0069|Job timeout|pvr_cccb|page fault|mmu fault|lockup|HWR|hard reset|timed out|Oops"; }
maxtemp() { cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | sort -n | tail -1 | awk '{printf "%.1f", $1/1000}'; }
shellpid() { if [ $KW = 1 ]; then pgrep -u radxa -x kwin_wayland | head -1
   elif [ $STACK = open ] && [ $PL = 0 ]; then pgrep -u radxa -x weston | head -1; else pgrep -u radxa -x plasmashell | head -1; fi; }
xdisp() { grep -o 'xserver listening on display :[0-9]*' /tmp/daily-weston.log 2>/dev/null | tail -1 | grep -o ':[0-9]*'; }
# Run a command with the desktop session's whole environment, as an app started from the desktop gets
# it (vendor: plasmashell's, whose GL is zink on the vendor Vulkan driver; open: the launcher's, plus
# the Wayland socket and the Xwayland display).
senv() { local p; p=$(shellpid); [ -n "$p" ] || return 1
   local -a e
   if [ $KW = 1 ]; then mapfile -d '' e < /tmp/daily-open.env; e+=(WAYLAND_DISPLAY=wayland-1 DISPLAY=:0)
   elif [ $STACK = open ] && [ $PL = 0 ]; then mapfile -d '' e < /tmp/daily-open.env; e+=(WAYLAND_DISPLAY=wayland-1 DISPLAY=$(xdisp) XDG_SESSION_TYPE=wayland)
   else mapfile -d '' e < /proc/$p/environ; fi
   env -i "${e[@]}" "$@"; }
shot() { if [ $STACK = vendor ]; then senv xwd -root -silent | ffmpeg -y -loglevel error -f xwd_pipe -i - -vf scale=1920:-2 -frames:v 1 $O/$1.png
         elif [ $PL = 1 ]; then senv python3 $A/shot.py $O/$1.png > /dev/null 2>&1
         else rm -f /tmp/daily-shot/*.png; mkdir -p /tmp/daily-shot
            (cd /tmp/daily-shot && senv timeout 20 weston-screenshooter > /dev/null 2>&1)
            f=$(ls /tmp/daily-shot/*.png 2>/dev/null | head -1)
            [ -n "$f" ] && ffmpeg -y -loglevel error -i "$f" -vf scale=1920:-2 -frames:v 1 $O/$1.png; fi
         # MODE (vcheck.py full|screen) checks the picture is plausibly right, not just that fps came out.
         if [ ! -s $O/$1.png ]; then VFAIL=$((VFAIL + 1)); hb "screenshot $1 FAILED"
         elif [ -n "${2:-}" ]; then local v; v=$(python3 $A/vcheck.py $2 $O/$1.png) || VFAIL=$((VFAIL + 1)); hb "screenshot $1.png: $v"
         else hb "screenshot $1.png"; fi; }
VFAIL=0
DSK=screen; [ $KW = 1 ] && DSK=""   # bare kwin has no wallpaper or panel: a black desktop is right there
WPIDS=""
# Console KMS clients (kmscon on any tty, e.g. autovt@tty2 after a session there ends) hold card0 and
# make switch-safe.sh refuse the switch; stop them right before each switch.
stopcons() { sudo systemctl stop 'autovt@*.service' 'kmsconvt@*.service' 2>/dev/null; sleep 2; }
# Plasma copies its session environment into the user's systemd manager; put that back as it was so
# the open stack's paths do not leak into later (vendor) sessions.
uenv() { sudo systemctl --user -M radxa@ "$@"; }
uenv_restore() {
   [ -s $O/user-env-before.txt ] || return 0
   uenv show-environment > $O/user-env-after.txt 2>/dev/null
   local n line
   for n in $(cut -d= -f1 $O/user-env-after.txt); do
      grep -q "^$n=" $O/user-env-before.txt || uenv unset-environment "$n"; done
   while IFS= read -r line; do
      grep -qxF -- "$line" $O/user-env-after.txt && continue
      case "$line" in *"\$'"*) hb "WARN: user env ${line%%=*} not restored (quoted value)";; *) uenv set-environment "$line";; esac
   done < $O/user-env-before.txt
   uenv show-environment > $O/user-env-restored.txt 2>/dev/null
   cmp -s $O/user-env-before.txt $O/user-env-restored.txt && hb "user env restored" || hb "WARN: user env differs after restore"
}
teardown_open() {
   sudo systemctl stop daily-plasma 2>/dev/null; sudo systemctl reset-failed daily-plasma 2>/dev/null
   for s in $(loginctl list-sessions --no-legend | awk '$3=="radxa" && $4=="seat0" {print $1}'); do sudo loginctl terminate-session $s; done
   for i in $(seq 1 20); do pgrep -u radxa -x "weston|kwin_wayland|plasmashell|Xwayland|ksmserver" >/dev/null || break; sleep 1; done
   pgrep -u radxa -l -x "weston|kwin_wayland|plasmashell|Xwayland|ksmserver" >> $O/heartbeat && hb "WARN: session processes left"
   cp /tmp/daily-weston.log $O/weston.log 2>/dev/null
}
restore() {
   rc=$?
   for p in $WPIDS; do kill $p 2>/dev/null; done
   if [ $STACK = open ]; then
      teardown_open
      uenv_restore
      if [ -n "$(dst)" ]; then hb "RESTORE REFUSED: persistent D-state tasks"; dsnap >> $O/heartbeat
         echo "$(date) daily-$STACK: restore REFUSED (D state)" >> $OP/session.log; exit 3; fi
      stopcons; sh $SW vendor > $O/sw-restore.log 2>&1; sleep 5
      sudo systemctl start display-manager; sleep 15
      pgrep -x kwin_x11 >/dev/null || { sudo systemctl restart display-manager; sleep 15; }
   fi
   klog > $O/kernel.log
   [ $STACK = open ] && sudo systemctl stop pvr-wedge-guard 2>/dev/null
   hb "RESTORED bound=$(drv) dm=$(systemctl is-active display-manager) kwin_x11=$(pgrep -x kwin_x11 >/dev/null && echo up || echo DOWN) gpu-errors=$(gpuerr) visual-fails=$VFAIL"
   rm -f $OP/.busy
   echo "$(date) daily-$STACK done rc=$rc (results $O)" >> $OP/session.log
   hb "DAILY-DONE rc=$rc"; exit $rc
}
[ "$(drv)" = pvrsrvkm ] || { echo "not on vendor"; exit 1; }
[ -e $OP/.busy ] && { echo ".busy: $(cat $OP/.busy)"; exit 1; }
pgrep -f "[g]lmark2|[d]rawbench|[d]eqp-vk" >/dev/null && { echo "GPU benchmark already running"; exit 1; }
[ -n "$(dst)" ] && { echo "D-state tasks present"; exit 1; }
trap restore EXIT
echo "daily-$STACK $(date +%T): desktop benchmark; do not use the GPU" > $OP/.busy
echo "$(date) daily-$STACK start (undo: open only - stop daily-plasma, switch-safe vendor, start display-manager)" >> $OP/session.log

if [ $STACK = open ]; then
   # Reboots the board through sysrq on a CCCB overflow or repeated GPU job timeouts (wedge-guard.sh).
   sudo systemd-run --unit=pvr-wedge-guard /bin/bash $A/wedge-guard.sh > /dev/null 2>&1 && hb "wedge guard armed"
   sudo systemctl stop display-manager; sleep 5
   stopcons; sh $SW open > $O/sw-open.log 2>&1; sleep 3
   [ "$(drv)" = powervr ] || { hb "switch to open FAILED: $(tail -2 $O/sw-open.log | tr '\n' ' ')"; exit 1; }
   hb "bound=$(drv) module srcversion loaded $(cat /sys/module/powervr/srcversion) installed $(/sbin/modinfo -F srcversion /lib/modules/$(uname -r)/extra/powervr/powervr.ko)"
   uenv show-environment > $O/user-env-before.txt 2>/dev/null
   rm -f /tmp/daily-weston.log /tmp/daily-open.env
   # A failed transient unit stays loaded and makes systemd-run refuse the same unit name.
   sudo systemctl stop daily-plasma 2>/dev/null; sudo systemctl reset-failed daily-plasma 2>/dev/null
   sudo systemd-run --unit=daily-plasma -p PAMName=login -p TTYPath=/dev/tty2 -p TTYReset=yes \
      -p StandardInput=tty-force -p StandardOutput=journal -p StandardError=journal -p User=radxa \
      -p Environment=XDG_SESSION_TYPE=wayland -p Environment=XDG_SESSION_CLASS=user \
      -p Environment=XDG_SESSION_DESKTOP=weston -p Environment=XDG_SEAT=seat0 -p Environment=XDG_VTNR=2 \
      /bin/bash $A/$( [ $KW = 1 ] && echo kwin-open.sh || { [ $PL = 1 ] && echo plasma-open.sh || echo weston-open.sh; } ) > $O/systemd-run.log 2>&1
   # Catch a (possibly short-lived) kwin_wayland: the environment and GBM/EGL libraries it really got.
   [ $PL = 1 ] && ( for i in $(seq 1 600); do kp=$(pgrep -u radxa -x kwin_wayland | head -1)
      if [ -n "$kp" ]; then sudo -n cat /proc/$kp/environ 2>/dev/null | tr '\0' '\n' > $O/kwin-env.txt
         sudo -n cat /proc/$kp/maps 2>/dev/null | grep -oE "/[^ ]*(libgbm|libEGL|libgallium|dri_gbm)[^ ]*" | sort -u > $O/kwin-libs.txt
         [ -s $O/kwin-libs.txt ] && break; fi; sleep 0.1; done ) &
   command -v chvt > /dev/null && sudo chvt 2; sleep 1   # kbd (chvt) is not installed; loginctl activate below switches
   # weston can only flip (be DRM master) while its logind session is the active one on the seat.
   sid=$(loginctl list-sessions --no-legend | awk '$3=="radxa" && $4=="seat0" && $7=="tty2" {print $1}' | head -1)
   [ -n "$sid" ] && sudo loginctl activate "$sid"
   up=0; for i in $(seq 1 90); do
      if [ $KW = 1 ]; then [ -n "$(shellpid)" ] && [ -S /run/user/1000/wayland-1 ] && { up=$i; break; }
      elif [ $PL = 1 ]; then [ -n "$(shellpid)" ] && pgrep -u radxa -x kwin_wayland > /dev/null && { up=$i; break; }
      else [ -n "$(shellpid)" ] && [ -S /run/user/1000/wayland-1 ] && [ -n "$(xdisp)" ] && { up=$i; break; }; fi; sleep 1; done
   # Which GBM/EGL/gallium the compositor really mapped (the vendor ones live in /usr/local/lib).
   kp=$(pgrep -u radxa -x kwin_wayland | head -1); [ -z "$kp" ] && kp=$(pgrep -u radxa -x weston | head -1)
   # kwin_wayland makes itself non-dumpable, so its maps are root-only.
   [ -n "$kp" ] && sudo -n cat /proc/$kp/maps | grep -oE "/[^ ]*(libgbm|libEGL|libgallium|dri_gbm)[^ ]*" | sort -u > $O/compositor-libs.txt
   sudo journalctl -u daily-plasma --since "$T0" --no-pager > $O/session-journal.log 2>&1
   if [ $up = 0 ]; then
      { echo "== unit status"; sudo systemctl status daily-plasma --no-pager -l
        echo "== unit processes"; sudo systemd-cgls --no-pager -u daily-plasma
        echo "== radxa processes"; ps -u radxa -o pid,ppid,stat,etime,wchan:20,cmd --sort=start_time | tail -40
        echo "== kernel"; klog | tail -30; } > $O/start-diag.txt 2>&1
      hb "DESKTOP DID NOT START within 90 s (see session-journal.log, start-diag.txt)"; exit 2
   fi
   hb "compositor libs: $(tr '\n' ' ' < $O/compositor-libs.txt 2>/dev/null)"
   hb "$DESKTOP up after ${up}s (X display $(xdisp)); session ${sid:-none} $(loginctl show-session "$sid" -p Active -p State 2>/dev/null | tr '\n' ' ')"; sleep 10
   if [ $PL = 0 ] && grep -q "repaint-flush failed" /tmp/daily-weston.log; then
      hb "weston cannot flip (repaint-flush failed: not DRM master) - results would be invalid"; exit 2; fi
else
   [ -n "$(shellpid)" ] && pgrep -x kwin_x11 >/dev/null || { hb "vendor desktop not running"; exit 1; }
   hb "vendor X11 desktop running"
fi

# (1) what the desktop and apps get
if [ $STACK = open ] && [ $PL = 0 ]; then
   grep -E 'Using rendering device|EGL version|GL version|GL renderer|Output .* enabled|mode [0-9]+x' /tmp/daily-weston.log > $O/compositor-info.txt
   hb "weston: $(grep -E 'GL renderer|GL version|Using rendering device' $O/compositor-info.txt | sed 's/.*\] *//' | head -3 | tr '\n' ';')"
else
   senv qdbus6 org.kde.KWin /KWin supportInformation > $O/kwin-support.txt 2>&1
   hb "kwin: $(grep -E 'Name: KWin::|Compositing is|^Compositing Type|OpenGL renderer string|OpenGL version string' $O/kwin-support.txt | head -5 | tr -s ' ' | tr '\n' ';')"
fi
senv eglinfo -B > $O/eglinfo.txt 2>&1; senv glxinfo -B > $O/glxinfo.txt 2>&1; senv vulkaninfo --summary > $O/vulkaninfo.txt 2>&1
hb "glx: $(grep -m2 -E 'OpenGL renderer|OpenGL version|OpenGL ES profile version' $O/glxinfo.txt | tr -s ' ' | tr '\n' ';') vk: $(grep -m1 -E 'deviceName' $O/vulkaninfo.txt | tr -s ' ')"
shot desktop-idle $DSK

# (2) browsers on the test page
# DIAG=1: the Chromium GPU process's threads (wait channel + kernel stack) and the kernel log, 20 s
# and 50 s into each Chromium run.
diagdump() { local p t f=$O/diag-$1.txt
   p=$(pgrep -u radxa -f "type=gpu-process" | head -1)
   { echo "== $(date +%T) gpu-process ${p:-none}"
     for t in /proc/$p/task/*; do [ -d "$t" ] || continue
        echo "-- $(basename $t) $(cat $t/comm 2>/dev/null) $(awk '{print $3}' $t/stat 2>/dev/null) wchan=$(cat $t/wchan 2>/dev/null)"
        sudo -n cat $t/stack 2>/dev/null | head -8; done
     for w in $(pgrep -u radxa -x "weston|Xwayland"); do echo "== $(cat /proc/$w/comm) $w"
        for t in /proc/$w/task/*; do echo "-- $(basename $t) $(cat $t/comm 2>/dev/null) $(awk '{print $3}' $t/stat 2>/dev/null) wchan=$(cat $t/wchan 2>/dev/null)"
           sudo -n cat $t/stack 2>/dev/null | head -10; done; done
     echo "== kernel (non-debug)"; klog | grep -v PVRDBG | tail -5
     echo "== PVRDBG lines so far: $(klog | grep -c PVRDBG)"; } >> $f 2>&1; }
CR_FLAGS="--kiosk --no-first-run --no-default-browser-check --password-store=basic --enable-logging=stderr --v=0 --autoplay-policy=no-user-gesture-required"
browser() { local name=$1; shift; local log=$O/$name.log
   senv "$@" > $log 2>&1 & local pid=$!; WPIDS="$WPIDS $pid"
   # One screenshot per page phase, 4 s after the page logs its start (Firefox escapes the quotes).
   local -A due=(); local ph
   for i in $(seq 1 100); do sleep 1
      for ph in webgl css video; do
         [ -z "${due[$ph]:-}" ] && grep -qE "phase.{1,6}name.{1,6}$ph" $log && due[$ph]=$((i + 4))
         [ "${due[$ph]:-}" = $i ] && shot $name-$ph $( [ $ph = video ] && echo screen || echo full )
      done
      [ "${DIAG:-0}" = 1 ] && case "$name:$i" in chromium*:20|chromium*:50) diagdump $name;; esac
      grep -q '"test":"done"' $log && break; done
   for ph in webgl css video; do [ -n "${due[$ph]:-}" ] || { VFAIL=$((VFAIL + 1)); hb "$name: phase $ph never started"; }; done
   kill $pid 2>/dev/null; sleep 2; pkill -u radxa -f "$O/$name-profile" 2>/dev/null; sleep 2
   hb "$name: $(grep -o 'DAILYBENCH {[^}]*}' $log | grep -v phase | sed 's/DAILYBENCH //' | tr '\n' ' ')"; }
rm -rf $O/chromium-profile $O/firefox-profile; mkdir -p $O/firefox-profile; cp $A/ff-user.js $O/firefox-profile/user.js
for r in $(seq 1 ${CHROMIUM_RUNS:-1}); do
   [ "$r" = "${CHAIN_OFF_RUN:-0}" ] && echo 0 | sudo tee /sys/module/powervr/parameters/chain_frag > /dev/null && hb "chain_frag=$(cat /sys/module/powervr/parameters/chain_frag) for chromium run $r"
   rm -rf $O/chromium$r-profile
   browser chromium$r chromium $CR_FLAGS --user-data-dir=$O/chromium$r-profile file://$A/page.html
   hb "after chromium run $r: gpu-errors=$(gpuerr)"
done
[ "${STOP_AFTER_CHROMIUM:-0}" = 1 ] && exit 0
browser firefox firefox --kiosk --no-remote --profile $O/firefox-profile file://$A/page.html

# (3) glmark2, windowed in the desktop
glm() { local tag=$1; shift; local f=$O/glmark2-$tag.txt
   senv "$@" timeout 900 $GLM --data-path $DATA > $f 2>&1
   hb "glmark2 $tag: $(grep -m1 GL_RENDERER $f | tr -s ' ') $(grep -o 'glmark2 Score: [0-9]*' $f) gpu-errors=$(gpuerr)"
   # Pixel check of each scene against glmark2's references. The vendor stack fails
   # [function] fragment-complexity=medium too, so compare the failing scenes with its run.
   local v=$O/glmark2-$tag-validate.txt
   senv "$@" timeout 300 $GLM --data-path $DATA --validate > $v 2>&1
   local nf; nf=$(grep -c "Validation: Failure" $v); VFAIL=$((VFAIL + nf))
   hb "glmark2 $tag validate: $(grep -c 'Validation: Success' $v) ok, $nf failed $(grep 'Validation: Failure' $v | cut -d: -f1-2 | tr '\n' ' '), $(grep -c 'Validation: Unknown' $v) no reference"; }
if [ $STACK = open ]; then
   glm default env
   glm native env MESA_LOADER_DRIVER_OVERRIDE=pvr
   glm zink env MESA_LOADER_DRIVER_OVERRIDE=zink
else
   glm default env
fi
shot after-glmark2 $DSK

# (4) soak: 15 minutes of glmark2 looping in the desktop (default driver), plus the session staying up
senv timeout 900 $GLM --data-path $DATA --run-forever > $O/soak.txt 2>&1; src=$?
hb "soak: exit $src (124 = ran the full 15 min) frames-lines $(grep -c 'FPS:' $O/soak.txt) session $( [ -n "$(shellpid)" ] && echo up || echo GONE ) gpu-errors=$(gpuerr) maxtemp $(maxtemp)C"

# (5) idle cost of the desktop: CPU of compositor + shell + X server over 120 s, RAM, temperature
ticks() { local t=0 p; for p in $(pgrep -u radxa -x "kwin_x11|kwin_wayland|plasmashell|Xwayland|weston"; pgrep -x "X|Xorg"); do
   t=$((t + $(awk '{print $14+$15}' /proc/$p/stat 2>/dev/null || echo 0))); done; echo $t; }
a=$(ticks); sleep 120; b=$(ticks); hz=$(getconf CLK_TCK)
hb "idle 120s: desktop processes CPU $(awk -v d=$((b-a)) -v hz=$hz 'BEGIN{printf "%.1f", d/hz/120*100}')% of one core, RAM used $(free -m | awk '/^Mem/{print $3}') MB, maxtemp $(maxtemp)C"
shot desktop-end $DSK
true
