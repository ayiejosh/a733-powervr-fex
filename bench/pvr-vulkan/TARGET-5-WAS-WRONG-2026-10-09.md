# Target (5) is WRONG — the PR job is not a non-issue, it is the worst stage at 4.03×

## The contradiction

**The goal's own target (5):**

> *"The unconditional partial-render job: CLOSED as a non-issue — the driver documents that the firmware
> performs no PRs when they are not needed, and geometry/PR/fragment are one submit so it adds no TA→3D
> transition."*

**The measurement says otherwise** — per-job kernel timestamps, single instrument, both drivers:

| job | open | vendor | ratio |
|---|---|---|---|
| geometry / TA | 0.35 ms | 0.82 ms | **0.43× — open WINS** |
| **PR (partial render)** | **9.31 ms** | **2.31 ms** | **4.03×** |
| fragment | 13.01 ms | 5.99 ms | 2.17× |

**9.31 ms of real, measured GPU time, and the WORST ratio in the decomposition — worse than the fragment job.**

## Where target (5) went wrong

**It reasoned from two true statements to a false conclusion:**

1. *"the firmware performs no PRs when they are not needed"* — **true** (and `pvr_drm_job_render.c:587` says so)
2. *"geometry/PR/fragment are one submit so it adds no TA→3D transition"* — **true**

**But "no PRs are performed" is not "the job costs nothing."** The job is still **submitted**, and its state is
built by **copying the fragment job's entire command stream** (`/* Massive copy :( */`). **So the GPU still
runs a fragment-shaped pass over the tile range, and the host still builds a whole stream for it.**

**The reasoning treated "the firmware skips the work" as equivalent to "the submission is free."** The
measurement shows a 9.31 ms job.

## Why this matters

**Target (5) being marked CLOSED removed the worst stage from the target list.** Had it stayed open, the
**4.03×** would have been the top item rounds earlier.

**Same failure mode as the withdrawn client-FPS claim: reasoning from a plausible mechanism instead of
measuring the thing.**

**Corrected position: the PR job is the highest-ratio stage, it is Mesa-side, and the driver's own TODOs flag
two ways to reduce it** — the host-side one identified as a refactor rather than a skip.
