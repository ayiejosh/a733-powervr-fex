# The fragment stage is ~10 ms of a 13.8 ms frame, delivered as TWO concurrent jobs

## Timeline: two long jobs run concurrently and contend

```
job        start(ms)   dur(ms)   end(ms)
b5c81c      0.033      9.398     9.431     ┐ both start at ~0,
e7a6d4      0.794     12.460    13.254     ┘ both span the whole frame
```

**The 9.4 ms and 12.5 ms jobs overlap almost entirely** — two concurrent full-surface raster jobs on
separate queues, contending for the same GPU. Neither hides behind the other.

## Job identification by suppressing the fragment job

| mode | jobs | durations |
|---|---|---|
| normal | 4 | 0.41 · 0.41 · **9.41** · **12.31** |
| **`PVR_NO_FRAG=1`** (`job->run_frag = false`) | 3 | 0.40 · 0.43 · **3.84** |

**Suppressing the fragment job removes BOTH long jobs**, leaving a 3.84 ms frame. So:

* **fragment stage ≈ 10 ms** of a 13.8 ms frame (two overlapping jobs)
* **everything else ≈ 3.84 ms** (geometry + null/transfer jobs)

*(Correctness is not asserted for the `PVR_NO_FRAG` variant — timing-only diagnostic, output expected
wrong. Knob reverted; 2048 verification re-confirmed PASS.)*

## Why this matters

* **Target (5) says the partial-render job performs no PRs when not needed and adds no TA→3D
  transition. The job is nevertheless 9.41 ms of real fragment-stage work** running concurrently with the
  12.31 ms fragment job. Whatever it does, it is not free.
* With the vendor control (fragment-side timelines 1.94 and 5.31 ms), **the open driver spends ~10 ms on
  a stage the vendor does in ~5.3 ms of critical path.**
* **Two concurrent full-surface raster jobs contending is itself a candidate**: if one is redundant,
  removing it frees the contention as well as its own time.

## Next

**Determine what the 9.41 ms job actually renders.** It is submitted as `DRM_PVR_JOB_TYPE_FRAGMENT` with
`DRM_PVR_SUBMIT_JOB_FRAG_CMD_PARTIAL_RENDER`, so it should perform no PRs when they are not needed — yet
it costs 9.41 ms and halves under `rasterizerDiscardEnable`. **Either it performs PRs it should not, or it
does fragment work that duplicates the second job.**
