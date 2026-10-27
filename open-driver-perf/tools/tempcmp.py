#!/usr/bin/env python3
"""tempcmp.py LABEL1=FILE1 LABEL2=FILE2 ...  -- side-by-side shader stats (bytes/temps/groups) per scene from dump33.py summary outputs."""
import re, sys
arms = [a.split("=", 1) for a in sys.argv[1:]]
data = []
for lab, f in arms:
    d = {}
    for l in open(f, errors="replace"):
        m = re.match(r"(\d\d) (.{58}) (.*)", l)
        if m:
            d[m.group(1)] = (m.group(2).strip(), m.group(3).strip())
    data.append((lab, d))
keys = sorted(data[0][1])
for k in keys:
    print("%s %s" % (k, data[0][1][k][0][:44]))
    for lab, d in data:
        print("    %-9s %s" % (lab, d.get(k, ("", "-"))[1]))
