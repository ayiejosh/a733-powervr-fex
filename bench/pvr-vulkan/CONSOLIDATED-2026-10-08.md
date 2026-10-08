# CONSOLIDATED: the gap, fully decomposed and measured

Everything below has a **vendor control** behind it; nothing is inferred from the open stack alone.

## The gap, decomposed

| term | open | vendor | ratio | where it lives |
|---|---|---|---|---|
| **per-pass syncobj overhead** | 0.968 ms/pass (0.456 kernel) | 0.003 ms/pass | **74x** | kernel UAPI - no null job exists |
| **render: per TILE** | 13.57 ms per 2048x2048 | 3.69 ms | **3.68x** | render per-tile path (Mesa/firmware) |
| **render: per drawn pixel** | 0.31 ms/Mpix | 0.47 ms/Mpix | **open is FASTER** | not a problem |
| **copy** | 4.006 ms | 2.858 ms | 1.40x | closest of the set |
| **compute** | 393.7 M inv/s | 441.9 M inv/s | 1.12x | not a problem |
| **present** | 45 FPS (640x480 windowed) | 1016 FPS | **22.6x** | WSI/compositor interaction |

## The corrected model

**The long-standing "raw render 2.5-4x down / fill-rate deficit" was wrong.** `AREA=quarter` showed the
cost barely moves when the drawn area shrinks 4x, and the open driver's **per-drawn-pixel** rate is
actually *better* than the vendor's. **It is not a fill-rate problem - it is a per-TILE cost.**

## What the per-tile cost is NOT (each measured)

| excluded | evidence |
|---|---|
| tile load/store | `LOADOP`/`STOREOP=dontcare` changed nothing |
| tile partition size | 6144 in both, exactly |
| tiles in flight | 6, matching `isp_max_tiles_in_flight` |
| tiling geometry | 16x16 tiles, `skip_init_hdrs=1` confirmed on |
| shader instruction stream | removing 24 prologue instructions changed nothing |
| PCO codegen | uniform across 1-instruction fill and 640-op shader |
| GPU clock | 1104000000 under both drivers |
| bytes/pixel | flat r8 -> rgba8 -> rg16 |
| memory layout | linear/exportable render at optimal's rate |
| user sample shading | DOUTU FULL->SELECTIVE measured null |
| ISP depth/HSR | the pass is colour-only, no depth attachment |
| EOT store | `STOREOP=dontcare` no effect; EOT fixed-size per device |
| pixel event PDS | fixed size from device info |

## The two real, actionable findings

1. **Per-pass syncobj overhead (74x, 0.456 ms kernel/pass).** Root cause found and proved:
   `pvr_drm_winsys_null_job_submit` is a userspace fence-forwarding routine (create + N+1 transfers +
   destroy), and it exists **because the mainline UAPI has no null job type** (`DRM_PVR_JOB_TYPE_NULL`
   is a stale comment; no handler in the kernel). Fix belongs in the kernel module. **Payoff measured
   at 2.5-5% of a real client frame** - real but second-tier.
2. **Render per-tile cost (3.68x).** The dominant render term, and **not yet localised to a specific
   operation** - every candidate reachable by configuration is excluded. Localising further needs the
   vendor's command stream for the same draw (closed, `libVK_IMG`) or a firmware-side counter.

## Honest status

**The objective is not met.** The gap is fully *decomposed* and two terms have *root causes*, but
neither has a landed fix: one needs a kernel UAPI addition (~5% payoff), the other is a per-tile
inefficiency whose specific operation is still unidentified. **The present path (22.6x) remains the
largest single term and is where the objective's original "787 vs 31" number actually lives.**

## Instruments left behind

`vkrender` (`TILING`, `EXPORTABLE`, `FORMAT`, `SAMPLES`, `LOADOP`, `STOREOP`, `AREA`, `MODE`, `BATCH`),
`vktex` (sampling), `vkheavy` (ALU-heavy), `cstp` (compute), `pvranimate` (KMS present), plus in-driver
traces `PVR_TILE_TRACE`, `PVR_SUBMIT_MIX`, `PVR_JOB_TRACE`, `ACQ_TRACE`, `WSIREL_TRACE`, `SWAP_TIMING`.
