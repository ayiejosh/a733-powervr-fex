# The compositing path runs ~13x slower than the driver's raw render

## Raw render (measured properly, 50 iters)

| size | throughput | frame |
|---|---|---|
| 512 | 149.7 Mpix/s | 1.75 ms |
| 1024 | 249.8 Mpix/s | 4.20 ms |
| 2048 | 300.7 Mpix/s | 13.95 ms |

Fits `frame_ms = 0.93 + 3.11 * Mpix` (predicted 13.97 vs measured 13.95). Marginal rate
**321 Mpix/s**, plus a **~0.93 ms fixed per-frame cost** (53% of a 512x512 frame).

The earlier "122.7 Mpix/s" was a single frame dominated by startup - do not quote it.

## Windowed frames minus render

| size | Mpix | render (model) | measured frame | implied compositor/sync |
|---|---|---|---|---|
| 160x120 | 0.019 | 0.99 ms | 8.85 ms | 7.86 ms |
| 320x240 | 0.077 | 1.17 ms | 9.90 ms | 8.73 ms |
| 640x480 | 0.307 | 1.89 ms | 18.52 ms | 16.63 ms |
| 1920x1080 | 2.074 | 7.38 ms | 90.91 ms | **83.53 ms** |

The compositor term grows super-linearly and dominates at 1080p: 83.5 ms of 90.9 ms, ~11x the render.

## The rate

83.5 ms for 2.07 Mpix is **~25 Mpix/s** through compositing vs the driver's raw **321 Mpix/s** - a
**13x penalty**. Not a memcpy (8.3 MB in 83 ms is only ~100 MB/s), so it is per-pixel GPU work or a
layout/format path that defeats it.

## Next, specific and cheap to test

1. Client swapchain image **layout** - if pvr's WSI hands weston a tiled/non-linear buffer,
   compositing may hit a slow sampling/detile path. (The earlier tiling comparison, 211 vs 218
   Mpix/s, measured the *client's render*, not weston's sampling of it.)
2. **Format conversion** between the client buffer and the output.
3. **Pass count** - one composite, or Xwayland surface plus weston output.

Measure weston's own GPU time for a composited frame rather than inferring it.
