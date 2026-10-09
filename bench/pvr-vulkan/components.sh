#!/bin/sh
# Probe every small component and say what works. One run, one table.
# Each check prints: OK / FAIL / WARN + the concrete value it saw.
B=/mnt/sdcard/_REVIEW/emulation/trixie-prep/bench/pvr-vulkan
gpu_dev=1800000.gpu
ok=0; bad=0
row() { printf "  %-42s %-5s %s\n" "$1" "$2" "$3"; [ "$2" = OK ] && ok=$((ok+1)) || bad=$((bad+1)); }

echo "===================== COMPONENT DUMP $(date +%H:%M:%S) ====================="
echo
echo "-- kernel / modules --"
for m in pvrsrvkm powervr drm_shmem_helper drm_exec gpu_sched; do
  n=$(lsmod | grep -c "^$(echo $m | tr - _)")
  s=$(lsmod | awk -v m="$m" '$1==m{print $2" bytes, refs "$3}')
  [ "$n" -gt 0 ] && row "module $m" OK "loaded ($s)" || row "module $m" "--" "not loaded"
done
run=$(uname -r)
row "running kernel" OK "$run"
if [ -f /home/radxa/kernel-src/powervr/powervr.ko ]; then
  vm=$(modinfo /home/radxa/kernel-src/powervr/powervr.ko 2>/dev/null | sed -n 's/^vermagic: *//p' | awk '{print $1}')
  [ -z "$vm" ] && vm=$(strings /home/radxa/kernel-src/powervr/powervr.ko 2>/dev/null | sed -n 's/^vermagic=//p' | awk '{print $1}')
  case "$vm" in $run*) row "powervr.ko vermagic" OK "$vm";; *) row "powervr.ko vermagic" WARN "built for '$vm', running $run";; esac
else row "powervr.ko" FAIL "missing"; fi

echo
echo "-- device / driver binding --"
d=$(ls -l /sys/bus/platform/devices/$gpu_dev/driver 2>/dev/null | sed 's/.*-> //' | xargs basename 2>/dev/null)
[ -n "$d" ] && row "GPU driver bound" OK "$d" || row "GPU driver bound" FAIL "nothing bound"
for n in /dev/dri/card0 /dev/dri/renderD128; do
  [ -e "$n" ] && row "device $n" OK "$(stat -c '%a %U' $n)" || row "device $n" FAIL "absent"
done
for a in $(ls /sys/class/devfreq/${gpu_dev}*/cur_freq 2>/dev/null) $(ls /sys/kernel/debug/pvr/apphint/EnableFTraceGPU 2>/dev/null); do
  [ -e "$a" ] && row "knob $(basename $(dirname $a))/$(basename $a)" OK "$(cat $a 2>/dev/null | head -1)"
done
[ -d /sys/kernel/debug/pvr/apphint ] || row "pvr apphint debugfs" "--" "absent under this driver"

echo
echo "-- firmware --"
for f in /lib/firmware/powervr/rogue_36.56.104.183_v1.fw /lib/firmware/rgx.fw.36.56.104.183; do
  if [ -e "$f" ]; then row "fw $(basename $f)" OK "$(stat -c%s $f) B  $(md5sum $f | cut -c1-12)"; else row "fw $(basename $f)" "--" "absent"; fi
done

echo
echo "-- userspace (Vulkan ICDs + Mesa) --"
for i in /home/radxa/pvr_gen_icd.json /usr/share/vulkan/icd.d/img_icd.json; do
  if [ -e "$i" ]; then
    lib=$(python3 -c "import json,sys;print(json.load(open('$i'))['ICD']['library_path'])" 2>/dev/null)
    real=$(ldconfig -p 2>/dev/null | awk -v l="$(basename $lib)" '$1==l{print $NF; exit}')
    [ -z "$real" ] && [ -e "$lib" ] && real=$lib
    if [ -z "$real" ]; then
      for d in /usr/lib /lib /usr/lib/aarch64-linux-gnu /usr/local/lib; do
        [ -e "$d/$(basename $lib)" ] && { real="$d/$(basename $lib)"; break; }
      done
    fi
    if [ -n "$real" ] && [ -e "$real" ]; then row "ICD $(basename $i)" OK "$(basename $lib) -> $real"
    else row "ICD $(basename $i)" FAIL "library $lib unresolvable"; fi
  else row "ICD $(basename $i)" "--" "absent"; fi
done
for l in /usr/lib/libVK_IMG.so.24.2.6603887 /home/radxa/mesa/mesa-main/build/src/imagination/vulkan/libvulkan_powervr_mesa.so; do
  [ -e "$l" ] && row "lib $(basename $l)" OK "$(stat -c%s $l) B" || row "lib $(basename $l)" FAIL "absent"
done

echo
echo "-- mesa source state (the four fixes) --"
cd /home/radxa/mesa/mesa-main 2>/dev/null && {
  u=$(grep -o "max_unroll_iterations = [0-9]*" src/imagination/pco/pco_nir.c | head -1)
  h=$(grep -c "Block-local hoisting" src/imagination/pco/pco_const_imms.c)
  row "pco unroll limit" OK "$u"
  row "pco immediate hoisting" $([ "$h" -gt 0 ] && echo OK || echo FAIL) "found in pco_const_imms.c"
  row "mesa git state" OK "$(git log --oneline -1 | cut -c1-40), $(git status --porcelain | wc -l) modified, $(git rev-list --count main..HEAD) ahead"
}

echo
echo "-- tracing / instruments --"
# ponytail: the tracing paths are root-only, and an unreadable directory is NOT proof of
# absence - it reported pvr_fence as missing while the harness read those events every run.
# Say "needs root" instead of a false negative.
for e in gpu_scheduler pvr_fence; do
  n=0
  for t in /sys/kernel/tracing/events/$e /sys/kernel/debug/tracing/events/$e; do
    m=$(ls "$t" 2>/dev/null | wc -l); [ "$m" -gt "$n" ] && n=$m
  done
  if [ "$n" -gt 0 ]; then
    row "tracepoints $e" OK "$n events"
  elif [ "$(id -u)" = 0 ]; then
    row "tracepoints $e" "--" "absent under the bound driver (checked as root)"
  else
    row "tracepoints $e" "--" "cannot read tracing dirs - re-run as root to know"
  fi
done
[ -w /sys/kernel/debug/tracing/tracing_on ] && row "trace control writable" OK "yes" || row "trace control writable" WARN "no (need root)"
pgrep -x kwin_x11 >/dev/null || pgrep -x kwin_wayland >/dev/null && row "compositor (blocks switch)" WARN "kwin alive" || row "compositor" OK "none - switching allowed"
systemctl is-active display-manager 2>/dev/null | grep -q active && row "display-manager" OK "active" || row "display-manager" "--" "inactive"
systemctl is-active gpu-fw-guard 2>/dev/null | grep -q active && row "gpu-fw-guard" OK "active" || row "gpu-fw-guard" WARN "not active"

echo
echo "-- harness --"
for f in harness.py sweep.sh ab.sh; do
  [ -x "$B/$f" ] && row "$f" OK "$(wc -l < $B/$f) lines" || row "$f" FAIL "missing/not executable"
done
[ -f "$B/harness-log.jsonl" ] && row "harness-log.jsonl" OK "$(wc -l < $B/harness-log.jsonl) records" || row "harness-log.jsonl" "--" "no records yet"

echo
echo "  ===== $ok OK, $bad needs attention ====="
