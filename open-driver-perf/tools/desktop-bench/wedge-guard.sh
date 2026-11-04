#!/bin/bash
# wedge-guard.sh: run as root in a transient unit (systemd-run --unit=pvr-wedge-guard) before an
# open-stack test. Reads the kernel log from /dev/kmsg (no journald, sudo or logind needed once
# running) and reboots through sysrq on the first CCCB overflow warning or the 5th GPU job timeout,
# so a wedged board recovers without a power cycle. The vendor driver prints neither message.
log=/home/radxa/open-perf/session.log
n=0
while read -r line; do
   case "$line" in
      *"pvr_cccb.c:178"*) trip="CCCB overflow warning" ;;
      *"powervr"*"Job timeout"*) n=$((n + 1)); [ $n -ge 5 ] && trip="$n GPU job timeouts" || continue ;;
      *) continue ;;
   esac
   echo "$(date) pvr-wedge-guard: $trip -> sysrq sync, remount ro, reboot" >> $log
   sync &
   echo 1 > /proc/sys/kernel/sysrq
   echo s > /proc/sysrq-trigger; sleep 3
   echo u > /proc/sysrq-trigger; sleep 2
   echo b > /proc/sysrq-trigger
done < /dev/kmsg
