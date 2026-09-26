#!/bin/bash
# REPORT-ONLY. Deliberately does not swap files any more: the v1/v2 behaviour caused the very
# failure it was meant to catch (it replaced the patched module with stock a moment before the
# autoload ran, so the stock build loaded instead). A missing GPU at boot is survivable - the
# desktop falls back to software GL - and it lets the patched module be insmod'd by hand to see
# the real error.
M=pvrsrvkm; PATCHED_BI=5d842b087f075c97bbf5225fb08d39ca3c145235
for _ in $(seq 1 25); do grep -q "^$M " /proc/modules && break; sleep 1; done
if grep -q "^$M " /proc/modules; then
  BI=$(od -An -tx1 /sys/module/$M/notes/.note.gnu.build-id 2>/dev/null | tr -d ' \n' | sed 's/^040000001400000003000000474e5500//')
  [ "$BI" = "$PATCHED_BI" ] && echo "prime-guard: $M loaded = PATCHED build (PRIME-import active)" \
                            || echo "prime-guard: $M loaded, build-id $BI (not the patched build)"
else
  echo "prime-guard: $M did NOT load within 25 s - load it by hand: sudo insmod /lib/modules/\$(uname -r)/updates/dkms/pvrsrvkm.ko"
fi
