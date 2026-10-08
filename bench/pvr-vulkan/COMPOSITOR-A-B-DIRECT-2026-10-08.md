# Direct A/B: the composited path costs ~64 ms/frame at 1080p, ~0 at 640x480

## Method

`pvranimate` presents by page flip to KMS with **no compositor**; the windowed client goes through
weston + Xwayland. Same driver, same sizes, so the only variable is the compositor path.

```
size        KMS (no compositor)   windowed (composited)
640x480     56.2 fps              58 FPS
1920x1080   55.8 fps              13 FPS
```

## Established

* **640x480: no compositor penalty** (56.2 vs 58).
* **1920x1080: composited is 4.3x slower** (55.8 vs 13) = **~64 ms/frame extra, measured directly**
  (previously this was only a residual from subtracting vkrender's model).
* **KMS is flat across sizes** (56.2 -> 55.8) = vsync-capped at ~56 fps with headroom, not
  render-bound. The driver renders 1080p well inside a frame (vkrender model: 7.4 ms at 2.07 Mpix).

**The 32x windowed gap is therefore in a path costing ~0 ms at 0.3 Mpix and ~64 ms at 2.07 Mpix.**
Strongly area-dependent, which rules out per-frame overheads (ioctls, sync round-trips, submits) as
the main term - those are area-independent and measured small.

## Refuted this round and last

| hypothesis | result |
|---|---|
| zink adds a presentation blit | no - `PVR_JOB_TRACE` shows 3.00 jobs/submit, ~2.9 jobs/frame, one geom+PR+frag submit |
| sampling a linear texture is slow | only 1.5x fill (`vktex`), not 4.3x |
| vsync caps the client | no - 113 FPS at 160x120 |
| Xwayland copies frames | no write/SHM/memcpy in its profile |
| swapchain images are tiled | no - linear-only driver |

## Remaining candidates

1. Buffer movement per frame in the WSI - if the swapchain image is not the presented buffer,
   something moves 2 Mpix per frame outside the render.
2. weston's composite reading the client's linear buffer with a bad access pattern.
3. A detile/repack somewhere in Xwayland -> weston -> KMS.

Instrument: same client through compositor vs KMS at matched sizes, counting **bytes moved**, not jobs.
