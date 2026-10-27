#!/usr/bin/env python3
"""sysprobe.py [--window S0:S1] -- CMD...   Run CMD (a glmark2 launcher), and between S0 and S1 seconds after it starts record deltas of:
scheduler (context switches, interrupts, softirqs), per-CPU idle-state entries/time, interrupt lines, CPU time per process/thread (top
consumers), temperatures and CPU frequencies. Prints per-second rates and, when the output shows 'FPS: N', per-frame values.
Read-only on the system; the only load is the benchmark that CMD starts."""
import glob, os, re, subprocess, sys, time

a = sys.argv[1:]
s0, s1 = 3.0, 7.0
if a and a[0] == "--window":
    s0, s1 = map(float, a[1].split(":")); a = a[2:]
if a and a[0] == "--":
    a = a[1:]
cmd = a

def read(p):
    try:
        return open(p).read()
    except OSError:
        return ""

def snap():
    d = {"t": time.time()}
    st = read("/proc/stat")
    d["ctxt"] = int(re.search(r"^ctxt (\d+)", st, re.M).group(1))
    d["intr"] = int(re.search(r"^intr (\d+)", st, re.M).group(1))
    d["softirq"] = int(re.search(r"^softirq (\d+)", st, re.M).group(1))
    d["cpu"] = {}
    for m in re.finditer(r"^(cpu\d+) (.*)$", st, re.M):
        v = list(map(int, m.group(2).split()))
        d["cpu"][m.group(1)] = (sum(v) - v[3] - v[4], v[3] + v[4])  # busy, idle jiffies
    idle = {}
    for c in glob.glob("/sys/devices/system/cpu/cpu[0-9]*"):
        n = os.path.basename(c)
        for s in sorted(glob.glob(c + "/cpuidle/state*")):
            idle[(n, os.path.basename(s))] = (int(read(s + "/usage") or 0), int(read(s + "/time") or 0), read(s + "/name").strip())
    d["idle"] = idle
    irqs = {}
    for l in read("/proc/interrupts").splitlines()[1:]:
        p = l.split()
        if not p or not p[0].rstrip(":").isdigit() and not p[0].rstrip(":").isalpha():
            continue
        cnt = 0
        k = 1
        while k < len(p) and p[k].isdigit():
            cnt += int(p[k]); k += 1
        irqs[p[0].rstrip(":") + " " + " ".join(p[k:k + 3])] = cnt
    d["irq"] = irqs
    procs = {}
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        for tid in os.listdir("/proc/%s/task" % pid) if os.path.isdir("/proc/%s/task" % pid) else []:
            s = read("/proc/%s/task/%s/stat" % (pid, tid))
            m = re.match(r"\d+ \((.*)\) (.*)", s, re.S)
            if not m:
                continue
            f = m.group(2).split()
            procs[(pid, tid)] = (m.group(1), int(f[11]) + int(f[12]), int(f[36]) if len(f) > 36 else -1)  # comm, utime+stime, cpu
    d["procs"] = procs
    d["temp"] = {os.path.basename(z): (read(z + "/type").strip(), int(read(z + "/temp") or 0) / 1000.0) for z in glob.glob("/sys/class/thermal/thermal_zone*")}
    d["freq"] = {c: int(read("/sys/devices/system/cpu/%s/cpufreq/scaling_cur_freq" % c) or 0) // 1000 for c in ("cpu0", "cpu6")}
    return d

p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
t0 = time.time()
time.sleep(max(0, s0 - (time.time() - t0)))
A = snap()
time.sleep(max(0, s1 - (time.time() - t0)))
B = snap()
out, _ = p.communicate()
if os.environ.get('SP_OUT') and os.path.exists(os.environ['SP_OUT']):
    out += open(os.environ['SP_OUT'], errors='replace').read()
fps = [int(x) for x in re.findall(r"FPS: (\d+)", out)]
f = fps[-1] if fps else 0
dt = B["t"] - A["t"]
frames = f * dt
print("window %.1f s, FPS %s -> about %d frames" % (dt, f, frames))
def rate(x):
    return "%.0f/s" % (x / dt) + (" (%.1f/frame)" % (x / frames) if frames else "")
print("context switches", rate(B["ctxt"] - A["ctxt"]), "| interrupts", rate(B["intr"] - A["intr"]), "| softirqs", rate(B["softirq"] - A["softirq"]))
print("temps C:", ", ".join("%s=%.0f" % (v[0], v[1]) for k, v in sorted(B["temp"].items())), "| freq MHz:", B["freq"])
print("-- busiest interrupt lines")
irq = sorted(((B["irq"].get(k, 0) - A["irq"].get(k, 0), k) for k in B["irq"]), reverse=True)[:8]
for n, k in irq:
    if n > 0:
        print("  %-48s %s" % (k[:48], rate(n)))
print("-- cpu busy % (window)")
print("  " + " ".join("%s=%.0f" % (c, 100.0 * (B["cpu"][c][0] - A["cpu"][c][0]) / max(1, (B["cpu"][c][0] - A["cpu"][c][0]) + (B["cpu"][c][1] - A["cpu"][c][1]))) for c in sorted(B["cpu"], key=lambda x: int(x[3:]))))
print("-- cpuidle entries per second per state (cpu0, cpu3, cpu6, cpu7)")
for cpu in ("cpu0", "cpu3", "cpu6", "cpu7"):
    row = []
    for (c, s), (u, t, nm) in sorted(B["idle"].items()):
        if c == cpu:
            u0, t0_, _ = A["idle"].get((c, s), (0, 0, ""))
            row.append("%s:%.0f/s,%.0f%%" % (nm or s, (u - u0) / dt, 100.0 * (t - t0_) / 1e6 / dt))
    print("  %s %s" % (cpu, "  ".join(row)))
print("-- CPU time per thread (top 14, jiffies=10 ms; share of one core)")
pr = []
for k, (comm, j, cpu) in B["procs"].items():
    j0 = A["procs"].get(k, (comm, j, cpu))[1]
    if j - j0 > 0:
        pr.append((j - j0, comm, k[0], k[1]))
for n, comm, pid, tid in sorted(pr, reverse=True)[:14]:
    print("  %-22s pid %-6s tid %-6s %5.1f%% of a core" % (comm[:22], pid, tid, n * 10.0 / 1000 / dt * 100))
