#!/bin/sh
# Full matrix through the harness: every probe, every observable, one consolidated table.
# Usage: ./sweep.sh [--driver=open|vendor]
B=/mnt/sdcard/_REVIEW/emulation/trixie-prep/bench/pvr-vulkan
cd $B
echo "probe|size|speed_ms|mpix_s|M_inv_s|correct|critical_ms|jobs|user_ms|sys_ms|bpp"
for spec in "vkrender|2048|20" "vkrender|512|50" "vkheavy|2048|5" "cstp|64|200" "cstpf|64|200" "cstpi|64|200" "cstpi1|64|200" "cstpin|64|200"; do
  P=$(echo $spec | cut -d'|' -f1); S=$(echo $spec | cut -d'|' -f2); C=$(echo $spec | cut -d'|' -f3)
  python3 harness.py $P $S $C "$@" >/dev/null 2>&1
done
python3 - <<'PY'
import json
recs=[json.loads(l) for l in open("/mnt/sdcard/_REVIEW/emulation/trixie-prep/bench/pvr-vulkan/harness-log.jsonl")]
seen={}
for r in recs: seen[(r["probe"],r["size"])]=r
print(f"{'probe':<9}{'size':>6}{'ms/frame':>10}{'Mpix/s':>9}{'M inv/s':>9}{'correct':>8}{'critical':>10}{'jobs':>6}{'user ms':>9}{'sys ms':>8}{'bpp':>5}  driver")
for (p,s),r in seen.items():
    crit=max((j[1] for j in r.get("jobs") or []), default=None)
    cpu=r.get("cpu") or {}
    print(f"{p:<9}{s:>6}{str(r['ms_per_frame'] or ''):>10}{str(r['mpix_s'] or ''):>9}{str(r['thr_M_inv_s'] or ''):>9}"
          f"{str(r['correct'] or ''):>8}{str(round(crit,2) if crit else ''):>10}{len(r.get('jobs') or []):>6}"
          f"{str(cpu.get('user_ms','')):>9}{str(cpu.get('sys_ms','')):>8}{str(r.get('bpp') or ''):>5}  {r['driver']}")
PY
