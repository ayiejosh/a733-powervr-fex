# Honest scope of the unroll fix: real for loop-bound shaders, no change on the default suite

Measured the **full glmark2-es2 suite** (all scenes, 640×480, composited through weston+Xwayland+zink) with
and without the `max_unroll_iterations = 64` fix:

| | glmark2 Score |
|---|---|
| WITH the fix | **46** |
| WITHOUT the fix | **46** |

**No change.** The fix's benefit is real but **narrow** — it applies when a shader's loops dominate.

| measurement | before | after | gain |
|---|---|---|---|
| `cstpi32` (32-iteration compute) | 26.7 M/s | 49.8 M/s | **1.87×** |
| `vkheavy` (32-iteration fragment) | 858.5 ms | 597.1 ms | **1.44×** |
| shader-heavy client scene (`fragment-complexity=high:steps=10`) | median 49 FPS | median 62 FPS | **~1.27×** |
| **full glmark2 default suite** | **46** | **46** | **none** |

**Why:** the default suite's slowest scenes are bound by other things — `terrain` 5 FPS, `refract` 12 FPS,
`desktop blur` 24 FPS — none loop-throughput-bound. The score is a sum dominated by those, so a loop-only
improvement doesn't move it.

**Recorded as a scope limit, not a retraction:** the fix is correct, measured, and committed (`c2bde57`),
and it is the right fix for loop-heavy shaders. It is simply not the whole gap.

## Where the remaining gap is, per the suite

The slowest scenes are those with **multi-pass or multi-window** work (`desktop blur` 24, `terrain` 5,
`refract` 12) rather than shader complexity (`conditionals` 57, `function` 48–53, `loop` 46–58). That
points back at the **per-surface/per-pass cost** measured earlier (the 3.3× per-tile deficit and the
per-pass syncobj overhead) rather than at shader execution.
