# The render deficit is GRAPHICS-SPECIFIC - compute runs at vendor speed

Built `cstp`, a compute throughput probe: dispatches W workgroups of 64 invocations, each doing a few
dependent ALU ops plus one store, with all iterations in one command buffer so a single submit is
timed. Same probe, same args, both drivers:

| workload | open | vendor | ratio |
|---|---|---|---|
| compute 1024x100 | 393.7 M invocation/s | **441.9 M invocation/s** | **1.12x** |
| compute 8192x100 | - | 453.7 M invocation/s | |
| graphics fill 2048 | ~300 Mpix/s | ~743 Mpix/s | **2.45x** |

**Compute is at 1.12x while graphics is at 2.45x.** The deficit is **graphics-specific**, which
eliminates a large class of explanations in one measurement:

* **Not the USC / shader execution** - compute uses the same shader cores and is at vendor speed.
  Independently confirms that removing 24 prologue instructions changed nothing.
* **Not the submission path** - compute submits through the same queue, BOs and syncs.
* **Not the memory path** - compute reads and writes buffers at vendor speed.

**What remains is the graphics-only pipeline: tiler / ISP / PBE.** Compute does not tile, does not run
the ISP's hidden-surface removal, and does not write through the PBE.

## Already excluded for that path

tile partition size (6144 both), tiles in flight (6, matching `isp_max_tiles_in_flight`), tiling
geometry (16x16, `skip_init_hdrs=1`), phantom count (1), bytes/pixel (flat r8->rg16), sample handling
(scales 4.87x for 4x).

## Remaining suspects

**The ISP's per-tile processing and the PBE's per-sample work** - the two things that (a) only graphics
uses, (b) scale with samples, and (c) are independent of bytes per pixel.

## Next

Remove the ISP from the equation: render with depth/stencil disabled and no depth attachment if
vkrender does not already, and compare the ratio. Deficit persists -> PBE; vanishes -> ISP's
hidden-surface removal. `SAMPLES=4` plus `cstp` as the graphics-only control make both directions
measurable in seconds.
