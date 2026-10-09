# Correction: the PR job's two TODOs fix different things, and only one touches the 4.03×

## What I got wrong

I listed the host-side TODO first as "the safe first step" toward the measured **4.03×** PR-job gap.
**Checked the submit array — that is wrong.**

```
[0] = geometry job
[1] = PR job      (DRM_PVR_JOB_TYPE_FRAGMENT | PARTIAL_RENDER)   — ALWAYS submitted
[2] = frag job    (only if submit_info->has_fragment_job, which is job->run_frag)
```

**When `!run_frag`, `pvr_render_job_ws_fragment_state_init()` builds a fragment state that is never
submitted.** Skipping it — the first TODO — **saves host CPU only.**

## The distinction that matters

| TODO | saves | touches the 4.03×? |
|---|---|---|
| "avoid setting up the fragment state … if `!job->run_frag`" | **host CPU** | **no** |
| "eliminate the pr and use the frag directly in case we enter SPM" | **GPU job** | **yes — this is the 4.03×** |

**The 4.03× is the PR job's GPU execution time** (9.31 vs 2.31 ms, from per-job kernel timestamps). **A
host-side setup saving cannot reduce it.** The prize belongs to the **second** TODO — the riskier one, because
it changes what the GPU is asked to do.

## Both are still worth having, for different reasons

- **Host-side: nearly free, correctness-safe** — and host CPU *is* one of the two levers (the client's
  84%-of-frame kernel cost). **A small low-risk win, not the 4.03×.**
- **GPU-side: the 4.03×** — needs a defensible test for "no PR can be needed", which the driver cannot know
  in advance because the firmware decides.

**Recorded because the previous entry implied the safe change would buy the measured gap, and it would not.**
