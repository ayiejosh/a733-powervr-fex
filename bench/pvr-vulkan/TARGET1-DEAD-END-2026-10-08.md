# Target (1) is a dead end: timeline-backing makes the ioctl count worse

The objective's target (1): "The driver creates and destroys vk_sync objects per job in
pvr_arch_queue.c (26 create/destroy sites) - **pool or timeline-back them**."

Applied the verified 13-anchor migration and measured syscalls rather than assuming:

| ioctl (200 frames) | baseline | timeline migration |
|---|---|---|
| `SYNCOBJ_CREATE` | 1004 | **805** (-20%) |
| `SYNCOBJ_DESTROY` | 999 | **800** (-20%) |
| `SYNCOBJ_TRANSFER` | 1000 | **1000** (unchanged) |
| `SYNCOBJ_WAIT` | 401 | **600** (+50%) |
| `SYNCOBJ_RESET` | - | **399** (new) |
| **total** | **3404** | **3604 (worse)** |

Performance: 2048 fill ~300 -> 287.5 Mpix/s; 64x64 x 2000 wall 2.063 -> 2.003 s; sys 0.785 -> 0.604 s
(-23%); ms/frame 1.001 -> 0.949.

**Timeline-backing does not remove the churn.** It trades 199 creates + 199 destroys for 399 resets,
adds 199 waits, and leaves all 1000 transfers untouched. sys drops 23% but the ioctl count rises 6%
and throughput is unchanged-to-slightly-worse. Reverted; tree clean.

## Why it cannot work as hoped

`VK_SYNC_FEATURE_CPU_RESET` is offered only by `vk_sync_timeline` (`vk_sync_timeline.c:58`), **not** by
the DRM syncobj type - a binary syncobj cannot be reset and reused at all. The timeline *does* pool
points (`alloc_point_locked` reuses `state->free_points`, `:190`), but each pooled point still costs a
reset and the waits rise, so pooling does not pay for itself here.

## Closed

**Target (1) as written is closed: measured and not worth it.** The churn is real (15 syncobj
ioctls/frame, ~0.27 ms/frame kernel vs the vendor), but the proposed remedy does not reduce it. Anyone
revisiting should start from these syscall counts, not from the assumption that timeline-backing helps.

0.27 ms/frame is ~1.6% of a core at 60 fps, so even a perfect fix could not move the 2.45x per-pixel
deficit - and the realistic fix makes things marginally worse.

## Next

Stop spending on the sync path. The per-pixel 2.45x render deficit and the 5.4-8.9x present deficit
are the whole story; the render half is in Mesa's scope.
