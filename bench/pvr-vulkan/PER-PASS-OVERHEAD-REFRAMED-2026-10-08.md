# REFRAME: the per-pass overhead is 22–65% of the slow scenes, not 5%

## The measurement

Job counts per frame, same setup (640×480, composited weston+Xwayland+zink, 30 s runs), counted from the
`drm_sched_job` tracepoints, normalised by frame count:

| scene | jobs/frame | FPS | frame time | est. per-pass overhead @ 0.968 ms/pass |
|---|---|---|---|---|
| **terrain** | **45.7** | 5 | 200 ms | 44 ms — **22%** |
| **desktop blur (4 windows)** | **28.0** | 24 | 41.7 ms | 27 ms — **65%** |
| **refract** | **20.4** | 12 | 83 ms | 20 ms — **24%** |
| conditionals (simple) | 8.7 | 47 | 21 ms | 8 ms — 38% |

## The correction

**The earlier "per-pass overhead payoff is 2.5–5%" was measured on a SINGLE-PASS client frame and is
misleading for multi-pass scenes.** The slowest glmark2 scenes issue **20–46 GPU jobs per frame**, and at the
measured **0.968 ms per pass** that is **22–65% of their frame time**, not 5%.

**Target (1) is therefore not a minor item — it is the largest remaining identified lever for exactly the
scenes that drag the suite score down.**

## Why this reframes the objective

* glmark2's slowest scenes (`terrain` 5, `refract` 12, `desktop blur` 24) are precisely the **multi-pass /
  multi-window** ones.
* The fast scenes (`conditionals` 47, `function` 48–53, `loop` 46–58) are the **single-pass** ones.
* **Jobs per frame tracks scene speed far better than shader complexity does.** The loop fix (1.44× on
  loop-bound shaders) did not move the suite; reducing per-pass cost would touch exactly the scenes that
  dominate it.

## The mechanism (already root-caused)

The 0.968 ms/pass is per-**pass** kernel time, and the driver re-establishes synchronisation for every job:

* `pvr_drm_winsys_null_job_submit` is a **userspace fence-forwarding routine** — it exists because **the UAPI
  has no null job type**, so there is no kernel-side way to chain jobs without an ioctl.
* 15 syncobj churn ioctls per frame (5 CREATE + 5 TRANSFER + 5 DESTROY), plus 2 WAIT + 2 SUBMIT_JOBS.
* **Timeline-backing was measured and is worse** (3404 → 3604 total ioctls, waits +50%, adds 399 RESET).

**So the fix is not another vk_sync representation — it is a kernel-side job-chaining facility (a null job
type or equivalent) that removes the per-pass ioctl round trip.** That is a mainline `powervr` UAPI change,
inside the objective's declared scope.
