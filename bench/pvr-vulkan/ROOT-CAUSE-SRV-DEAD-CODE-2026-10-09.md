# ROOT CAUSE: the vendor DRM name is `pvr`, so the mainline branch matches first — the srv winsys is dead code

## The bug, in `pvr_winsys.c`

```c
   if (strcmp(version->name, PVR_DRM_DRIVER_NAME) == 0) {
      result = pvr_drm_winsys_create(...);
#if defined(PVR_SUPPORT_SERVICES_DRIVER)
   } else if (strcmp(version->name, PVR_SRV_DRIVER_NAME) == 0) {
      result = pvr_srv_winsys_create(...);
#endif
   } else { error }
```

**The vendor kernel presents `name=pvr`** (measured with a raw `DRM_IOCTL_VERSION`: `name=pvr version=24.2.6603887
desc=Imagination Technologies PVR DRM`).

**If `PVR_DRM_DRIVER_NAME` is also `"pvr"`, the FIRST branch matches** and `pvr_drm_winsys_create()` runs — so
**the srv branch is never reached**, and enabling `imagination-srv` cannot help, **exactly as measured.**

## The complete chain

1. vendor kernel reports `name=pvr`
2. mainline name check matches first
3. mainline winsys issues `DRM_IOCTL_PVR_DEV_QUERY` (three sites in `winsys/powervr/pvr_drm.c`)
4. the vendor kernel doesn't implement the mainline UAPI (it has its own 33-ioctl surface + the srv bridge)
5. **0 physical devices**

## The fix shape — disambiguate two drivers that both call themselves `pvr`

| # | option | cost |
|---|---|---|
| **1** | **try the srv path first** when `PVR_SUPPORT_SERVICES_DRIVER` is defined, fall back to mainline | **one reordering** |
| 2 | distinguish by version (`24.2.6603887` vs mainline's scheme) | **fragile** |
| 3 | probe `DEV_QUERY`, fall back on `-EINVAL` | **most robust, more code** |

**Option 1 is the smallest change and directly tests the hypothesis.** If it works, the open userspace drives
the vendor kernel and gets **`pvr_srv_sync_type`** — **the 60% lever — with no timeline conversion at all.**

## Why this is a good place to have reached

**Three rounds ago this looked like "the vendor has a sync type we lack."** Now it is: **the code is present,
the build option enables it, and a single ordering bug makes it unreachable.**
