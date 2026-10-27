#!/usr/bin/env python3
"""cts_run2.py ICD_JSON CASELIST OUT_PREFIX [--stall S] [--hangs FILE] [--shard I/N] [--batch N] [--cpus LIST]

Like cts_run.py, but a hung test costs --stall seconds (default 120) instead of 900: deqp-vk is killed when its
log stops growing, the case it was in is marked 'Hang' and the run continues. Cases matching a regex in --hangs are
not run ('KnownHang'). --shard I/N takes every N-th case (balanced shards). Writes OUT.results (one 'case status'
per line) and OUT.progress (JSON, updated while running: done, total, hangs, elapsed)."""
import argparse, json, os, re, signal, subprocess, sys, time

DEQP_DIR = "/mnt/sdcard/cts/VK-GL-CTS/build/external/vulkancts/modules/vulkan"
ap = argparse.ArgumentParser()
ap.add_argument("icd"); ap.add_argument("caselist"); ap.add_argument("out")
ap.add_argument("--stall", type=int, default=120)
ap.add_argument("--hangs", default=os.path.expanduser("~/open-perf/cts/known_hangs.txt"))
ap.add_argument("--shard")
ap.add_argument("--batch", type=int, default=1500)
ap.add_argument("--cpus")
a = ap.parse_args()

cases = [l.strip() for l in open(a.caselist) if l.strip()]
if a.shard:
    i, n = map(int, a.shard.split("/"))
    cases = cases[i::n]
out = os.path.abspath(a.out)
hang_res = []
if a.hangs and os.path.exists(a.hangs):
    hang_res = [re.compile(l.strip()) for l in open(a.hangs) if l.strip() and not l.startswith("#")]
results = {c: "KnownHang" for c in cases if any(r.search(c) for r in hang_res)}
remaining = [c for c in cases if c not in results]
env = dict(os.environ, VK_ICD_FILENAMES=a.icd, PVR_I_WANT_A_BROKEN_VULKAN_DRIVER="1", MESA_SHADER_CACHE_DISABLE="1")
t_start = time.time()
log = out + ".qpa"


def progress(extra=None):
    d = {"done": len(results), "total": len(cases), "hangs": sum(1 for v in results.values() if v == "Hang"),
         "elapsed": round(time.time() - t_start), "icd": os.path.basename(a.icd)}
    if extra:
        d.update(extra)
    with open(out + ".progress", "w") as f:
        json.dump(d, f)


def parse(logfile):
    started, last, cur = set(), None, None
    if not os.path.exists(logfile):
        return last
    for line in open(logfile, errors="replace"):
        m = re.match(r"#beginTestCaseResult (\S+)", line)
        if m:
            cur = m.group(1); started.add(cur); last = cur
            continue
        m = re.search(r'<Result StatusCode="(\w+)"', line)
        if m and cur:
            results[cur] = m.group(1)
        if line.startswith("#endTestCaseResult") or line.startswith("#terminateTestCaseResult"):
            if line.startswith("#terminateTestCaseResult") and cur not in results:
                results[cur] = "Terminated"
            cur = None
    return last


attempt = 0
while remaining and attempt < 2000:
    attempt += 1
    batch = remaining[:a.batch]
    lst = out + ".batch.txt"
    open(lst, "w").write("\n".join(batch) + "\n")
    if os.path.exists(log):
        os.remove(log)
    cmd = ["./deqp-vk", "--deqp-caselist-file=" + lst, "--deqp-log-filename=" + log, "--deqp-log-images=disable",
           "--deqp-log-shader-sources=disable", "--deqp-log-flush=enable"]
    if a.cpus:
        cmd = ["taskset", "-c", a.cpus] + cmd
    p = subprocess.Popen(cmd, cwd=DEQP_DIR, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    last_size, last_change, hung, ticks = -1, time.time(), False, 0
    off, cnt = 0, 0  # bytes of the qpa log read so far, test cases begun in this batch
    while p.poll() is None:
        time.sleep(2)
        ticks += 1
        try:
            size = os.path.getsize(log)
        except OSError:
            size = 0
        if size != last_size:
            last_size, last_change = size, time.time()
        elif time.time() - last_change > a.stall:
            hung = True
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            break
        if ticks % 5 == 0:
            try:
                with open(log, "rb") as lf:
                    lf.seek(off)
                    data = lf.read()
                nl = data.rfind(b"\n") + 1
                cnt += data[:nl].count(b"#beginTestCaseResult")
                off += nl
            except OSError:
                pass
            progress({"in_batch": attempt, "stalled_for": round(time.time() - last_change),
                      "done": len(results) + max(cnt - 1, 0)})
    last = parse(log)
    if last and last not in results:
        results[last] = "Hang" if hung else "Crash"
    if not any(c in results for c in batch):
        results[batch[0]] = "Hang" if hung else "Crash"
    remaining = [c for c in remaining if c not in results]
    progress()
    print("attempt %d: %d done, %d remaining, %.0fs%s" % (attempt, len(results), len(remaining), time.time() - t_start,
                                                          " (stall kill)" if hung else ""), flush=True)

with open(out + ".results", "w") as f:
    for c in cases:
        f.write("%s %s\n" % (c, results.get(c, "NotRun")))
progress({"finished": True})
from collections import Counter
print(Counter(results.get(c, "NotRun") for c in cases))
