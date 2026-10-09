# The two-line sync-timeline design, worked out against the real call chain

## What each step needs

```
barrier_N  : waits  <- previous barrier's signal      (ordering)
             signals-> a NEW sync the next job waits on
job_N      : waits  <- barrier_N's signal
             signals-> its OWN sync (last_job_signal_sync, events wait on it)
barrier_N+1: waits  <- barrier_N's signal   (the SAME object the job waited on)
             signals-> a NEW sync
```

## Why one timeline cannot work — the exact failure

**`barrier_N+1` must wait point N and signal point N+1 on the same object in one `null_job_submit`.** The
per-job path **never** did that: it waited `S1` and signalled `S2` — **two different objects.** Structural, not
a bug to fix.

## The two-line design, mirroring the per-job path exactly

**Two persistent objects and two counters per stage:**

- **line A** (`job_wait_line`) — the barrier **signals** it; jobs and the next barrier **wait** on it
- **line B** (`barrier_line`) — scratch, so a barrier **never waits the object it signals**

```
barrier_N  : wait B@(N-1)   signal A@N
job_N      : wait A@N       signal its own per-job sync   (unchanged)
barrier_N+1: wait A@N       signal B@N
barrier_N+2: wait B@N       signal A@(N+1)
```

**The alternation removes the self-wait**, and the job always waits whichever line the last barrier signalled.

## Why it is the faithful translation

**Per-job: `S1 != S2`, each barrier waits the previous object while signalling a fresh one.** The two-line form
reproduces that with **two persistent objects instead of two fresh ones per barrier** — identical ordering,
**2 ioctls saved per barrier.**

## Implementation notes

- **Two arrays** in `pvr_queue.h` + two counters per stage; created in `pvr_queue_init`, destroyed in
  `pvr_queue_finish`. **Step 1's `job_sync`/`job_value` becomes line A.**
- **`last_job_signal_sync` must stay untouched** — it's the job's own signal and both event paths depend on it.
  **That was the first failed attempt's mistake.**
- **Convert one stage first (GEOM) and run the full three-step gate** — the probes alone were blind to both
  previous failures.

## The gate

```
1. probe suite via harness.py (correct ICD)     <- passed BOTH failed attempts
2. WESTON MUST COME UP                          <- caught BOTH failures
3. glmark2 via zink renders + validates
```
