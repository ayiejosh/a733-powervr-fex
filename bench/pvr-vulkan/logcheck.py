#!/usr/bin/env python3
"""Which harness-log records are safe to read?

The per-stage breakdown comes from fence pairing, which degrades above a few thousand jobs and then
silently emits impossible data - about 6% of the log. harness.py now warns at run time; this filters
the history. Two signatures, both impossible for a ONE-FRAME window:

  * two stages equal to within 1%   (independent stages cannot match that closely)
  * a stage longer than its frame

  ./logcheck.py            summary + the bad records
  ./logcheck.py --clean    print only the trustworthy records as JSON lines
"""
import json, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))


def bad_reasons(r):
    j = r.get("jobs") or []
    out = []
    if len(j) >= 2:
        ds = sorted((d for _, d in j), reverse=True)
        for i in range(len(ds) - 1):
            if ds[i + 1] > 0 and abs(ds[i] - ds[i + 1]) / ds[i + 1] < 0.01:
                out.append(f"two stages within 1% ({ds[i]:.3f} vs {ds[i+1]:.3f})")
                break
    ms = r.get("ms_per_frame")
    if j and ms:
        mx = max(d for _, d in j)
        if mx > ms * 1.5:
            out.append(f"stage {mx:.1f} ms > frame {ms:.1f} ms")
    return out


def main():
    recs = [json.loads(l) for l in open(f"{HERE}/harness-log.jsonl") if l.strip()]
    bad = [(r, bad_reasons(r)) for r in recs]
    bad = [(r, why) for r, why in bad if why]
    if "--clean" in sys.argv:
        for r in recs:
            if not bad_reasons(r):
                print(json.dumps(r))
        return
    withjobs = sum(1 for r in recs if r.get("jobs"))
    print(f"  records: {len(recs)}   with jobs: {withjobs}   suspicious: {len(bad)}")
    print(f"  trustworthy: {len(recs) - len(bad)}   ({100*(len(recs)-len(bad))/max(len(recs),1):.1f}%)")
    if bad:
        print("\n  the bad ones (do not read their 'jobs'):")
        for r, why in bad[:12]:
            print(f"    {r['driver']:<9} {r['probe']:<9} {str(r['size']):>5}  {why[0]}")
    print("\n  vetted sets to prefer: rounds 256 (stability), 257 (size sweep), and S25 (both arms).")


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:   # ponytail: piping into head is normal, not an error
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
