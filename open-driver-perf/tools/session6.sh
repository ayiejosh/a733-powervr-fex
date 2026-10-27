#!/bin/bash
# session6.sh: NIR scheduler threshold sweep on pc4 (PCO_SCHED_THRESHOLD): compile statistics of all 33 scenes per threshold, then A/Bs.
cd ~/open-perf
T=pc4; NC="MESA_SHADER_CACHE_DISABLE=1"
echo "## bsession start $(date +%T)"; tools/bsession.sh start 2>&1 | tail -n 2
[ "$(basename $(readlink /sys/bus/platform/devices/1800000.gpu/driver))" = powervr ] || { echo "NOT ON OPEN STACK"; echo "SESSION6 DONE"; exit 1; }
for thr in 8 12 16 24 32; do
  echo "## dump33 SCHED=$thr $(date +%T)"; DUMP_EXTRA="PCO_SCHED_THRESHOLD=$thr" python3 tools/dump33.py $T /tmp/d33-t$thr > /tmp/d33-t$thr.txt 2>&1
done
python3 tools/dump33.py $T /tmp/d33-base > /tmp/d33-base.txt 2>&1
python3 tools/tempcmp.py base=/tmp/d33-base.txt t8=/tmp/d33-t8.txt t12=/tmp/d33-t12.txt t16=/tmp/d33-t16.txt t24=/tmp/d33-t24.txt t32=/tmp/d33-t32.txt > /tmp/d33-sweep.txt 2>&1
echo "## sweep table (max FS temps/groups per scene)"
python3 - <<'PY'
import re
txt=open("/tmp/d33-sweep.txt").read().split("\n")
cur=None; by={}
for l in txt:
    if re.match(r"^\d\d ",l): cur=l[:48]
    m=re.match(r"^\s+(base|t8|t12|t16|t24|t32)\s+(.*)$",l)
    if m and cur: by.setdefault(cur,{})[m.group(1)]=m.group(2)
def mx(s):
    t=[int(x) for x in re.findall(r"FS:\d+B/(\d+)t",s)]; g=[int(x) for x in re.findall(r"FS:\d+B/\d+t/(\d+)g",s)]
    return "%d/%d"%(max(t),max(g)) if t else "-"
print("%-48s %8s %8s %8s %8s %8s %8s"%("scene","base","t8","t12","t16","t24","t32"))
for c,d in by.items(): print("%-48s"%c+"".join(" %8s"%mx(d.get(k,"")) for k in ("base","t8","t12","t16","t24","t32")))
PY
for thr in 12 16 32; do
  echo "## A/B SCHED=$thr $(date +%T)"; tools/ab.sh $T "$NC" "$NC PCO_SCHED_THRESHOLD=$thr" 3 2>&1 | tail -n 13
done
echo "## bsession stop $(date +%T)"; tools/bsession.sh stop 2>&1 | tail -n 2
echo "SESSION6 DONE $(date +%T)"
