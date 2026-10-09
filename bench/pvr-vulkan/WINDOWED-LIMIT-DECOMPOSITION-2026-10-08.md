# The windowed limit: a fixed ~7 ms latency plus an area-dependent term

## Sweep

Open stack, weston + Xwayland, zink client, default vsync:

| size | area (Mpix) | release wait | acquire wait | FPS | frame |
|---|---|---|---|---|---|
| 160x120 | 0.019 | 7.10 ms | 3.93 ms | 113 | 8.8 ms |
| 320x240 | 0.077 | 7.20 ms | - | 101 | 9.9 ms |
| 640x480 | 0.307 | 13.08 ms | 10.82 ms | 54 | 18.5 ms |

## The vsync hypothesis is dead

**113 FPS at 160x120 is far above 60**, and FPS tracks area. Not vsync-capped. (Earlier
`vblank_mode=0` giving only +15% was consistent with this and should have been read that way.)

## The two terms

* **Fixed ~7 ms floor** in the release wait, area-independent (7.10 ms at 0.019 Mpix, 7.20 ms at
  0.077). Caps the client at ~140 FPS, matching the 113 measured at the smallest size.
* **Area-dependent term**: 13.08 ms at 0.307 Mpix, ~6 ms above the floor.

At the smallest size the client still spends **81% of its frame blocked** (7.1 of 8.8 ms).

## What they are

* **~7 ms fixed**: compositor/sync round-trip - commit -> weston repaint -> Xwayland ->
  explicit-sync signal -> client wakes. Not vsync (~16.7 ms, would cap at 60).
* **Area term**: the GPU finishing the frame; the release waits on render completion. This is the
  driver's throughput, i.e. the same thing "raw render 2.5-4x down" measures. At 1920x1080 the
  client measured 11 FPS - render-bound.

## Where to work

1. The fixed 7 ms is weston + Xwayland + explicit-sync latency; it caps the ceiling at ~140 FPS
   whatever the driver does. Earlier rounds attacked the sync churn and measured ~3.4 ms/frame
   removable - a fraction of this floor.
2. **The area term is the larger prize at realistic sizes and is in the driver's scope.** 1080p at
   11 FPS is render-bound, not latency-bound.
