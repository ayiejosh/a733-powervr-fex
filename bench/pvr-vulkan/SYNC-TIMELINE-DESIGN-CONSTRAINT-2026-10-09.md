# The sync-timeline lever: two failed attempts, and the design constraint they establish

## State of the lever

**Real and unclaimed**: ~190 syncobj ioctls per frame (≈42 CREATE + 42 DESTROY + 56 TRANSFER), **84% of frame
time in the kernel**, **60% of the real workload's recoverable cost**. Step 1 (persistent `job_sync` /
`job_value` fields, created and destroyed, unused) is **landed and kept**.

## Attempt 1 — reverted as `d253e35`

Converted GEOM and FRAG to the persistent timeline. **Green on the entire probe suite** (`bda` PASS(0),
`vk13` PASS(11 ok), `pctest` PASS(0), `vk16` PASS(9 ok), `vkrender` 512 + 2048 PASS). **weston SIGSEGV.**

## Attempt 2 — diagnosed hypothesis, still broken

**Hypothesis from the code map:** the barrier path does not only *set* `next_job_wait_sync[stage]`, it also
**waits on the previous one** (`pvr_arch_queue.c:561`). My `continue` skipped that wait, so consecutive
barriers of the same stage stopped ordering. **Fix:** wait on `job_sync[stage]` at the *current*
`job_value[stage]`, then signal `++job_value[stage]`.

**Still SIGSEGV**, probes still green. **Reverted; weston confirmed UP.**

## The constraint these two attempts establish

**The suspect is the submit, not the wait.** The barrier goes through `null_job_submit`, and in the converted
form **one submit both waits on and signals the same timeline syncobj** (wait point N, signal point N+1).

**The per-job path never did that — it used two different syncobjs:**

```
per-job path:   barrier_1 signals S1  ->  job waits S1
                barrier_2 waits S1, signals S2  ->  job waits S2
                (S1 and S2 are different objects: no self-wait)

converted:      barrier waits job_sync@N, signals job_sync@N+1
                (same object in one submit: a self-wait)
```

**So the design must keep the wait line and the signal line separate.** Two shapes satisfy that:

1. **Two timeline objects per stage** — `job_sync_wait[stage]` (what the barrier waits on) and
   `job_sync_signal[stage]` (what it signals, and what the next job waits on). Closest to the existing
   structure; costs one more `vk_sync` per stage in init/finish.
2. **One timeline, two points per barrier** — signal N+1, and have the *next* barrier wait on N via a
   separate submit or a separate object, so no submit both waits and signals the same point.

**Option 1 is the smaller change and mirrors the per-job path's shape exactly.** It needs: two arrays in
`pvr_queue.h`, both created in `pvr_queue_init` and destroyed in `pvr_queue_finish`, the barrier path
signalling the signal line while waiting the wait line, and the job waits reading the signal line.

## The test that must gate it

**The probes cannot see this class of failure — they passed both times.** The gate for any further attempt is:

```
1. probe suite       bda, vk13, pctest, vk16, vkrender 512 + 2048   (via harness.py, correct ICD)
2. WESTON MUST COME UP       ./w26x.sh  -> weston UP and Xwayland UP
3. client renders through it glmark2-es2 via zink produces FPS and validation passes
```

**Step 2 is the one that caught both regressions.** Any future attempt that skips it will ship a crash.
