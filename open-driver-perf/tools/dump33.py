#!/usr/bin/env python3
"""dump33.py TAG [OUTDIR]  -- run every glmark2 scene of the 33-scene set for 1 s on the open stack (bsession) with PCO_DEBUG_PRINT=fs,vs,stats and keep
the compile dump of each scene in OUTDIR (default /tmp/dump33), one file per scene; prints one summary line per scene (shader stats).
Scene specs come from the results file ~/open-perf/full-tx1.txt."""
import os, re, shlex, subprocess, sys
tag = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/dump33"
os.makedirs(out, exist_ok=True)
os.chdir(os.path.expanduser("~/open-perf"))
specs = []
for l in open("full-tx1.txt", errors="replace"):
    m = re.match(r"\[(.*?)\] (.*?): FPS", l)
    if m:
        name, opts = m.group(1), m.group(2)
        specs.append(name if opts == "<default>" else name + ":" + opts)
env = "MESA_LOADER_DRIVER_OVERRIDE=zink PCO_DEBUG_PRINT=fs,vs,stats MESA_SHADER_CACHE_DISABLE=1"
env += " " + os.environ.get("DUMP_EXTRA", "")
for i, s in enumerate(specs):
    f = "%s/%02d-%s.txt" % (out, i, re.sub(r"[^A-Za-z0-9]+", "_", s)[:60])
    cmd = "CLIENT_ENV=%s taskset -c 6-7 ./glbench.sh %s/icd-%s.json %s --off-screen -b %s >/dev/null 2>&1" % (shlex.quote(env), os.getcwd(), tag, f, shlex.quote(s + ":duration=1"))
    subprocess.run(cmd, shell=True)
    stats = re.findall(r"^(FS|VS) shader: (\d+) code size.*?(\d+) temp count.*?(\d+) inst group", open(f, errors="replace").read(), re.M)
    print("%02d %-58s %s" % (i, s[:58], " ".join("%s:%sB/%st/%sg" % x for x in stats)), flush=True)
