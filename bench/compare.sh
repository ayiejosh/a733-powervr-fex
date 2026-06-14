#!/usr/bin/env bash
# bench/compare.sh — run the full suite now and show a human-readable diff vs the
# committed baseline (bench/baseline.txt as it is on git main). Make a change, run
# this, and you immediately see what moved.
#
#   bench/compare.sh            # vs the baseline committed in git (origin/main -> HEAD -> file)
#   bench/compare.sh --hw       # also include the CPU/RAM/UFS hardware baselines
#   BASELINE=path bench/compare.sh   # diff against an explicit baseline file instead
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; cd "$HERE"

# --- resolve the baseline (what's on git main) ---
BSRC=""; BFILE=/tmp/_bench_base.txt
if [ -n "${BASELINE:-}" ] && [ -f "${BASELINE}" ]; then cp "$BASELINE" "$BFILE"; BSRC="$BASELINE"
elif git -C .. rev-parse --git-dir >/dev/null 2>&1; then
  if   git -C .. show origin/main:bench/baseline.txt >"$BFILE" 2>/dev/null; then BSRC="git origin/main"
  elif git -C .. show main:bench/baseline.txt        >"$BFILE" 2>/dev/null; then BSRC="git main"
  elif git -C .. show HEAD:bench/baseline.txt         >"$BFILE" 2>/dev/null; then BSRC="git HEAD"
  fi
fi
[ -z "$BSRC" ] && { cp baseline.txt "$BFILE" 2>/dev/null && BSRC="file:baseline.txt"; }
[ -s "$BFILE" ] || { echo "No baseline found (need bench/baseline.txt in git or BASELINE=...)"; exit 1; }

echo ">>> baseline = $BSRC ; running current suite (~2-3 min)..." >&2
CFILE=/tmp/_bench_cur.txt
bash run.sh ${1:-} > "$CFILE" 2>/dev/null

python3 - "$BFILE" "$CFILE" "$BSRC" <<'PY'
import sys, re
bfile, cfile, bsrc = sys.argv[1], sys.argv[2], sys.argv[3]
def load(fn):
    d={}
    for ln in open(fn):
        ln=ln.split('#',1)[0].strip()
        if not ln or ln.startswith('#') or ':' not in ln: continue
        k,v=ln.split(':',1); k=k.strip(); v=v.strip()
        m=re.match(r'([-+]?\d+\.?\d*)\s*(\S*)',v)
        if m: d[k]=(float(m.group(1)), m.group(2))
    return d
B,C=load(bfile),load(cfile)
def better(k,delta):
    lower_better = ('nsop' in k) or ('nsiter' in k) or k.endswith('.ns')
    if abs(delta)<0.0001: return 0
    up = delta>0
    return (1 if (up ^ lower_better) else -1)   # +1 improve, -1 regress
def is_signal(k): return k.startswith('ratio.') or k.startswith('gpu.')
def is_info(k):   return k.startswith('info.')
def band_for(k):
    if k.startswith('gpu.') or k.startswith('cpu.'): return 5    # stable / load-independent
    return 25   # ratios (tiny stock denom) + absolutes wobble ~this much run-to-run on this box
GRN='\033[32m'; RED='\033[31m'; DIM='\033[2m'; BLD='\033[1m'; RST='\033[0m'
def row(k):
    b=B.get(k); c=C.get(k)
    bs = (f"{b[0]:g}{b[1]}" if b else "—")
    cs = (f"{c[0]:g}{c[1]}" if c else "—")
    if b and c and b[0]:
        d=(c[0]-b[0])/b[0]*100; sign=better(k,c[0]-b[0])
        # noise band: 3% for signal, 20% for absolutes
        band = band_for(k)
        if abs(d)<band: arrow=f"{DIM}≈{RST}"
        elif sign>0:    arrow=f"{GRN}▲ better{RST}"
        else:           arrow=f"{RED}▼ worse{RST}"
        ds=f"{d:+.1f}%"
    else:
        ds = ("NEW" if c and not b else "GONE" if b and not c else ""); arrow=""
    print(f"  {k:<38} {bs:>12} {cs:>12}   {ds:>8}  {arrow}")
allk=list(dict.fromkeys(list(B)+list(C)))
sig=[k for k in allk if is_signal(k)]; info=[k for k in allk if is_info(k)]
absol=[k for k in allk if not is_signal(k) and not is_info(k)]
hdr=f"{'metric':<38} {'baseline':>12} {'current':>12}   {'Δ':>8}"
print(f"\n{BLD}=== A733 benchmark — current vs baseline ({bsrc}) ==={RST}")
print(f"\n{BLD}SIGNAL  (reliable — judge changes here){RST}")
print(f"  {hdr}")
for k in sig: row(k)
print(f"\n{BLD}ABSOLUTES  (load-sensitive ±20% — informational){RST}")
print(f"  {hdr}")
for k in absol: row(k)
if info:
    print(f"\n{BLD}INFO{RST}")
    for k in info: row(k)
# verdict on signal metrics only
regr=[k for k in sig if B.get(k) and C.get(k) and B[k][0] and (lambda d:abs(d)>=band_for(k) and better(k,C[k][0]-B[k][0])<0)((C[k][0]-B[k][0])/B[k][0]*100)]
impr=[k for k in sig if B.get(k) and C.get(k) and B[k][0] and (lambda d:abs(d)>=band_for(k) and better(k,C[k][0]-B[k][0])>0)((C[k][0]-B[k][0])/B[k][0]*100)]
print(f"\n{BLD}VERDICT (signal metrics):{RST} ", end="")
if regr: print(f"{RED}REGRESSION in: {', '.join(regr)}{RST}")
elif impr: print(f"{GRN}improvement in: {', '.join(impr)}{RST} (no regressions)")
else: print(f"{DIM}no change beyond noise{RST}")
PY
