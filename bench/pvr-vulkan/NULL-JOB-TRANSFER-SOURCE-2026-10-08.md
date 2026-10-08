# FOUND: the per-pass syncobj churn is a userspace fence-forwarding routine

## The trace

Instrumented the two `transfer` call sites in `vk_drm_syncobj.c` with a backtrace - **neither was ever
hit**, ruling out the whole generic runtime path. The winsys had it:

**`pvr_drm_winsys_null_job_submit()` (`pvr_drm_job_null.c:41`) - and it makes no kernel call at all.**
It is purely a userspace fence-forwarding routine built from DRM syncobj operations:

```c
if (wait_count == 1) {
   drmSyncobjTransfer(fd, dst->syncobj, signal_value, src->syncobj, wait_value, 0);  /* 1 transfer */
   return VK_SUCCESS;
}
drmSyncobjCreate(fd, ..., &tmp_syncobj);                                /* 1 create  */
for (i = 0; i < wait_count; i++)
   drmSyncobjTransfer(fd, tmp_syncobj, i+1, src[i]->syncobj, ...);      /* N transfers */
drmSyncobjTransfer(fd, dst->syncobj, signal_value, tmp_syncobj, wait_count, 0);  /* 1 transfer */
drmSyncobjDestroy(fd, tmp_syncobj);                                     /* 1 destroy */
```

## Matches the measured counts exactly

`PVR_SUBMIT_MIX=1 MODE=empty`:

```
[mx] null=400 waits:0=0 1=0 2=400
```

**One null job per frame with 2 waits** -> create + 2 transfers + 1 transfer + destroy = **3 transfers,
1 create, 1 destroy per frame** - precisely the 3 transfers + 3 creates + 3 destroys measured per
empty pass. (Full render: `null=800 ... 1=400 3+=400`, two null jobs per frame, one on the N+1 path.)

## Why this is the right thing to attack

* **Userspace-only.** No GPU work is submitted, so its entire cost is DRM ioctls - exactly the
  0.456 ms/frame of kernel time measured for an empty pass.
* **Scales with wait count**; the many-to-many path is O(N+1) ioctls.
* **The kernel already takes sync handles directly** (`DRM_PVR_SYNC_OP_FLAG_SIGNAL` in
  `pvr_drm_job_render.c`), so forwarding fences in userspace is a choice, not a requirement. If the
  null job's signal came from the kernel, or the null job were skipped when there is no work to order,
  the churn would disappear.

## Next

Test directly: handle `wait_count == 2` on the fast path without a temp syncobj (two sequential
transfers are NOT equivalent to a chained wait, so this needs care), or skip the null job where its
signal is not observable. **`MODE=empty` + `PVR_SUBMIT_MIX` make the result measurable in seconds**;
the target is the vendor's 0.003 ms.
