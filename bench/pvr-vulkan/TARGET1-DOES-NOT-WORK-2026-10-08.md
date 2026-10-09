# Target (1) as stated would NOT work — the per-pass cost is the WAIT, not the churn

## What I tried, and why I stopped before shipping it

Target (1) proposes pooling or timeline-backing the per-job `vk_sync` objects. I started implementing a pool
for the temporary syncobj in `pvr_drm_winsys_null_job_submit` (the multi-wait path currently does
`drmSyncobjCreate` + N transfers + final transfer + `drmSyncobjDestroy`).

**Two findings stopped it:**

1. **Pooling a syncobj used as a join point is unsound.** `drmSyncobjTransfer` installs a **persistent
   dependency** from the destination point onto the source point. Reusing a pooled syncobj **re-points an
   earlier submit's dependency at a new source** — a correctness bug, and exactly why the driver creates a
   fresh one per call. The safe variant (reset instead of create+destroy) saves one of three ioctls.
2. **The churn is not where the time goes.** The measured breakdown of the per-pass cost was
   **0.456 ms/pass blocked in `drm_syncobj_array_wait_timeout`** — **waiting**, not object management.
   Removing create/destroy pairs would not touch the dominant term.

**Reverted; no code shipped.**

## This explains why timeline-backing measured worse

The earlier measurement (3404 → 3604 total ioctls, waits +50%, +399 RESET) is consistent: **both of target
(1)'s proposed fixes attack the object representation, and the cost is the wait.**

## What the per-pass cost actually is

| component | measured |
|---|---|
| `drm_syncobj_array_wait_timeout` blocking | **0.456 ms/pass** |
| create/destroy/transfer churn | 15 ioctls/frame, not the dominant term |
| total per-pass kernel time | **0.968 ms** vs the vendor's 0.003 ms |

The wait exists because each pass must observe the previous pass's completion before the next is submitted —
**there is no kernel-side mechanism to chain them.**

## The correct target is the kernel UAPI

**A kernel-side job-chaining facility (a null job type, or equivalent) letting the firmware order passes
without a userspace round trip.** That is a `drm/imagination` UAPI change — in the objective's declared
scope (the mainline `powervr` module) — and the only identified route to the **22–65%** that the multi-pass
scenes spend on per-pass overhead.
