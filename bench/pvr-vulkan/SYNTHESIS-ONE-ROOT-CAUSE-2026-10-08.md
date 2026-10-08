# SYNTHESIS: one root cause (3.3x per-surface render cost) explains BOTH the render and present gaps

## The release wait is per-surface, not a fixed sync latency

`WSIREL_TRACE` at two client sizes:

| client size | surface | release wait |
|---|---|---|
| 640x480 | 0.31 Mpix | 12.5 ms |
| 1920x1080 | 2.07 Mpix | **66.0 ms** |

**5.3x the wait for 6.7x the pixels** - the wait is proportional to the surface, exactly like the render
cost. `ACQ_TRACE` gives 61.3 ms at 1080p for the same reason.

## The arithmetic closes with the per-surface rate

| quantity | open | vendor | ratio |
|---|---|---|---|
| per-surface render rate (fixed-work test) | **3.47 ms/Mpix** | **1.06 ms/Mpix** | **3.29x** |
| weston 4K output (8.29 Mpix), 1 pass | 28.8 ms | 8.8 ms | |
| **same, 2 passes** | **57.5 ms** | 17.5 ms | |
| **measured 1080p release wait** | **66 ms** | - | |

**57.5 ms predicted, 66 ms measured** - weston compositing the 4K output in ~2 passes at the open
driver's per-surface rate. Remainder: the sync round-trip.

## The synthesis

**The entire gap traces to one defect: the open driver's per-surface render cost is 3.3x the vendor's.**
It appears twice:

1. **The client's own render** is 3.3x slower (7.2 vs 2.2 ms at 1080p).
2. **weston's composite** is 3.3x slower, and because it runs at the **4K output size in ~2 passes** the
   absolute penalty is large (57.5 vs 17.5 ms). The open client **waits** for that composite via explicit
   sync, so the penalty lands in its frame. **The vendor client doesn't wait, so it never pays it in its
   own timing at all.**

**So the objective's framing - "the gap is the Vulkan driver, not the compositor" - is right, and now for
a measured reason: the compositor is slow *because* it runs on the same 3.3x-costlier driver. It is a
victim, not a cause.**

## Why this is the most important result of the session

* **Collapses two previously separate terms** (render 2.5-3.4x, present 22.6x) into **one cause**.
* **Explains why widening either the client surface or the output size both hurt** - both scale the same
  per-surface term.
* **Explains why the waits looked like a sync/scheduling problem for so long**: they are downstream of a
  rendering cost, and I was measuring the *wait* rather than what it was waiting for.
* **Gives one target**: reduce the per-tile render cost by 3.3x and both gaps shrink together.
  **The vendor's 1.06 ms/Mpix is the number to reach.**

## Confidence

The two inputs are the two most reliable measurements in the session: the per-surface rate (confirmed by
two independent methods, 3.4x and 3.3x, both far above the ~25% noise floor) and the per-surface release
wait (a driver-internal trace, not wall-clock FPS). The agreement between the predicted 57.5 ms and
measured 66 ms is within the sync round-trip.
