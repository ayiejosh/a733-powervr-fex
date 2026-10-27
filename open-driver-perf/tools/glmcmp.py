#!/usr/bin/env python3
"""glmcmp.py REF.txt NEW.txt [NEW2.txt ...]  -- per-scene FPS ratio table and geomean of NEW/REF from glmark2 output files.
Scene key = the text before "FPS:" on a line like "[build] use-vbo=false: FPS: 1044 FrameTime: 0.958 ms"."""
import math, re, sys


def load(p):
    d = {}
    for l in open(p, errors="replace"):
        m = re.match(r"\s*\[(.*?)\]\s*(.*?):?\s*FPS:\s*(\d+)", l)
        if m:
            d[m.group(1) + " " + m.group(2).strip()] = int(m.group(3))
    return d


ref = load(sys.argv[1])
news = [load(p) for p in sys.argv[2:]]
keys = [k for k in ref if all(k in n for n in news)]
print("%-52s %7s" % ("scene", "ref") + "".join(" %7s %6s" % ("new%d" % (i + 1), "ratio") for i in range(len(news))))
for k in keys:
    print("%-52s %7d" % (k[:52], ref[k]) + "".join(" %7d %5.0f%%" % (n[k], 100.0 * n[k] / ref[k]) for n in news))
print("scenes compared: %d of %d in ref" % (len(keys), len(ref)))
for i, n in enumerate(news):
    g = math.exp(sum(math.log(n[k] / ref[k]) for k in keys) / len(keys))
    print("geomean new%d/ref = %.3f" % (i + 1, g))
