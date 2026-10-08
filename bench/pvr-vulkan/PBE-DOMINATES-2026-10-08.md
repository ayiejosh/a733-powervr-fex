# The bottleneck is a per-sample PBE cost that dominates tile traffic entirely

## No depth/stencil in the measured pass

`vkrender`'s render pass is **colour-only** (`attachments[2] = { colour, resolve }`,
`attachmentCount = samples > 1 ? 2 : 1`), blending disabled, loadOp=CLEAR/storeOp=STORE. So the 2.45x is
**without any ISP depth work** - hidden-surface removal is excluded before any new test.

## Tile load/store is not the cost; the asymmetry is the finding

| configuration | open | vendor | ratio |
|---|---|---|---|
| default (CLEAR/STORE) | 300.5 | 735.4 | 2.45x |
| **both dontcare** | **294.9** (unchanged) | **1030.0 (+40%)** | **3.49x** |
| LOADOP=load | 228.4 (-24%) | 759.6 (+3%) | 3.33x |
| SAMPLES=4 | 62.4 | 282.2 | 4.52x |

* **Vendor +40% from removing tile load/store; open +0%.** The driver *does* handle DONT_CARE
  (`pvr_arch_cmd_buffer.c:4276-4278`, `:4450`), so this is not a missing optimisation - it means
  **tile traffic is simply not the open driver's bottleneck**; a dominant cost hides a 40% traffic
  reduction inside it.
* **`LOADOP=load` costs the open driver 24% and the vendor 3%.** The vendor's tile load is nearly
  free; the open driver pays for it.

## The picture

* Compute **1.12x**; graphics **2.45x**.
* Colour-only, no depth, no blending, no MSAA at 1 sample.
* Tile load/store removable: vendor +40%, open +0%.
* Bytes/pixel flat r8 -> rg16.

**The open driver's graphics path is dominated by a per-sample PBE cost that (a) compute never pays,
(b) is independent of bytes and tile traffic, and (c) worsens super-linearly with samples (4.87x for
4x vs the vendor's 2.58x).**

## Next

Exclusion from outside is exhausted; the next instrument must look at what the driver programs into
the PBE/ISP state for a fragment job and compare against what the same hardware needs:
`pvr_arch_job_render.c`'s PBE/ISP setup and `pvr_arch_cmd_buffer.c`'s
`pvr_setup_isp_faces_and_control`. **Two sharp discriminators: `SAMPLES=4` (4.52x) and
`LOADOP=load` (3.33x).**
