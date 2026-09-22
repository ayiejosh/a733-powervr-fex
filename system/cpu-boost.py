#!/usr/bin/env python3
"""cpu-boost — demand-driven CPU clocking for the A733 board.

Goal: full clock the moment there is real work, idle clock the moment there isn't,
with no throughput sacrificed while work continues.

How: the schedutil governor stays in charge (light loads get sensible mid frequencies),
but while demand is detected the per-cluster *floor* (scaling_min_freq) is raised to the
hardware maximum.  A floor at max pins the cluster at max for exactly as long as the
demand lasts — the same throughput as the `performance` governor — and the floor is
released after the board goes quiet, so idle returns to 416 MHz.

Demand signal: CPU consumed by **user.slice** (the desktop session, games, emulators,
the agent) read from cgroup v2 accounting.  Background daemons (syncthing, tailscaled,
indexers) live in system.slice and therefore cannot trigger a boost — which matters
here because syncthing alone keeps ~30% of a core busy at all times, enough to defeat
any plain utilisation threshold.  A system-wide guard also boosts for heavy system jobs
(apt/dkms builds, big compiles).

Why not the kernel's own mechanisms:
  * CONFIG_UCLAMP_TASK is not set (no util-clamp).
  * schedutil never ramps to max for bursty threads blocked on GPU ioctls
    (measured 1.2-1.4 GHz, -29% on CPU-bound glmark2 scenes vs performance).
  * neither schedutil's nor ondemand's tunables are exposed in sysfs.
"""
import argparse
import os
import signal
import sys
import time

CPUFREQ = "/sys/devices/system/cpu/cpufreq"
USER_SLICE = "/sys/fs/cgroup/user.slice/cpu.stat"
NCPU = os.cpu_count() or 8


_POLS = []


def cleanup(signum, frame):
    """On stop, hand the clocks back to the governor at their minimum."""
    for pol in _POLS:
        set_floor(pol, pol["mn"])
    sys.exit(0)


def log(msg):
    sys.stdout.write("cpu-boost: %s\n" % msg)
    sys.stdout.flush()


def read_user_usec():
    """Cumulative CPU microseconds charged to user.slice (None if unavailable)."""
    try:
        with open(USER_SLICE) as fh:
            for ln in fh:
                if ln.startswith("usage_usec"):
                    return int(ln.split()[1])
    except (OSError, ValueError, IndexError):
        pass
    return None


def read_stat():
    """{cpu: (busy, total)} jiffies from /proc/stat."""
    out = {}
    with open("/proc/stat") as fh:
        for ln in fh:
            if not ln.startswith("cpu") or ln[3] == " ":
                continue
            f = ln.split()
            try:
                idx = int(f[0][3:])
            except ValueError:
                continue
            busy = sum(int(f[i]) for i in (1, 2, 3, 7, 8, 9))
            out[idx] = (busy, busy + int(f[4]) + int(f[5]))
    return out


def discover():
    pols = []
    for name in sorted(os.listdir(CPUFREQ)):
        if not name.startswith("policy"):
            continue
        p = os.path.join(CPUFREQ, name)
        try:
            hw = int(open(p + "/cpuinfo_max_freq").read())
            mn = int(open(p + "/cpuinfo_min_freq").read())
        except (OSError, ValueError):
            continue
        pols.append({"name": name, "path": p, "hw": hw, "mn": mn, "boosted": False, "idle": 0})
    return pols


def set_floor(pol, value):
    try:
        with open(pol["path"] + "/scaling_min_freq", "w") as fh:
            fh.write(str(value))
        return True
    except OSError as exc:
        log("cannot set %s floor=%s: %s" % (pol["name"], value, exc))
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--interval", type=float, default=0.2)
    ap.add_argument("--boost-cores", type=float, default=0.60,
                    help="user.slice cores in use that count as demand")
    ap.add_argument("--drop-cores", type=float, default=0.15,
                    help="user.slice cores below which the board counts as quiet")
    ap.add_argument("--sys-cores", type=float, default=None,
                    help="system-wide cores in use that also count as demand "
                         "(default: NCPU * 0.7)")
    ap.add_argument("--sys-quiet", type=float, default=None,
                    help="system-wide cores below which the board counts as quiet "
                         "(default: NCPU * 0.12)")
    ap.add_argument("--drop-after", type=int, default=5, help="quiet windows before release")
    ap.add_argument("--min-boost", type=float, default=5.0,
                    help="seconds to stay boosted once triggered (prevents flapping)")
    ap.add_argument("--max-floor", type=int, default=0,
                    help="never boost above this frequency (safety cap during OC testing; 0 = no cap)")
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()

    sys_boost = args.sys_cores if args.sys_cores is not None else NCPU * 0.7
    sys_quiet = args.sys_quiet if args.sys_quiet is not None else NCPU * 0.12

    pols = discover()
    if args.max_floor:
        for pol in pols:
            if pol["hw"] > args.max_floor:
                pol["hw"] = args.max_floor
        log("boost capped at %d Hz" % args.max_floor)
    _POLS[:] = pols
    signal.signal(signal.SIGTERM, cleanup)
    signal.signal(signal.SIGINT, cleanup)
    if not pols:
        log("no cpufreq policies found")
        return 1
    for p in pols:
        set_floor(p, p["mn"])

    log("started interval=%.2fs demand: user.slice>=%.2f cores or system>=%.2f of %d cores; "
        "quiet: user<%.2f and system<%.2f for %d windows"
        % (args.interval, args.boost_cores, sys_boost, NCPU, args.drop_cores, sys_quiet, args.drop_after))

    prev_user = read_user_usec()
    prev_stat = read_stat()
    boosted_state = False
    idle_count = 0
    boosted_at = 0.0

    while True:
        time.sleep(args.interval)
        cur_user = read_user_usec()
        cur_stat = read_stat()

        user_cores = 0.0
        if cur_user is not None and prev_user is not None:
            user_cores = (cur_user - prev_user) / (args.interval * 1e6)

        busy = tot = 0
        for c, (b, t) in cur_stat.items():
            if c in prev_stat:
                busy += b - prev_stat[c][0]
                tot += t - prev_stat[c][1]
        sys_cores = (busy / tot * NCPU) if tot > 0 else 0.0

        demand = user_cores >= args.boost_cores or sys_cores >= sys_boost
        quiet = user_cores < args.drop_cores and sys_cores < sys_quiet

        if args.debug:
            log("dbg user=%.2f cores  system=%.2f/%d cores  boosted=%d idle=%d%s"
                % (user_cores, sys_cores, NCPU, boosted_state, idle_count,
                   "  <-- demand" if demand else ("  <-- quiet" if quiet else "")))

        if demand:
            idle_count = 0
            if not boosted_state:
                if all(set_floor(p, p["hw"]) for p in pols):
                    boosted_state = True
                    boosted_at = time.monotonic()
                    log("BOOST -> floors at max (user %.2f cores, system %.2f cores)"
                        % (user_cores, sys_cores))
        elif quiet:
            idle_count += 1
            if (idle_count >= args.drop_after and boosted_state
                    and (time.monotonic() - boosted_at) >= args.min_boost):
                if all(set_floor(p, p["mn"]) for p in pols):
                    boosted_state = False
                    idle_count = 0
                    log("release -> floors at min (user %.2f cores, system %.2f cores)"
                        % (user_cores, sys_cores))
        else:
            idle_count = 0

        prev_user = cur_user
        prev_stat = cur_stat


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
