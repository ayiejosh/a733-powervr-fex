#!/bin/sh
# Full A/B in ONE run: close the desktop, finish arm A, arm the other driver and
# finish arm B, reopen the desktop, print the diff.
#
#   ./ab.sh [probe size count ...]     default: the standard matrix
#
# The desktop is SDDM (display-manager.service). It is stopped before switching and
# started again by a trap, so it comes back even if this script is interrupted.
#
# DO NOT PIPE THIS SCRIPT. `./ab.sh | head` kills it with SIGPIPE while the desktop is down
# and the driver is mid-switch, leaving the board unbound and unmeasured - and that is how
# this board took a fourth kernel Oops. Redirect to a file instead.
B=/mnt/sdcard/_REVIEW/emulation/trixie-prep/bench/pvr-vulkan
LOG=$B/ab-$(date +%H%M%S).jsonl
DM=display-manager

reopen() {
  pgrep -x kwin_x11 >/dev/null || pgrep -x weston >/dev/null || {
    echo "  reopening the desktop ($DM)..."
    sudo systemctl start $DM >/dev/null 2>&1
    sleep 6
  }
}
trap 'reopen; echo "  (trap) desktop restored"' EXIT INT TERM

echo "=== closing the desktop ==="
sudo systemctl stop $DM >/dev/null 2>&1
sleep 3
sudo pkill -x weston 2>/dev/null; sudo pkill -x Xwayland 2>/dev/null
sleep 2
for p in kwin_x11 kwin_wayland weston Xwayland; do
  pgrep -x $p >/dev/null && { echo "  ABORT: $p still alive"; exit 1; }
done
echo "  desktop closed, no compositor running"

SPECS="$*"
[ -z "$SPECS" ] && SPECS="vkrender:2048:20 vkrender:512:50 vkheavy:2048:5 cstp:64:200 cstpf:64:200 cstpi:64:200"

for DRV in open vendor; do
  echo
  echo "=== ARM $DRV ==="
  # ponytail: verify the switch instead of trusting it. Silencing this script's output and
  # ignoring its exit code is what let a failed switch proceed: the probes then ran against
  # NO driver and the board crashed. Check the exit code AND that the expected name is bound,
  # and refuse to measure an arm that did not come up.
  case $DRV in
    open)   WANT=powervr;  SW=/home/radxa/gpu-open-stack/switch-open.sh ;;
    vendor) WANT=pvrsrvkm; SW=/home/radxa/gpu-open-stack/switch-vendor.sh ;;
  esac
  if ! sudo $SW; then
    echo "  ABORT: $SW failed (exit $?) - not measuring the $DRV arm"; exit 1
  fi
  sleep 4
  BOUND=$(ls -l /sys/bus/platform/devices/1800000.gpu/driver 2>/dev/null | sed 's/.*-> //' | xargs basename 2>/dev/null)
  echo "  bound driver: $BOUND (want $WANT)"
  if [ "$BOUND" != "$WANT" ]; then
    echo "  ABORT: expected $WANT but got '${BOUND:-nothing}' - restoring and not measuring"
    /home/radxa/gpu-open-stack/switch-vendor.sh >/dev/null 2>&1 || true
    exit 1
  fi
  for spec in $SPECS; do
    P=$(echo $spec | cut -d: -f1); S=$(echo $spec | cut -d: -f2); C=$(echo $spec | cut -d: -f3)
    python3 $B/harness.py $P $S $C --driver=$DRV >/dev/null 2>&1
  done
done

echo
echo "=== reopening the desktop ==="
reopen
echo "  done"

echo
echo "=== A/B DIFF (from this run) ==="
B=$B LOG=$LOG python3 - <<'PY'
import json, os, collections
B=os.environ["B"]; LOG=os.environ["LOG"]
# harness-log.jsonl is shared; take the newest record per (driver,probe,size)
recs=[json.loads(l) for l in open(f"{B}/harness-log.jsonl")]
seen={}
for r in recs: seen[(r["driver"],r["probe"],r["size"])]=r
print(f"{'probe':<9}{'size':>6} {'open':>26} {'vendor':>26}  ratio")
keys=sorted({(p,s) for (d,p,s) in seen})
for p,s in keys:
    o=seen.get(("powervr",p,s)); v=seen.get(("pvrsrvkm",p,s))
    def fmt(r):
        if not r: return " " * 26
        if r.get("ms_per_frame"): return f"{r['ms_per_frame']:>9.3f} ms/frame      "
        if r.get("thr_M_inv_s"):  return f"{r['thr_M_inv_s']:>9.1f} M inv/s       "
        return " " * 26
    ratio=""
    if o and v and o.get("ms_per_frame") and v.get("ms_per_frame"):
        ratio=f"{o['ms_per_frame']/v['ms_per_frame']:.2f}x"
    elif o and v and o.get("thr_M_inv_s") and v.get("thr_M_inv_s"):
        ratio=f"{v['thr_M_inv_s']/o['thr_M_inv_s']:.2f}x"
    print(f"{p:<9}{s:>6} {fmt(o)} {fmt(v)}  {ratio}")
PY
