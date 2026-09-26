#!/usr/bin/env python3
"""dissect-report.py — turn chrome-dissect.sh output into an attribution of the launch.

Answers, in order:
  1. How much wall time was real CPU, and how much was blocked or waiting on I/O?
  2. Which processes exist, when each started, and what each cost?
  3. Does a launch pay FEX init + relocation more than once (child re-exec vs fork)?
  4. How much memory pressure / zram swap did the launch generate?
  5. What does the launch look like second by second?

usage: ./dissect-report.py <label.tsv> [more.tsv ...]
"""
import sys
from collections import defaultdict

CLK = 100.0        # USER_HZ on this kernel
PAGE_KB = 4


def load(path):
    S, G, procs = [], [], defaultdict(list)
    with open(path) as fh:
        for line in fh:
            f = line.rstrip("\n").split("\t")
            if f[0] == "S" and len(f) >= 10:
                S.append(tuple(int(x) for x in f[1:10]))
            elif f[0] == "G" and len(f) >= 8:
                G.append(tuple(int(x) for x in f[1:8]))
            elif f[0] == "P" and len(f) >= 13:
                procs[int(f[2])].append({
                    "t": int(f[1]), "ppid": int(f[3]), "comm": f[4], "state": f[5],
                    "ut": int(f[6]), "st": int(f[7]), "minf": int(f[8]),
                    "majf": int(f[9]), "rss": int(f[10]) * PAGE_KB,
                    "rd": int(f[11]), "wr": int(f[12]),
                })
    return S, G, procs


def report(path):
    S, G, procs = load(path)
    print(f"\n{'='*84}\n{path}\n{'='*84}")
    if not S:
        print("  no samples")
        return

    wall = S[-1][0] / 1000.0
    print(f"  wall (sampled): {wall:.1f} s over {len(S)} ticks")

    # ---------- system-wide CPU from the aggregate ----------
    a, b = S[0], S[-1]
    busy = ((b[1]-a[1]) + (b[2]-a[2]) + (b[3]-a[3]) + (b[6]-a[6]) + (b[7]-a[7]) + (b[8]-a[8])) / CLK
    idle = (b[4]-a[4]) / CLK
    iowait = (b[5]-a[5]) / CLK
    total = busy + idle + iowait
    print(f"\n  system CPU: busy {busy:.1f}s ({100*busy/total:.0f}%)  "
          f"iowait {iowait:.1f}s ({100*iowait/total:.0f}%)  idle {idle:.1f}s ({100*idle/total:.0f}%)")
    print(f"  effective parallelism over the window: {busy/wall:.2f} of 8 cores")

    # ---------- per-process deltas inside the window ----------
    rows = []
    for pid, s in procs.items():
        s.sort(key=lambda r: r["t"])
        p, q = s[0], s[-1]
        cpu = ((q["ut"]-p["ut"]) + (q["st"]-p["st"])) / CLK
        rows.append({
            "pid": pid, "comm": q["comm"], "ppid": q["ppid"], "first": p["t"], "last": q["t"],
            "cpu": cpu, "rss": max(r["rss"] for r in s)//1024,
            "minf": q["minf"]-p["minf"], "majf": q["majf"]-p["majf"],
            "rd": (q["rd"]-p["rd"])/1e6, "wr": (q["wr"]-p["wr"])/1e6, "n": len(s),
        })
    rows.sort(key=lambda r: -r["cpu"])
    sub_cpu = sum(r["cpu"] for r in rows)
    ambient = busy - sub_cpu

    print(f"\n  subtree: {len(rows)} processes, {sub_cpu:.1f} s CPU "
          f"({100*sub_cpu/busy if busy else 0:.0f}% of system busy)")
    print(f"  ambient (everything else on the board): {ambient:.1f} s CPU "
          f"-> {100*ambient/busy if busy else 0:.0f}% of system busy")

    # ---------- group by binary: how much does each guest binary cost ----------
    byname = defaultdict(lambda: [0, 0])
    for r in rows:
        byname[r["comm"]][0] += r["cpu"]
        byname[r["comm"]][1] += 1
    print("\n  CPU by process name (subtree):")
    for name, (cpu, cnt) in sorted(byname.items(), key=lambda kv: -kv[1][0])[:10]:
        print(f"    {cpu:>7.2f} s  x{cnt:<3}  {name[:40]}")

    print(f"\n  {'pid':>7} {'start':>7} {'end':>7} {'cpu_s':>7} {'rss_MB':>7} "
          f"{'minflt':>8} {'majflt':>7} {'rd_MB':>7} {'wr_MB':>7}  comm")
    for r in rows[:20]:
        print(f"  {r['pid']:>7} {r['first']/1000:>6.2f}s {r['last']/1000:>6.2f}s "
              f"{r['cpu']:>7.2f} {r['rss']:>7} {r['minf']:>8} {r['majf']:>7} "
              f"{r['rd']:>7.1f} {r['wr']:>7.1f}  {r['comm'][:26]}")

    # ---------- spawn timeline: is init paid once or N times? ----------
    spawns = sorted([r for r in rows if r["cpu"] > 0.02], key=lambda r: r["first"])
    print("\n  spawn timeline:")
    for r in spawns[:22]:
        parent = next((x["comm"] for x in rows if x["pid"] == r["ppid"]), f"pid{r['ppid']}")
        print(f"    t={r['first']/1000:>6.2f}s -> {r['last']/1000:>6.2f}s  pid={r['pid']:<7} "
              f"cpu={r['cpu']:>6.2f}s rss={r['rss']:>5}MB  {r['comm'][:22]} (parent {parent[:18]})")

    peak = 0
    for t in range(0, int(wall*1000)+1, 1000):
        n = sum(1 for s in procs.values() if s[0]["t"] <= t <= s[-1]["t"])
        peak = max(peak, n)
    print(f"  peak concurrent processes in subtree: {peak}")

    # ---------- per-second histogram ----------
    buckets = defaultdict(float)
    bsys = defaultdict(float)
    biow = defaultdict(float)
    bwall = defaultdict(float)
    btop = defaultdict(lambda: defaultdict(float))
    for i in range(len(S)-1):
        p, q = S[i], S[i+1]
        dt = (q[0]-p[0])/1000.0
        if dt <= 0:
            continue
        k = q[0]//1000
        bwall[k] += dt
        bsys[k] += ((q[1]-p[1])+(q[2]-p[2])+(q[3]-p[3])+(q[6]-p[6])+(q[7]-p[7])+(q[8]-p[8]))/CLK
        biow[k] += (q[5]-p[5])/CLK
    for pid, s in procs.items():
        s.sort(key=lambda r: r["t"])
        for p, q in zip(s, s[1:]):
            d = ((q["ut"]-p["ut"]) + (q["st"]-p["st"]))/CLK
            if d > 0:
                k = q["t"]//1000
                buckets[k] += d
                btop[k][q["comm"]] += d
    print(f"\n  {'t(s)':>5} {'syscpu':>7} {'subtree':>8} {'iowait':>7}  top subtree consumers")
    for k in sorted(bwall):
        top = sorted(btop[k].items(), key=lambda kv: -kv[1])[:3]
        tops = "  ".join(f"{n[:14]}={c:.2f}" for n, c in top)
        print(f"  {k:>5} {bsys[k]:>7.2f} {buckets[k]:>8.2f} {biow[k]:>7.2f}  {tops}")

    # ---------- blocked / idle analysis ----------
    gaps = []
    for i in range(len(S)-1):
        p, q = S[i], S[i+1]
        d = ((q[1]-p[1])+(q[2]-p[2])+(q[3]-p[3])+(q[6]-p[6])+(q[7]-p[7])+(q[8]-p[8]))
        if d == 0:
            gaps.append((p[0], q[0]))
    merged = []
    for x, y in gaps:
        if merged and x <= merged[-1][1] + 400:
            merged[-1][1] = y
        else:
            merged.append([x, y])
    blocked = sum(y-x for x, y in merged)/1000.0
    print(f"\n  blocked wall (no CPU advancing anywhere): {blocked:.1f} s "
          f"({100*blocked/wall:.0f}% of wall) in {len(merged)} span(s)")
    for x, y in sorted(merged, key=lambda g: -(g[1]-g[0]))[:6]:
        print(f"    {(y-x)/1000:>6.1f}s  t={x/1000:>6.2f}s .. {y/1000:>6.2f}s")

    # ---------- memory ----------
    if G:
        g0, g1 = G[0], G[-1]
        print("\n  memory pressure during launch:")
        print(f"    MemFree  {g0[1]/1024:>6.0f} MB -> {g1[1]/1024:>6.0f} MB "
              f"(min {min(g[1] for g in G)/1024:.0f} MB)")
        print(f"    Cached   {g0[2]/1024:>6.0f} MB -> {g1[2]/1024:>6.0f} MB")
        print(f"    pswpin   {4*(g1[4]-g0[4])/1024:>6.0f} MB swapped IN   "
              f"pswpout {4*(g1[5]-g0[5])/1024:>6.0f} MB swapped OUT")
        print(f"    pgmajfault {g1[6]-g0[6]:>6} (system-wide), "
              f"subtree majflt {sum(r['majf'] for r in rows)}")
        print(f"    subtree read from disk {sum(r['rd'] for r in rows):.0f} MB, "
              f"written {sum(r['wr'] for r in rows):.0f} MB")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        report(p)
