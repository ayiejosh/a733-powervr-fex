# NEW INSTRUMENT: per-job kernel timing — and the deficit is NOT uniform

## Found while hunting vendor debugfs

The vendor module creates `/sys/kernel/debug/pvr/` (`status`, `driver_stats`, `apphint/`, `buildvar/`)
with **`EnableFTraceGPU`** and **`HWPerfClientFilter_Vulkan`**. Setting `EnableFTraceGPU=Y` enables
in-kernel GPU tracepoints, and the useful ones exist for **both** drivers:

```
gpu_scheduler:drm_sched_job / drm_run_job / drm_sched_process_job   (open, via drm_sched)
pvr_fence:pvr_fence_enable_signaling / pvr_fence_signal_fence       (vendor, own scheduler)
```

**`drm_run_job` → `drm_sched_process_job` gives per-job durations with microsecond timestamps on the open
driver, with no kernel change.** This is the "TA/3D phase timing" instrument I listed as doable — it
already existed.

## Per-job durations at 2048 (open)

| entity | durations |
|---|---|
| A | 0.40 / 0.35 / 0.35 ms |
| B | 0.41 / **9.31** (9.34, 9.41) |
| **C** | **13.01** (13.00, 13.11) — **critical path** |

Attribution **within** the open driver by disabling rasterization (valid: same driver, comparing job
durations not output):

| 2048, job C | duration |
|---|---|
| normal | **13.01 ms** |
| `DISCARD=1` (no rasterization) | **6.45 ms** |
| `MODE=empty` (no draw) | 0.42 ms |

**So the critical job splits ~6.45 ms geometry/tiling + ~6.56 ms fragment/raster — 50/50.** Vindicates the
earlier open-driver split (6.99/6.76); the discard contamination affected the *cross-driver* ratio, not
the open driver's own.

## Per-job, cross-driver — the deficit is NOT uniform

Vendor per-queue durations from its own fence tracepoints (enable → signal):

| 2048 | open | vendor | ratio |
|---|---|---|---|
| job A | 0.35 ms | 0.49 ms | **open is FASTER** |
| **job B** | **9.31 ms** | **1.94 ms** | **4.8x slower** |
| job C | 13.01 ms | 5.31 ms | **2.45x slower** |
| frame | 13.78 ms | 5.607 ms | 2.46x |

**The vendor's fence contexts name its architecture**: `rogue-ta3d` (tile accelerator), `rogue-tq3d`
(transfer), `rogue-cdm` (compute), plus per-queue `VV`/`PV`/`QV` timelines — **3 jobs per frame**,
matching geometry/PR/fragment.

## What this changes

1. **The deficit is not a uniform per-tile slowdown** — one job is *faster* on the open driver while
   another is 4.8x slower. The single-cause framing needs this qualification.
2. **The critical path is job C at 2.45x**, split evenly between geometry/tiling and fragment/raster.
3. **Job B at 4.8x is the most disproportionate.** If job B is the PR job, target (5)'s "PR is a
   non-issue" needs revisiting — Mesa's comment says PRs are not *performed* when unnecessary, but the
   job is still scheduled and its 9.31 ms is real.
4. **A new reliable instrument exists** — three frames per run, microsecond timestamps, far above the
   ~25% wall-clock noise floor.

## Next

Identify which open-driver job maps to the vendor's `PV` timeline and why it is 4.8x slower. The
`gpu_scheduler` trace includes a filter field and the driver's submit path knows the job type, so this
should be separable without guessing.
