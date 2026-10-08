# 15 syncobj ioctls per frame - target (1) confirmed, and a correction to my own claim

`strace -c -e trace=ioctl` on `vkrender 64 200`:

| ioctl | total (200 frames) | per frame |
|---|---|---|
| `DRM_IOCTL_SYNCOBJ_CREATE` | 1004 | **5.02** |
| `DRM_IOCTL_SYNCOBJ_TRANSFER` | 1000 | **5.00** |
| `DRM_IOCTL_SYNCOBJ_DESTROY` | 999 | **5.00** |
| `DRM_IOCTL_SYNCOBJ_WAIT` | 401 | 2.00 |
| `DRM_IOCTL_PVR_SUBMIT_JOBS` | 400 | 2.00 |
| `DRM_IOCTL_PVR_VM_MAP` | 307 | 1.54 |
| `DRM_IOCTL_PVR_CREATE_BO` | 307 | 1.54 |

**15 syncobj churn ioctls per frame** (5 create + 5 transfer + 5 destroy), plus 2 waits, 2 submits.

## Confirms target (1)

"creates and destroys vk_sync objects per job in pvr_arch_queue.c (26 create/destroy sites)" -
**confirmed: 5 creates and 5 destroys every frame.** Pooling removes 10 of the 15 churn ioctls.

## Correction to my previous note

I said last round "the cost is the wait, not create/destroy" because the process blocks in
`drm_syncobj_array_wait_timeout`. **Too strong.** `wchan` shows where a thread *sleeps*, not where its
CPU time goes. The 0.39 ms/frame of `sys` is spread across all 15 churn ioctls plus the waits; 15
ioctls at ~10-20 us each is ~0.15-0.30 ms/frame on its own, so the churn is a large share. **Pooling /
timeline-backing is worth doing after all.**

Also: **5 `SYNCOBJ_TRANSFER` per frame.** Transfers convert a fence between syncobj types, so a
timeline-backed path avoiding the binary/timeline conversion could remove those 5 as well.

## The vendor comparison could not be made with strace

`strace` on the vendor run captured unrelated processes (AMDGPU and MSM ioctls, opens of
`renderD184`-`renderD191`) - impossible for this board. This host runs emulation layers, so
system-wide syscall tracing is untrustworthy here. **The vendor's DRM ioctl count per frame is still
unmeasured**, and it is the number that would say whether the vendor pays none of this or the same.

## Next

Pool or timeline-back the per-job syncs so 5+5 create/destroy and 5 transfers collapse. Bounded by the
measured kernel time: 0.27 ms/frame extra vs the vendor, 0.39 ms/frame total `sys` (~1.6% of a core at
60 fps). Worth doing - and it will not close the 2.45x per-pixel deficit.
