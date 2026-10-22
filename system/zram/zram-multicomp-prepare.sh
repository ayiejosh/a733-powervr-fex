#!/bin/sh
# zram-multicomp-prepare.sh — run from zramswap.service's ExecStartPre.
#
# It guarantees the two things zramswap itself cannot do:
#   1. a zram device exists (the module may already be loaded with none, e.g.
#      after any tool that removed it);
#   2. the algorithm writes happen while disksize is 0, which is the only state
#      the kernel accepts them in (__comp_algorithm_store -> -EBUSY afterwards).
#
# It never removes a device and never uses zramctl --reset: on this util-linux
# (2.41) --reset hot-removes the device, which is what left the board without
# zram swap after the last attempt.
set -u
SYS=/sys/block/zram0
SWAPDEV=/dev/zram0
LOG=/home/radxa/zram-multicomp/recompress.log
log() { printf '%s [prepare] %s\n' "$(date -Is)" "$*" >> "$LOG" 2>/dev/null; }

modprobe zram 2>/dev/null || true

if [ ! -e "$SYS/disksize" ]; then
  log "no zram0, adding one via zram-control"
  cat /sys/class/zram-control/hot_add >/dev/null 2>&1 || true
  i=0
  while [ ! -e "$SYS/disksize" ] && [ "$i" -lt 20 ]; do sleep 0.1; i=$((i+1)); done
fi
[ -e "$SYS/disksize" ] || { log "could not create zram0"; exit 1; }

# A device that is already initialised cannot be reconfigured: disksize_store
# rejects writes once init_done (verified in the module source), so the only
# legal reset is to remove the device and add a fresh one. hot_remove() calls
# zram_reset_device() itself and refuses only while the block device is open,
# hence swapoff first.
if [ "$(cat "$SYS/disksize" 2>/dev/null || echo 0)" != "0" ]; then
  if awk 'NR>1 && $1=="/dev/zram0"{f=1} END{exit !f}' /proc/swaps; then
    swapoff "$SWAPDEV" || { log "swapoff failed"; exit 1; }
  fi
  i=0
  while [ "$i" -lt 20 ]; do
    if echo 0 > /sys/class/zram-control/hot_remove 2>/dev/null; then break; fi
    sleep 0.2; i=$((i+1))
  done
  [ ! -e "$SYS/disksize" ] || { log "hot_remove did not free zram0 (still open?)"; exit 1; }
  cat /sys/class/zram-control/hot_add >/dev/null 2>&1 || true
  i=0
  while [ ! -e "$SYS/disksize" ] && [ "$i" -lt 20 ]; do sleep 0.1; i=$((i+1)); done
  [ -e "$SYS/disksize" ] || { log "hot_add did not recreate zram0"; exit 1; }
  log "device reset via hot_remove/hot_add"
fi

printf 'lz4' > "$SYS/comp_algorithm" || { log "primary algorithm write failed"; exit 1; }

# Optional writeback backing device (also only settable before init_done).
WB_IMG=/home/radxa/zram-writeback.img
if [ -f "$WB_IMG" ]; then
  LOOP=$(losetup -j "$WB_IMG" 2>/dev/null | cut -d: -f1 | head -1)
  if [ -z "$LOOP" ]; then
    LOOP=$(losetup --find --show "$WB_IMG" 2>/dev/null)
    log "attached $WB_IMG as $LOOP"
  fi
  if [ -n "$LOOP" ] && [ -e "$SYS/backing_dev" ]; then
    echo "$LOOP" > "$SYS/backing_dev" 2>/dev/null && log "backing_dev=$LOOP" || log "backing_dev write failed"
  fi
fi
if echo "algo=zstd priority=1" > "$SYS/recomp_algorithm" 2>/dev/null; then
  log "primary lz4, secondary zstd registered"
else
  log "secondary algorithm write failed (continuing with lz4 only)"
fi
exit 0
