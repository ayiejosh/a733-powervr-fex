#!/bin/bash
# bsession.sh start|stop|status|ensure  -- one open-stack session for many experiments (instead of a switch/restore cycle each).
#   start/ensure: guarded switch to the open driver (stops the desktop, stops only a lingering kded6 by PID)
#   stop: restores vendor + desktop + schedutil + AllowedCPUs, but only after a liveness probe (foldtest) passes: no log-line counting
# Never unbinds a driver the probe says is unhealthy.
PROBE_ICD=${BSESSION_PROBE_ICD:-$HOME/open-perf/icd-alt3.json}
drv() { basename "$(readlink /sys/bus/platform/devices/1800000.gpu/driver)"; }
log() { echo "$(date "+%F %T") bsession $1" >> ~/open-perf/session.log; }
case "$1" in
status) echo "driver=$(drv) dm=$(systemctl is-active display-manager) crd=$(systemctl is-active crd-mirror) busy=$([ -e ~/open-perf/.busy ] && echo yes || echo no)";;
start|ensure)
  if [ "$(drv)" = powervr ]; then echo "open stack already loaded"; exit 0; fi
  log "start: desktop down, switch-open (undo: bsession.sh stop)"
  sudo systemctl stop display-manager; sleep 3
  for p in $(sudo fuser /dev/dri/renderD128 /dev/dri/card0 2>/dev/null | tr -s " " "\n" | grep -E "^[0-9]+$"); do
    n=$(ps -o comm= -p $p); echo "gpu holder $p=$n"; [ "$n" = kded6 ] && sudo kill $p; done; sleep 2
  sudo ~/gpu-open-stack/switch-open.sh 2>&1 | tail -1
  sudo systemctl set-property --runtime system.slice AllowedCPUs=0-5
  for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance | sudo tee $f >/dev/null; done
  echo "driver=$(drv)"; [ "$(drv)" = powervr ] || exit 1;;
stop)
  if [ "$(drv)" != powervr ]; then echo "not on the open stack"; exit 0; fi
  if pgrep -x deqp-vk >/dev/null; then echo "deqp-vk still running: not restoring"; exit 1; fi
  (cd ~/open-perf/foldtest && env VK_ICD_FILENAMES=$PROBE_ICD PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 60 ./foldtest 2>&1 | grep -q "ALL PASSED") \
    || { echo "GPU probe failed: NOT restoring (do not unbind; check dmesg, reboot procedure)"; log "stop refused: probe failed"; exit 2; }
  sudo ~/gpu-open-stack/switch-vendor.sh 2>&1 | tail -1; sleep 2
  sudo systemctl start display-manager
  sudo systemctl set-property --runtime system.slice AllowedCPUs=
  for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo schedutil | sudo tee $f >/dev/null; done
  sleep 5; log "stop: RESTORED vendor + display-manager, schedutil, AllowedCPUs cleared"
  echo "RESTORED driver=$(drv) dm=$(systemctl is-active display-manager) crd=$(systemctl is-active crd-mirror)";;
*) echo "usage: bsession.sh start|stop|status|ensure"; exit 64;;
esac
