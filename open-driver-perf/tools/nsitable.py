#!/usr/bin/env python3
"""nsitable.py LABEL DELAY_S LAUNCH_TEMPLATE [scene ...]  -- for each scene run LAUNCH_TEMPLATE ({S} = the -b scene spec with duration), open
the NSI PMU window DELAY_S seconds in, and print FPS plus GPU/DE/total read and write bandwidth per frame and the GPU read latency.
PMU unit assumed KB/s (calibrated against the display engine); only pmu_timer is written."""
import re, shlex, subprocess, sys, time
P = "/sys/devices/platform/soc@3000000/2020000.nsi-controller/nsi-pmu/hwmon0/"
label, delay, tmpl = sys.argv[1], float(sys.argv[2]), sys.argv[3]
scenes = sys.argv[4:] or ["loop:fragment-steps=5:fragment-uniform=false", "function:fragment-complexity=medium:fragment-steps=5", "ideas:speed=duration",
    "bump:bump-render=normals", "shading:shading=phong", "texture:texture-filter=mipmap", "jellyfish", "effect2d:kernel=0,1,0;1,-4,1;0,1,0;",
    "desktop:effect=shadow:windows=4", "shadow"]
names = subprocess.run("sudo -n cat " + P + "available_pmu", shell=True, capture_output=True, text=True).stdout.split()
ix = {n: i for i, n in enumerate(names)}
def rd(f): return list(map(int, subprocess.run("sudo -n cat " + P + f, shell=True, capture_output=True, text=True).stdout.split()))
print("%-9s %-34s %5s | %8s %8s | %8s %8s | %6s | %7s %7s" % ("stack", "scene", "FPS", "gpu rdMB", "gpu wrMB", "de rd", "tot rd+wr", "lat_rd", "gpu rd/s", "wr/s"))
for s in scenes:
    spec = s + ":duration=" + str(int(delay + 6))
    p = subprocess.Popen(tmpl.replace("{S}", shlex.quote(spec)), shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(delay)
    subprocess.run("echo 1000000 | sudo -n tee " + P + "pmu_timer >/dev/null", shell=True)
    time.sleep(1.5)
    r, w, l = rd("pmu_bandwidth_rd"), rd("pmu_bandwidth_wr"), rd("pmu_latency_rd")
    out, _ = p.communicate()
    subprocess.run("echo 0 | sudo -n tee " + P + "pmu_timer >/dev/null", shell=True)
    import os
    if os.environ.get('NSI_OUT') and os.path.exists(os.environ['NSI_OUT']):
        out += open(os.environ['NSI_OUT'], errors='replace').read()
    m = re.findall(r"FPS: (\d+)", out)
    f = int(m[-1]) if m else 0
    g = ix["gpu"]
    pf = lambda v: (v / 1000.0 / f) if f else 0   # KB/s -> MB per frame
    print("%-9s %-34s %5d | %8.2f %8.2f | %8.0f %8.2f | %6d | %7d %7d" % (label, s[:34], f, pf(r[g]), pf(w[g]), r[ix["de"]], pf(r[ix["total"]] + w[ix["total"]]), l[g], r[g], w[g]), flush=True)
