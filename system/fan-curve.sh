#!/bin/sh
# Radxa A7A thermal controller — GRADUAL 1-PWM-fine fan curve + clock backstop.
# 2026-06-12: millidegree input -> PWM glides in 1-unit steps. PWM 0 below 40.0C;
# linear 40.0->80.0C maps 0->255; 100% >=80.0C. No on/off hysteresis; gentle
# 110-pwm start-nudge only from a full stop.
set -u
INTERVAL=2
PWM=""
for h in /sys/class/hwmon/hwmon*; do
  [ -r "$h/name" ] || continue
  if [ "$(cat "$h/name" 2>/dev/null)" = "pwmfan" ] && [ -w "$h/pwm1" ]; then PWM="$h/pwm1"; break; fi
done
[ -z "$PWM" ] && { echo "fan-curve: pwmfan not found"; exit 1; }
set_pwm() { echo "$1" > "$PWM" 2>/dev/null; }
trap 'set_pwm 255; exit 0' TERM INT
trap 'set_pwm 255' EXIT
for z in /sys/class/thermal/thermal_zone*; do
  case "$(cat "$z/type" 2>/dev/null)" in
    cpul_thermal_zone|cpub_thermal_zone|gpu_thermal_zone)
      grep -qw user_space "$z/available_policies" 2>/dev/null && echo user_space > "$z/policy" 2>/dev/null ;;
  esac
done
POLICIES=""
for p in /sys/devices/system/cpu/cpufreq/policy*; do POLICIES="$POLICIES $p"; done
restore_clock() { for p in $POLICIES; do cat "$p/cpuinfo_max_freq" > "$p/scaling_max_freq" 2>/dev/null; done; }
cap_clock()     { for p in $POLICIES; do echo "$1"               > "$p/scaling_max_freq" 2>/dev/null; done; }

# 2026-09-22: release the thermal clamp the kernel applied BEFORE we switched the zones to
# user_space.  Once a zone is user_space its cooling state is frozen, and a stale non-zero
# state pins scaling_max_freq below cpuinfo_max_freq through the thermal freq-QoS clamp —
# writing scaling_max_freq cannot override it.  Measured cost of the stale state:
# little cores 1508 MHz / big cores 1716 MHz instead of 1794/2002 MHz = ~14% CPU lost
# (SHA-256, 8 threads: 5.78M -> 6.59M once released).
release_clamps() {
  for c in /sys/class/thermal/cooling_device*; do
    case "$(cat "$c/type" 2>/dev/null)" in
      cpufreq-*) echo 0 > "$c/cur_state" 2>/dev/null ;;
    esac
  done
}
release_clamps
restore_clock
THROTTLE_AT=78000; THROTTLE_TO=1404000; RESTORE_BELOW=74000; capped=0
# max CPU/GPU temp in MILLIDEGREES
cpu_mtemp() {
  m=0
  for z in /sys/class/thermal/thermal_zone*; do
    case "$(cat "$z/type" 2>/dev/null)" in
      cpul_thermal_zone|cpub_thermal_zone|gpu_thermal_zone)
        t=$(cat "$z/temp" 2>/dev/null); case "$t" in ''|*[!0-9]*) continue;; esac
        [ "$t" -gt "$m" ] && m=$t ;;
    esac
  done
  echo "$m"
}
LOW=58000; HIGH=78000; SPAN=20000
fan_for() {   # arg = millidegrees
  mc=$1
  if   [ "$mc" -le "$LOW" ];  then echo 0
  elif [ "$mc" -ge "$HIGH" ]; then echo 255
  else echo $(( (mc - LOW) * 255 / SPAN ))
  fi
}
cur=-1
while :; do
  mc=$(cpu_mtemp)
  if [ "$capped" -eq 0 ] && [ "$mc" -ge "$THROTTLE_AT" ]; then cap_clock "$THROTTLE_TO"; capped=1; fi
  if [ "$capped" -eq 1 ] && [ "$mc" -lt "$RESTORE_BELOW" ]; then release_clamps; restore_clock; capped=0; fi
  want=$(fan_for "$mc")
  if [ "$want" != "$cur" ]; then
    if [ "$cur" -le 0 ] && [ "$want" -gt 0 ] && [ "$want" -lt 110 ]; then set_pwm 110; sleep 1; fi
    set_pwm "$want"; cur=$want
  fi
  sleep "$INTERVAL"
done
