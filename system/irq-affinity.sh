#!/bin/sh
# Pin latency-critical IRQs away from CPU0 and keep constant samplers on little cores.
# CPU0 was carrying 5.3M interrupts vs ~1.1M on the others (gpadc 2.6M + pvrsrvkm 1.5M).
# Re-applied every boot (IRQ numbers change), so this runs from a systemd unit.
set -u
set_irq() {
  name="$1"; cpu="$2"
  for n in $(awk -F: -v pat="$name" '$0 ~ pat {gsub(/[^0-9]/, "", $1); print $1}' /proc/interrupts); do
    [ -n "$n" ] || continue
    [ -w "/proc/irq/$n/smp_affinity_list" ] || continue
    if echo "$cpu" > "/proc/irq/$n/smp_affinity_list" 2>/dev/null; then
      echo "irq $n ($name) -> cpu $cpu"
    fi
  done
}
set_irq pvrsrvkm 6      # GPU: latency-critical, only fires while the GPU works
set_irq ufshcd 7        # UFS storage: bursty I/O, benefits from a big core
set_irq sunxi-gpadc 2   # constant ADC sampling: keep off CPU0 and off the big cores
set_irq tcon3 3         # display controller
