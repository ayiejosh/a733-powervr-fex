#!/bin/bash
# full-compare.sh - vendor vs open PowerVR on the A733: performance AND compatibility.
#
# Runs BOTH drivers in ONE session, because run-to-run variance here is 10-25% between
# sessions and 5-17% within one - absolute numbers from different sessions are not
# comparable. Writes a per-arm report and prints a combined table.
#
#   sudo ./full-compare.sh [samples]
#
# Compatibility is sampled with vulkaninfo (headless, works with the desktop stopped).
# glmark2 needs a windowing system and is NOT run here - it cannot run during a switch.
#
# ponytail: deliberately does not use ab.sh, because ab.sh cannot run vulkaninfo per arm
# and its guard aborts on switch-script exit codes. This reuses the same proven sequence.
set -u
SAMPLES=${1:-3}
B="$(cd "$(dirname "$0")" && pwd)"
OUT=/tmp/full-compare
mkdir -p $OUT
KHZ=$(uname -r)

# --- the probe matrix: render sizes where they differ, plus compute ---
PERF_SPECS="vkrender:256:40 vkrender:512:40 vkrender:1024:40 vkrender:2048:15 vkrender:4096:6 vkheavy:2048:5"
COMPAT_SPECS="cstp:64:200 cstpf:64:200 cstpi:64:200 cstpin:64:200"

restore_desktop() {
  local d; d=$(readlink -f /sys/bus/platform/devices/1800000.gpu/driver 2>/dev/null | xargs basename)
  if [ "$d" != "pvrsrvkm" ]; then
    echo 1800000.gpu > /sys/bus/platform/drivers/powervr/unbind 2>/dev/null; sleep 2
    rmmod powervr 2>/dev/null; sleep 2
    modprobe pvrsrvkm 2>/dev/null; sleep 4
  fi
  systemctl start display-manager >/dev/null 2>&1
}
trap 'echo "[trap] restoring the desktop"; restore_desktop' EXIT

die() { echo "ABORT: $*" >&2; exit 1; }

capture_vulkaninfo() {  # $1 = arm label
  timeout 120 vulkaninfo --summary >"$OUT/vulkaninfo-$1.txt" 2>/dev/null
  timeout 180 vulkaninfo >"$OUT/vulkaninfo-full-$1.txt" 2>/dev/null
  echo "  vulkaninfo: $(grep -c . "$OUT/vulkaninfo-$1.txt" 2>/dev/null) summary lines, \
$(grep -c . "$OUT/vulkaninfo-full-$1.txt" 2>/dev/null) full lines"
}

run_probes() {  # $1 = arm label
  : >"$OUT/probes-$1.txt"
  for spec in $PERF_SPECS $COMPAT_SPECS; do
    IFS=: read -r p s c <<<"$spec"
    for i in $(seq 1 $SAMPLES); do
      python3 "$B/harness.py" "$p" "$s" "$c" 2>&1 \
        | grep -E "^  speed|samples|cross|stages|critical|GPU busy|wait states" \
        | sed "s/^/$p $s /" >>"$OUT/probes-$1.txt"
    done
  done
  echo "  probes: $(grep -c '^.*speed' "$OUT/probes-$1.txt" 2>/dev/null) samples recorded"
}

# ---------------------------------------------------------------- ARM 1: VENDOR
echo "=== ARM 1: VENDOR (pvrsrvkm) ==="
BOUND=$(readlink -f /sys/bus/platform/devices/1800000.gpu/driver 2>/dev/null | xargs basename)
[ "$BOUND" = "pvrsrvkm" ] || die "board is on '$BOUND', not pvrsrvkm - switch first"
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json
unset DISPLAY XAUTHORITY
capture_vulkaninfo vendor
run_probes vendor

# ---------------------------------------------------------------- ARM 2: OPEN
echo
echo "=== switching to the OPEN driver ==="
systemctl stop display-manager >/dev/null 2>&1; sleep 3
pkill -x weston 2>/dev/null; pkill -x Xwayland 2>/dev/null; pkill -x X 2>/dev/null; sleep 2
pgrep -x kwin_x11 >/dev/null && die "kwin still alive - refusing to unbind the GPU"
SW=/home/radxa/gpu-open-stack/switch-open.sh
[ -x "$SW" ] || die "switch script not found: $SW"
"$SW" >/dev/null 2>&1 || true   # exits non-zero on a redundant write; the driver NAME is the check
sleep 5
BOUND=$(readlink -f /sys/bus/platform/devices/1800000.gpu/driver 2>/dev/null | xargs basename)
[ "$BOUND" = "powervr" ] || die "switch to powervr failed (bound: $BOUND)"
echo "  bound: powervr   params: $(cat /sys/module/powervr/parameters/chain_frag 2>/dev/null)/$(cat /sys/module/powervr/parameters/combine_pr 2>/dev/null)"

echo "=== ARM 2: OPEN (powervr) ==="
export VK_ICD_FILENAMES=/home/radxa/pvr_gen_icd.json
export PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1
unset DISPLAY XAUTHORITY
capture_vulkaninfo open
run_probes open

# ---------------------------------------------------------------- CLEAN RETURN
echo
echo "=== returning to the VENDOR driver (unbind at refcount 0, then rmmod) ==="
echo 1800000.gpu > /sys/bus/platform/drivers/powervr/unbind 2>/dev/null
sleep 2
rmmod powervr 2>/dev/null
sleep 2
modprobe pvrsrvkm 2>/dev/null
sleep 4
BOUND=$(readlink -f /sys/bus/platform/devices/1800000.gpu/driver 2>/dev/null | xargs basename)
[ "$BOUND" = "pvrsrvkm" ] || echo "  WARNING: bound driver is '$BOUND', not pvrsrvkm"
systemctl start display-manager >/dev/null 2>&1; sleep 15
echo "  bound: $BOUND   kwin: $(pgrep -x kwin_x11 >/dev/null && echo ALIVE || echo absent)   \
warnings: $(dmesg 2>/dev/null | grep -icE 'WARNING|Call trace|pvr_remove|Oops')"

# ---------------------------------------------------------------- COMBINED TABLE
echo
python3 - "$OUT" <<'PY'
import re, sys, statistics as st, collections
OUT = sys.argv[1]
def parse(arm):
    d = collections.defaultdict(list)
    for line in open(f"{OUT}/probes-{arm}.txt", errors="replace"):
        m = re.match(r"(\S+) (\S+) +speed *: *([0-9.]+) ms/frame", line)
        if m: d[(m.group(1), m.group(2))].append(float(m.group(3)))
    return d
v, o = parse("vendor"), parse("open")
print("=== PERFORMANCE (median ms/frame; lower is better) ===")
print(f"  {'probe':<9}{'size':>6}{'n':>4}{'open':>10}{'vendor':>10}{'ratio':>8}   verdict")
for k in sorted(set(o) | set(v), key=lambda k: (k[0], int(k[1]))):
    if k not in o or k not in v: continue
    om, vm = st.median(o[k]), st.median(v[k])
    r = om / vm
    verdict = "OPEN WINS" if r < 0.985 else ("vendor wins" if r > 1.015 else "parity")
    print(f"  {k[0]:<9}{k[1]:>6}{len(o[k]):>4}{om:>10.3f}{vm:>10.3f}{r:>8.3f}   {verdict}")
print()
print("=== COMPATIBILITY (from vulkaninfo) ===")
for arm in ("vendor", "open"):
    try: t = open(f"{OUT}/vulkaninfo-{arm}.txt", errors="replace").read()
    except OSError: print(f"  {arm}: no data"); continue
    def g(pat):
        m = re.search(pat, t); return m.group(1).strip() if m else "-"
    full = open(f"{OUT}/vulkaninfo-full-{arm}.txt", errors="replace").read()
    exts = len(set(re.findall(r"^\s+(VK_[A-Z0-9_]+)\s*:", full, re.M)))
    print(f"  {arm:<7} apiVersion={g(r'apiVersion *= *([0-9.]+)'):<10} "
          f"driver={g(r'driverName *= *(.+)'):<34} conformance={g(r'conformanceVersion *= *([0-9.]+)'):<10} "
          f"extensions={exts}")
PY
echo
echo "reports: $OUT/"
