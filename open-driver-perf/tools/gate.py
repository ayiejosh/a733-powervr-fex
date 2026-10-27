#!/usr/bin/env python3
"""gate.py --new TAG --lists a,b,c [--base TAG] [--workers N] [--recheck R] [--stall S]

CTS differential gate. For every list: runs build TAG (icd-TAG.json) in shards (cts_run2: stall watchdog, known hangs
from cts/known_hangs.txt), compares with the stored baseline in cts/baselines.map or, if there is none, runs build
--base too. Tasks run --workers at a time (largest first, no pinning). Every differing case is then re-run alone R times
on both builds, and only persistent differences are reported. Heartbeat for a quick look: cat /tmp/gate.status."""
import argparse, collections, concurrent.futures as cf, glob, json, math, os, subprocess, sys, threading, time

HOME = os.path.expanduser("~/open-perf")
CTS = HOME + "/cts"
ap = argparse.ArgumentParser()
ap.add_argument("--new", required=True)
ap.add_argument("--base")
ap.add_argument("--lists", required=True)
ap.add_argument("--workers", type=int, default=4)
ap.add_argument("--recheck", type=int, default=3)
ap.add_argument("--stall", type=int, default=120)
ap.add_argument("--shard-size", type=int, default=2500)
ap.add_argument("--shard-secs", type=int, default=150)
a = ap.parse_args()
os.chdir(CTS)
t0 = time.time()
baselines = {}
if os.path.exists("baselines.map"):
    for l in open("baselines.map"):
        if "=" in l and not l.startswith("#"):
            k, v = l.strip().split("=", 1)
            baselines[k] = v


def load(f):
    d = {}
    for x in open(f):
        p = x.rsplit(" ", 1)
        if len(p) == 2:
            d[p[0].strip()] = p[1].strip()
    return d


def run_task(lst, tag, shard, nshards):
    out = "gt_%s_%s_s%d" % (tag, lst, shard)
    cmd = [sys.executable, HOME + "/tools/cts_run2.py", "%s/icd-%s.json" % (HOME, tag), lst + ".txt", out,
           "--stall", str(a.stall), "--hangs", "known_hangs.txt"]
    if nshards > 1:
        cmd += ["--shard", "%d/%d" % (shard, nshards)]
    subprocess.run(cmd, stdout=open(out + ".log", "w"), stderr=subprocess.STDOUT)
    return out


try:
    cost = json.load(open("list_cost.json"))  # seconds per case of each list, learned from the earlier gates
except Exception:
    cost = {}
tasks = []  # (expected seconds, list, tag, shard, nshards, cases)
for lst in a.lists.split(","):
    n = sum(1 for _ in open(lst + ".txt"))
    sec = cost.get(lst, 0.035) * n
    tags = [a.new] + ([a.base] if (a.base and lst not in baselines) else [])
    if lst not in baselines and not a.base:
        sys.exit("no stored baseline for %s: pass --base TAG" % lst)
    for tag in tags:
        k = max(1, min(max(n // 40, 1), math.ceil(max(sec / a.shard_secs, n / a.shard_size))))
        for s in range(k):
            tasks.append((sec / k, lst, tag, s, k, n // k))
tasks.sort(reverse=True)
state = {"total": len(tasks), "done": 0, "running": set(), "est_left": sum(t[0] for t in tasks)}
secs = collections.defaultdict(list)
stop = False


def heartbeat():
    while not stop:
        prog = {"cases_done": 0, "cases_total": 0, "hangs": 0}
        for f in glob.glob("gt_*_s*.progress"):
            try:
                d = json.load(open(f))
                prog["cases_done"] += d.get("done", 0); prog["cases_total"] += d.get("total", 0); prog["hangs"] += d.get("hangs", 0)
            except Exception:
                pass
        el = time.time() - t0
        rate = prog["cases_done"] / el if el > 1 else 0
        eta = max(state["est_left"], 0) / a.workers / 60  # learned seconds per case, not the case rate
        pct = 100.0 * prog["cases_done"] / prog["cases_total"] if prog["cases_total"] else 0
        with open("/tmp/gate.line", "w") as f:
            f.write("gate: %d/%d tasks | %d/%d cases (%.0f%%) | %.1f cases/s | ETA %s min | hangs %d | %.1f min elapsed\n" % (
                state["done"], state["total"], prog["cases_done"], prog["cases_total"], pct, rate,
                "?" if eta is None else "%.0f" % eta, prog["hangs"], el / 60))
        with open("/tmp/gate.status", "w") as f:
            json.dump({"tasks_done": state["done"], "tasks_total": state["total"], "running": sorted(state["running"]),
                       "elapsed_min": round(el / 60, 1), "cases_per_s": round(rate, 1), "eta_min": None if eta is None else round(eta, 1),
                       **prog}, f)
        time.sleep(5)


for f in glob.glob("gt_*"):
    os.remove(f)
threading.Thread(target=heartbeat, daemon=True).start()


def work(t):
    est, lst, tag, s, k, cases = t
    name = "%s/%s/s%d" % (tag, lst, s)
    state["running"].add(name)
    t1 = time.time()
    out = run_task(lst, tag, s, k)
    if tag == a.new:
        secs[lst].append((time.time() - t1) / max(cases, 1))
    state["running"].discard(name)
    state["done"] += 1
    state["est_left"] -= est
    return t, out


outs = collections.defaultdict(dict)  # (lst, tag) -> merged results
with cf.ThreadPoolExecutor(max_workers=a.workers) as ex:
    for t, out in ex.map(work, tasks):
        est, lst, tag, s, k, cases = t
        if os.path.exists(out + ".results"):
            outs[(lst, tag)].update(load(out + ".results"))
stop = True
for lst, v in secs.items():
    cost[lst] = round(sorted(v)[len(v) // 2], 4)
json.dump(cost, open("list_cost.json", "w"))
print("all runs done in %.1f min" % ((time.time() - t0) / 60), flush=True)

diffs = []
for lst in a.lists.split(","):
    new = outs[(lst, a.new)]
    base = load(baselines[lst]) if lst in baselines else outs[(lst, a.base)]
    d = [(k, base.get(k), new.get(k)) for k in sorted(set(base) | set(new)) if base.get(k) != new.get(k)]
    print("CTS %-12s base=%d new=%d %s diffs=%d" % (lst, len(base), len(new), dict(collections.Counter(new.values())), len(d)), flush=True)
    diffs += [(lst, k, b, n) for k, b, n in d]

real = []
if diffs and a.recheck:
    print("rechecking %d differing cases alone, %dx on both builds ..." % (len(diffs), a.recheck), flush=True)
    base_tag = a.base or a.new
    cases = sorted(set(k for _, k, _, _ in diffs))
    open("gt_recheck.txt", "w").write("\n".join(cases) + "\n")
    res = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in range(a.recheck):
        for tag in (base_tag, a.new):
            out = "gt_rc_%s_%d" % (tag, r)
            subprocess.run([sys.executable, HOME + "/tools/cts_run2.py", "%s/icd-%s.json" % (HOME, tag), "gt_recheck.txt", out,
                            "--stall", str(a.stall)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for k, v in load(out + ".results").items():
                res[k][tag].append(v)
    for lst, k, b, n in diffs:
        bs, ns = res[k][base_tag], res[k][a.new]
        persistent = len(set(bs) | set(ns)) > 1 and set(bs) != set(ns)
        print("  %-12s %s: stored=%s new=%s | alone: base=%s new=%s -> %s" % (lst, k, b, n, bs, ns, "REAL DIFFERENCE" if persistent else "flaky under load"), flush=True)
        if persistent:
            real.append(k)
print("GATE %s: %d real differences, total %.1f min" % ("FAILED" if real else "PASSED", len(real), (time.time() - t0) / 60), flush=True)
sys.exit(1 if real else 0)
