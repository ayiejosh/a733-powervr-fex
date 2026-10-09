# The open driver's submit cost is ioctl count (2026-10-06, later round)

This corrects an earlier conclusion in `results-2026-10-06-render-gap.md`, which said
the per-frame ioctl count was "a red herring" and that kernel time was negligible.
That was wrong, and it was wrong because of a bad measurement.

## The bad measurement

`strace -T` reports per-syscall duration. Reading it gave **~0.1 µs per ioctl**, which
is not plausible for a real syscall, and it was taken at face value instead of being
sanity-checked. Everything downstream of it inherited the error.

## The correct measurement

Regress `submit` (from `PVR_TIMING=1`, which brackets `vkQueueSubmit`) against the
ioctl count per frame (from `strace -f -e trace=ioctl`, counting two runs and dividing
the delta by the frame delta):

| MODE | ioctls/frame | submit ms |
|---|---|---|
| copy | 7.0 | 0.073 |
| render | 19.6 | 0.167 |
| both | 26.6 | 0.185 |
| empty | 18.0 | 0.294 (outlier, noisy) |

`copy` and `both` pin a clean line:

```
submit_ms = 0.033 + 5.7 µs × ioctls_per_frame
  copy:  0.033 + 5.7e-3 × 7.0  = 0.073   measured 0.073
  both:  0.033 + 5.7e-3 × 26.6 = 0.185   measured 0.185
```

**~5.7 µs per ioctl, with a 0.033 ms base.** The ioctl count accounts for the entire
submit gap against the vendor (5 ioctls/frame → 0.046 ms measured).

## Where the 26.6 ioctls go (MODE=both, per frame)

| ioctl | per frame | µs |
|---|---|---|
| `SYNCOBJ_CREATE` | 5 | 28 |
| `SYNCOBJ_TRANSFER` | 5 | 28 |
| `SYNCOBJ_DESTROY` | 5 | 28 |
| `SYNCOBJ_WAIT` | ~3 | 17 |
| `PVR_SUBMIT_JOBS` | 2 | 11 |
| `PVR_CREATE_BO` | 1.3 | 8 |
| `PVR_VM_MAP` | 1.3 | 8 |
| `PVR_GET_BO_MMAP_OFFSET` | 1.3 | 8 |

The 15 syncobj ioctls are **~85 µs/frame**, over half the count.

## Where the syncobjs come from

- **2 `vk_sync_create` per render pass per view** — `pvr_arch_queue.c:271-287` creates
  a geometry signal sync and a fragment signal sync.
- **2 destroys** — `pvr_update_job_syncs` (`pvr_arch_queue.c:235-252`) destroys the
  previous pass's pair on every pass.
- **The `SYNCOBJ_TRANSFER`s are not pvr-specific.** They come from Mesa's generic DRM
  syncobj backend, `src/vulkan/runtime/vk_drm_syncobj.c:560` (the fence signal path),
  so any change there affects every DRM driver.

## What this is worth, and what it is not

Eliminating the syncobj churn entirely is ~85 µs of a 1050 µs gap — **8%**. It is not
free: reusing a binary syncobj requires a reset (1 ioctl) instead of create+destroy
(2 ioctls), so a pool saves at most 1 ioctl per sync per frame, and the correctness
risk is real (an earlier attempt at a similar lifetime change — caching the CSB BO —
segfaulted and had to be reverted).

**The ceiling for UMD work here is roughly 10-12% of the gap.** That does not change
the conclusion in `results-2026-10-06-fw-trace.md`: `gpu_wait` (0.581 ms of the gap)
is untouched by any of this and is dominated by the firmware's TA→3D transition.
