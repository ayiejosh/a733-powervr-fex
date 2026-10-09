# DEFINITIVE: the srv winsys supports only vendor DDK 1.17 — this board runs 24.2

## The root cause, in `pvr_is_driver_compatible()`

```c
   /* Only the 1.17 driver is supported for now. */
   if (version->version_major != PVR_SRV_VERSION_MAJ ||
       version->version_minor != PVR_SRV_VERSION_MIN) {
      vk_errorf(NULL, VK_ERROR_INCOMPATIBLE_DRIVER,
                "Unsupported downstream driver version (%u.%u)", ...);
      return false;
   }
```

**Mesa's `pvrsrvkm` winsys is written against vendor DDK `1.17`.** The board's vendor kernel reports **`24.2`**
(the same `24.2.6603887` seen throughout). **`pvr_srv_winsys_create()` calls this first, it returns false, and
the function returns `VK_ERROR_INCOMPATIBLE_DRIVER` — hence 0 physical devices.**

## The full chain, by code AND measurement

| # | step | evidence |
|---|---|---|
| 1 | `imagination-srv=true` compiles the srv winsys in | 69 define occurrences — **verified** |
| 2 | `PVR_DRM_DRIVER_NAME="powervr"`, `PVR_SRV_DRIVER_NAME="pvr"` | **srv branch IS taken** — my "dead code" claim was wrong, corrected |
| 3 | srv path's first act: `pvr_is_driver_compatible()` requires DDK **1.17** | read |
| 4 | board's vendor kernel is **24.2** | measured via raw `DRM_IOCTL_VERSION` |
| 5 | `VK_ERROR_INCOMPATIBLE_DRIVER` → **0 physical devices** | measured |

## What this means

**The srv lever is real but NOT one option away.** The winsys exists and is complete, but targets a DDK **~23
major versions behind** this board. **The vendor UAPI changed between 1.17 and 24.2**, so using it means
**porting the winsys to the 24.2 bridge interface** — substantial work, not configuration.

**Ranking update: the srv path drops from "cheapest lever" to "a porting project."** The **two-line sync
conversion** (spec complete) returns as the most tractable route to the 60% lever — with the caveat that it has
failed three times and needs the recorded bisection approach.

## Worth knowing before someone tries it

**The version constant is the single gate.** Updating `PVR_SRV_VERSION_MAJ/MIN` alone would **compile and then
fail deeper** — the bridge interface must be ported too.
