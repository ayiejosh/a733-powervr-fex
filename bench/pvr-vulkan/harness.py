#!/usr/bin/env python3
"""One run, everything observed: speed, per-stage job times, CPU, correctness.
Works under either driver - it reads the tracepoints the bound driver actually emits.

  ./harness.py <probe> <size> <secs> [env...]

Emits a formatted report and appends one JSON line to harness-log.jsonl.
"""
import json, os, re, resource, subprocess, sys, time

TRACE = "/sys/kernel/debug/tracing"
BENCH = os.path.dirname(os.path.abspath(__file__))

OPEN_ICD = ("/home/radxa/pvr_gen_icd.json",
            "PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1")
VENDOR_ICD = ("/usr/share/vulkan/icd.d/img_icd.json", None)


def sh(cmd, check=False):
    return subprocess.run(["sudo", "sh", "-c", cmd], capture_output=True,
                          text=True, check=check)


def driver():
    p = "/sys/bus/platform/devices/1800000.gpu/driver"
    try:
        d = os.path.basename(os.path.realpath(p))
    except OSError:
        return "none"
    return d


def cpu_of(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            v = f.read().split()
        return int(v[13]), int(v[14])          # utime, stime in ticks
    except OSError:
        return None


def trace_on(events):
    sh(f"cd {TRACE}; echo 0 > tracing_on; echo > trace")
    for e in events:
        sh(f"for f in {TRACE}/events/{e}/*/enable; do echo 1 > $f 2>/dev/null; done")
    sh(f"echo 1 > {TRACE}/tracing_on")


def trace_off():
    sh(f"echo 0 > {TRACE}/tracing_on")
    return sh(f"cat {TRACE}/trace").stdout


# ---- stage parsing: whichever driver is bound decides which tracepoints exist ----
def parse_jobs(txt):
    """Return [(name, dur_ms)] for open (drm_sched) or vendor (pvr_fence)."""
    runs, procs = {}, []
    for l in txt.splitlines():
        m = re.search(r"(\d+\.\d+): drm_run_job: entity=(\w+), id=(\d+), fence=(\w+)", l)
        if m:
            runs.setdefault((m.group(2)[-6:] + "#" + m.group(3),),
                            (float(m.group(1)), m.group(4)))
        m = re.search(r"(\d+\.\d+): drm_sched_process_job: fence=(\w+) signaled", l)
        if m:
            procs.append((float(m.group(1)), m.group(2)))
    out = []
    for (k,), (t, f) in runs.items():
        c = [x for x, q in procs if q == f and x >= t]
        if c:
            out.append((k, (min(c) - t) * 1000))
    if out:
        return sorted(out, key=lambda r: -r[1]), "drm_sched"
    pend, res = {}, []
    for l in txt.splitlines():
        m = re.search(r"(\d+\.\d+): (pvr_fence_\w+): .*?timeline=(\S+)", l)
        if not m:
            continue
        t, ev, tl = float(m.group(1)), m.group(2), m.group(3)
        if ev == "pvr_fence_enable_signaling":
            pend[tl] = t
        elif ev == "pvr_fence_signal_fence" and tl in pend:
            res.append((tl.split("-")[0], (t - pend.pop(tl)) * 1000))
    return sorted(res, key=lambda r: -r[1]), "pvr_fence"


def kwin_alive():
    for x in ("kwin_x11", "kwin_wayland"):
        if subprocess.run(["pgrep", "-x", x], capture_output=True).returncode == 0:
            return True
    return False


def switch_to(want):
    """Switch kernel driver, honouring the guard's rule: never while kwin is up."""
    cur = driver()
    if cur == want:
        return True, "already bound"
    if kwin_alive():
        return False, "kwin is alive - guard would abort; refusing to unbind"
    sh("pkill -x weston; pkill -x Xwayland; sleep 2")
    args = (["/home/radxa/gpu-open-stack/switch-vendor.sh", "switch-open.sh"]
            if want != "pvrsrvkm" else ["switch-open.sh", "switch-vendor.sh"])
    # the switch scripts are named by target; map explicitly
    target = {"powervr": "/home/radxa/gpu-open-stack/switch-open.sh",
              "pvrsrvkm": "/home/radxa/gpu-open-stack/switch-vendor.sh"}[want]
    r = subprocess.run(["sudo", target], capture_output=True, text=True)
    time.sleep(4)
    return driver() == want, (r.stdout + r.stderr).strip().splitlines()[-1] if (r.stdout + r.stderr).strip() else ""


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--driver=")]
    want = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--driver=")), None)
    if want:
        target = {"open": "powervr", "vendor": "pvrsrvkm"}.get(want, want)
        ok, why = switch_to(target)
        print(f"  switch to {want}: {'ok' if ok else 'REFUSED'} ({why})")
    probe = argv[0] if argv else "vkrender"
    size = argv[1] if len(argv) > 1 else "2048"
    count = argv[2] if len(argv) > 2 else "20"   # frames (vk*) or iterations (cst*)
    secs = 300.0                                  # only a hang guard
    extra = argv[3:]

    drv = driver()
    is_open = drv == "powrvr" or drv == "powervr"
    icd, broken = OPEN_ICD if is_open else VENDOR_ICD

    env = dict(os.environ)
    env["VK_ICD_FILENAMES"] = icd
    if broken:
        env["PVR_I_WANT_A_BROKEN_VULKAN_DRIVER"] = "1"
    env.pop("DISPLAY", None)
    env.pop("XAUTHORITY", None)
    env["MESA_SHADER_CACHE_DISABLE"] = "1"
    for e in extra:
        if "=" in e:
            k, v = e.split("=", 1)
            env[k] = v
    if is_open:
        sh(f"rm -rf /home/radxa/.cache/mesa_shader_cache*")

    events = (["gpu_scheduler"] if is_open else ["pvr_fence"])
    trace_on(events)

    def run(frames, limit):
        t0 = time.time()
        pr = subprocess.Popen([f"{BENCH}/{probe}", size, str(frames)],
                              cwd=BENCH, env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True)
        while pr.poll() is None:
            if time.time() - t0 > limit:
                pr.terminate()
                break
            time.sleep(0.2)
        o, _ = pr.communicate()
        return o, time.time() - t0

    # phase 1: ONE frame - fence handles are unique, so job pairing is exact.
    # ponytail: fence pairing degrades above a few thousand jobs, so the
    # breakdown must come from a short run. The speed comes from phase 2.
    trace_on(events)
    _, _ = run("1", 60)
    txt = trace_off()

    # phase 2: many frames - for speed and correctness only.
    # ponytail: let phase 2 finish rather than terminate it - a truncated run
    # never prints its summary, which is where speed and correctness come from.
    # A generous limit only guards against a hang.
    # CPU split: sample the child's utime/stime. Needs the pid, so run inline.
    t0 = time.time()
    pr = subprocess.Popen([f"{BENCH}/{probe}", size, str(count)], cwd=BENCH, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    ru0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    while pr.poll() is None:
        if time.time() - t0 > secs:
            pr.terminate(); break
        time.sleep(0.05)
    out, _ = pr.communicate()
    wall = time.time() - t0
    ru1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = {"user_ms": round((ru1.ru_utime - ru0.ru_utime) * 1000, 1),
           "sys_ms": round((ru1.ru_stime - ru0.ru_stime) * 1000, 1)}

    jobs, src = parse_jobs(txt)
    frames = re.findall(r"([\d.]+) ms/frame", out)
    mpix = re.findall(r"([\d.]+) Mpix/s", out)
    correct = re.findall(r"RESULT: (PASS|FAIL) - (\d+)/(\d+) pixels correct", out)
    thr = re.findall(r"([\d.]+) M invocation/s", out)

    bpp = re.findall(r"bpp=(\d+)", out)
    fps = re.findall(r"FPS: (\d+)", out)
    rec = {"probe": probe, "size": size, "driver": drv, "wall_s": round(wall, 2),
           "cpu": cpu, "bpp": int(bpp[-1]) if bpp else None,
           "fps": int(fps[-1]) if fps else None,
           "ms_per_frame": float(frames[-1]) if frames else None,
           "mpix_s": float(mpix[-1]) if mpix else None,
           "thr_M_inv_s": float(thr[-1]) if thr else None,
           "correct": (correct[-1][0] if correct else None),
           "pixels_ok": (correct[-1][1] + "/" + correct[-1][2]) if correct else None,
           "jobs": [[n, round(d, 3)] for n, d in jobs[:6]],
           "trace": src}

    print(f"\n{'='*74}")
    print(f"  HARNESS  driver={drv}  probe={probe} size={size}  wall={wall:.1f}s")
    print(f"{'='*74}")
    print(f"  speed      : {rec['ms_per_frame']} ms/frame   {rec['mpix_s']} Mpix/s"
          f"   {rec['thr_M_inv_s']} M inv/s")
    print(f"  correctness: {rec['correct']}  {rec['pixels_ok'] or ''}")
    print(f"  jobs ({src}):")
    tot = 0
    for n, d in jobs[:6]:
        print(f"      {n:>12}  {d:9.3f} ms")
        tot = max(tot, d)
    print(f"      {'critical path':>12}  {tot:9.3f} ms")
    print(f"  stages     : {len(jobs)} jobs in the trace window")
    if cpu:
        tot_c = cpu["user_ms"] + cpu["sys_ms"]
        print(f"  cpu        : user {cpu['user_ms']} ms  sys {cpu['sys_ms']} ms"
              f"  (kernel {100*cpu['sys_ms']/max(tot_c,1e-9):.0f}% of probe CPU)")
    bw = ""
    if rec["mpix_s"] and rec["bpp"]:
        bw = f"   ~{rec['mpix_s']*rec['bpp']:.0f} MB/s written"
    print(f"  bandwidth  : bpp={rec['bpp']}  {bw}")
    if rec["fps"]:
        print(f"  fps        : {rec['fps']}")
    print(f"{'='*74}\n")

    with open(f"{BENCH}/harness-log.jsonl", "a") as f:
        f.write(json.dumps(rec) + "\n")


main()
