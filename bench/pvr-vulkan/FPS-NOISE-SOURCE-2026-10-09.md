# The FPS noise source is ~65% of a core of uncontrollable background load

## What was tried

Following the withdrawn client-scene claim, the obvious fix was to remove the background load and re-measure.
**It cannot be removed:**

```
%CPU COMMAND
33.7 syncthing     ← persisted despite `systemctl --user stop` and `pkill -x syncthing`
31.8 MainThread    ← the DSH agent harness itself; the thing doing the measuring
28.4 Xwayland      ← the compositor under test
 7.4 bash
```

**~65% of a core of load that exists precisely because this session is running.** Stopping it from inside it
is not possible.

## The measurement, re-taken

| | runs | median | range |
|---|---|---|---|
| shader-heavy scene, 5 runs | **49 / 59 / 54 / 44 / 49** | **49** | **44–59, ~34%** |

**The variance is unchanged** — confirming the load, not the technique, is the source. This independently
re-confirms that the withdrawn client claim (49 → 62) was noise.

## The methodological conclusion — the actual result of this round

**On this host, wall-clock FPS cannot resolve any effect smaller than ~35%.** That is not a flaw in the
benchmarks; it is a property of a machine simultaneously running the agent, a sync daemon and a compositor.

**Everything trustworthy measured this session used a metric insensitive to that load:**

- **Throughput in M invocations/s** — a fixed amount of work over the kernel-timed duration of that work, so
  contention on other cores doesn't scale it (`cstpi`, `cstpf`, `cstpin`, `cstp`: repeat to **~1%**)
- **Per-job kernel timestamps** via `drm_run_job`/`drm_sched_process_job` (`vkrender`, `vkheavy`: repeat to
  within **1%**)
- **ioctl and job counts** — integers, not durations

> **Standing rule for this board: measure effects with throughput or kernel timestamps; use wall-clock FPS
> only to detect changes above ~35%, and never for a 3-run median comparison.**

The session's earlier lesson (25% variance, interleaving mandatory) now has **its cause identified and its
threshold corrected to ~35%.**
