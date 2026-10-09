# The per-pass syncobj cost is FORCED by the kernel UAPI - no null job exists

## The fix that does not build

Implemented the obvious fix: submit the forwarding as a kernel `DRM_PVR_JOB_TYPE_NULL` job carrying the
wait and signal `sync_ops`, replacing the userspace transfer chain (5 ioctls for 2 waits) with one
`DRM_IOCTL_PVR_SUBMIT_JOBS`.

```
error: 'DRM_PVR_JOB_TYPE_NULL' undeclared (first use in this function);
       did you mean 'DRM_PVR_JOB_TYPE_COMPUTE'?
```

**`DRM_PVR_JOB_TYPE_NULL` is referenced in a UAPI comment but never defined.** Both copies checked:

* `enum drm_pvr_job_type` defines only `GEOMETRY`, `FRAGMENT`, `COMPUTE`, `TRANSFER_FRAG` - in the
  kernel's UAPI header **and** the system header Mesa compiles against.
* `grep JOB_TYPE_NULL` over the whole kernel `powervr/` tree finds **only the comment**. There is **no
  null-job handler in the kernel at all.**

Reverted; tree clean.

## What it means

**The driver's userspace transfer chain is not a shortcut - it is the only mechanism available.** The
mainline `powervr` UAPI cannot express "signal this sync when these syncs complete", so Mesa builds it
from `DRM_IOCTL_SYNCOBJ_TRANSFER` chains. Hence:

* **0.456 ms/frame of kernel time per pass, 74x the vendor's 0.003 ms**
* **15 syncobj ioctls per frame**
* No Mesa-side fix keeps the ordering semantics: timeline-backing was measured and made it *worse*, and
  a kernel null job does not exist.

## The architectural difference, named precisely

Target (3) says the vendor uses a driver-native sync type where Mesa uses DRM syncobj operations.
**Correct, and now concrete:** `pvrsrvkm`'s UAPI provides a native sync and a job path that orders
syncs, so the vendor issues **zero** DRM syncobj ioctls (empty pass 0.003 ms). The mainline UAPI
provides neither.

**So the fix belongs in the mainline kernel module - in the objective's scope:** add a null/no-op job
type accepting `sync_ops` (the stale comment suggests it was once intended), or a sync-forwarding
primitive. Either removes the entire per-pass cost for every Mesa pvr client. **It is a kernel UAPI
change, not a Mesa patch - which is exactly why Mesa-side work never moved this number.**

## Objective status: three gaps, all measured against a vendor control

| gap | size | where |
|---|---|---|
| per-pass syncobj overhead | **74x** (0.46 ms kernel/pass) | kernel UAPI - fixable only there |
| draw | **2.13x** | per-sample PBE cost, in Mesa's scope |
| copy | **1.40x** | closest of the three |
| present | **5.4-8.9x** | WSI/compositor interaction |
