# The kernel fix is worth ~2.5-5% on a real client - quantified before taking the risk

The fix for the per-pass syncobj cost belongs in the mainline kernel module (no null job type exists).
That means: add `DRM_PVR_JOB_TYPE_NULL` to the UAPI enum, add a case in `pvr_job.c`'s submit switch,
rebuild the module, load it, update Mesa to use it. **Loading a modified GPU module risks the session,
so I measured the payoff first.**

`PVR_SUBMIT_MIX=1` on a real windowed zink client:

```
[mx] null=3200   waits:0=1067 1=1066 2=1067 3+=0
[mx] render_job_submits=500
FPS 55, FrameTime 18.503 ms
```

**6.4 null jobs per frame** (3200/500), waits evenly split: 1/3 with 0 waits (create+destroy), 1/3 with
1 (1 transfer), 1/3 with 2 (create + 3 transfers + destroy).

Per frame: **~4.2 creates + 4.2 destroys + 8.4 transfers ≈ 17 ioctls**, i.e. **~0.45-0.9 ms/frame =
2.5-5% of an 18.5 ms frame.**

## Decision

**Not taking the kernel-module change this round:**

* Payoff bounded at ~5% on a real client - an order of magnitude below the draw (2.13x) and present
  (5.4-8.9x) gaps.
* Cost is a UAPI change plus a rebuilt, reloaded GPU module, with session-loss risk on a board already
  rebooted by GPU driver work.
* The per-pass cost **is** worth fixing eventually, and this says exactly what it is worth - the useful
  output of the round. Not worth doing blind.

## What would change the decision

If the draw or present gaps turn out to be blocked, this becomes the next-best target and the ~5% is
worth the module risk. Until then it is second-tier.
