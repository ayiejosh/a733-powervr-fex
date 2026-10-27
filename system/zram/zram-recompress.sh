#!/bin/sh
# zram-recompress.sh — move cold pages to the denser secondary algorithm.
#
# The zram device is configured with lz4 as primary (fast writes) and zstd as
# the recompression algorithm. This script marks pages that have not been
# touched for a while as idle, then asks zram to recompress the idle pages that
# are still larger than `threshold` bytes. Recompressed pages stop matching the
# threshold, so each run does less work than the previous one.
#
# Runs from zram-recompress.timer via zram-recompress.service with Nice=19, so
# the compression CPU it spends competes only for otherwise idle cycles.
set -u

DEV=/sys/block/zram0
IDLE_SECONDS=${IDLE_SECONDS:-1800}
THRESHOLD=${THRESHOLD:-1500}
MAX_LOAD=${MAX_LOAD:-6}

LOG=/home/radxa/zram-multicomp/recompress.log
log() { printf '%s [zram-recompress] %s\n' "$(date -Is)" "$*" | tee -a "$LOG" 2>/dev/null; }

# Stock module (or no device): nothing to do.
[ -e "$DEV/recompress" ] || exit 0
[ -e "$DEV/disksize" ] || exit 0
[ "$(cat "$DEV/disksize" 2>/dev/null || echo 0)" -gt 0 ] || exit 0

# The secondary algorithm can only be registered before the device is
# initialised (the kernel returns -EBUSY afterwards), so it is set by
# zramswap.service's ExecStartPre drop-in, not here. Warn if it is missing.
if ! grep -q 'zstd' "$DEV/recomp_algorithm" 2>/dev/null; then
  log "no secondary algorithm configured - check zramswap.service.d/10-recomp.conf"
  exit 0
fi

# Skip when something else is already working the CPU.
LOAD=$(awk '{print int($1)}' /proc/loadavg)
[ "$LOAD" -ge "$MAX_LOAD" ] && { log "load $LOAD too high, skipping"; exit 0; }

before_data=$(awk '{print $1}' "$DEV/mm_stat")
before_compr=$(awk '{print $2}' "$DEV/mm_stat")

# Incompressible pages waste a full 4 KB each; if a backing device is
# configured, hand them to it before spending CPU on recompression.
if [ -e "$DEV/backing_dev" ] && [ -n "$(cat "$DEV/backing_dev" 2>/dev/null)" ]; then
  echo huge > "$DEV/writeback" 2>/dev/null || log "huge writeback failed"
fi

echo "$IDLE_SECONDS" > "$DEV/idle" 2>/dev/null || log "idle write failed (no access-time tracking?)"
if ! echo "type=idle threshold=$THRESHOLD" > "$DEV/recompress" 2>/dev/null; then
  log "recompress write failed"
  exit 0
fi

after_data=$(awk '{print $1}' "$DEV/mm_stat")
after_compr=$(awk '{print $2}' "$DEV/mm_stat")
log "idle>=${IDLE_SECONDS}s, threshold=${THRESHOLD}B: data $((${after_data:-0}/1048576))MB compr $((${after_compr:-0}/1048576))MB" \
    "(was $((${before_data:-0}/1048576))MB / $((${before_compr:-0}/1048576))MB)"
