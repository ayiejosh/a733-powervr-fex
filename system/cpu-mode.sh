#!/bin/sh
# cpu-mode.sh — CPU power/performance profile for the A733 board.
#
#   auto      schedutil + cpu-boost.service: idle at 416 MHz, floors jump to the
#             hardware maximum the moment user.slice shows real work (games, emulation,
#             desktop apps).  While boosted every core is at max — throughput identical
#             to "performance" — and it drops back ~1 s after the work stops.
#             Background daemons (syncthing etc., in system.slice) never trigger it.
#   max       both clusters "performance": always max clock, no dynamic behaviour.
#   balanced  little "schedutil", big "performance": big cores always ready.
#   eco       both clusters "schedutil", no boost: lowest idle power, -29% on
#             CPU-bound work.
#
# Idle floor is cpuinfo_min_freq (416 MHz); "hardware maximum" is 1794 MHz little /
# 2002 MHz big.
#
# The GPU/DSU clocks are independent of this (device-tree overlays, ../overlays/).
set -u
MODE="${1:-}"
P0=/sys/devices/system/cpu/cpufreq/policy0/scaling_governor
P6=/sys/devices/system/cpu/cpufreq/policy6/scaling_governor
BOOST=off
case "$MODE" in
  auto)     G0=schedutil;   G6=schedutil;   BOOST=on  ;;
  max)      G0=performance; G6=performance; BOOST=off ;;
  balanced) G0=schedutil;   G6=performance; BOOST=off ;;
  eco)      G0=schedutil;   G6=schedutil;   BOOST=off ;;
  *) echo "usage: $0 auto|max|balanced|eco" >&2; exit 2 ;;
esac
echo "$G0" > "$P0" 2>/dev/null || true
echo "$G6" > "$P6" 2>/dev/null || true
if [ "$BOOST" = on ]; then
  systemctl enable --now cpu-boost.service >/dev/null 2>&1 || true
else
  systemctl disable --now cpu-boost.service >/dev/null 2>&1 || true
fi
echo "cpu-mode: $MODE (policy0=$G0 policy6=$G6 boost=$BOOST)"
