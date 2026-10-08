# The bottleneck is the explicit-sync release wait - and it is not vsync

## Measured

Open stack, weston + Xwayland, zink client, 640x480, existing instrumentation:

```
[acq] AcquireNextImageKHR     avg=14.36 ms  max=68.35 ms   (frame 20.8 ms)
[rel] explicit-sync release   avg=13.41 ms  max=48.37 ms   images=3
[swap] 0.09-0.18 ms outside swap, 20-28 ms inside
FPS 49-55
```

The client renders in **under 0.2 ms** and blocks **~14 ms of a ~20 ms frame** in
`AcquireNextImageKHR`, waiting on the explicit-sync release (**13.4 ms**). The wait *is* the release.

## Ruled out by measurement

| candidate | evidence |
|---|---|
| vsync | `ZINK_PM_TRACE` confirms FIFO at interval 1, IMMEDIATE at 0; `vblank_mode=0` gives 44 -> 49-55 FPS, +15% not 32x |
| a copy | Xwayland profile: no `write`, no SHM, no memcpy traffic |
| the render | 0.2 ms/frame; KMS path's 396 Mpix/s implies ~1300 FPS at this size |
| CPU | client ~29% of a core, Xwayland ~2.3% |

## The chain that remains

`AcquireNextImageKHR` -> explicit-sync release -> **the compositor chain's turnover of swapchain
images.** With 3 images released ~13 ms apart, the return *rate* is fixed, which is why adding
images (target 2) does nothing: the rate is the constraint, not the count.

## Next instrument

Extend `WSIREL_TRACE`, which already brackets `wsi_drm_wait_for_explicit_sync_release()`, to log the
**sync value being waited on**. That distinguishes "the compositor signals late" from "the client
waits on the wrong point" - the two remaining explanations.

## Note on stale ground truth

The objective states Xwayland burns 56.8% of a core. Measured now it is **~2.3%**, with 7 of 8
threads idle, and its 61.7% futex share is blocking (idle waiting) rather than contention. That
figure should not be reused.
