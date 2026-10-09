# Outside research: Imagination's own guides, and what they confirm

## MSAA: the per-sample cost is documented, expected behaviour

From the [MSAA Performance](https://docs.imgtec.com/performance-guides/graphics-recommendations/html/topics/msaa-performance.html) page:

> "the increased on-chip memory footprint... results in a reduction in tile dimensions (32 x 32 ->
> 32 x 16 -> 16 x 16 pixels) as the number of samples taken increases. This in turn results in an
> increased number of tiles that need to be processed by the tile accelerator hardware, which then
> increases the vertex stages' overall processing cost."

**MSAA shrinks tile dimensions, multiplying tile count and the TA's per-tile work.** The driver's own
code agrees: *"When MSAA is enabled, the USC has to process half the tile (16x8 pixels)."* So **the 4.87x
MSAA cost is expected behaviour, not a defect** - and it explains why the cheapest shader suffers most
(the per-tile cost dominates when the shader is trivial).

The page also flags **"on edge blend"**: with an alpha-blended edge, "the blending is performed for each
sample by a shader in software", and it is **sticky**. `vkrender` has blending disabled, so it does not
apply to the benchmark, but is worth knowing for real clients.

## MRT: no spilling here

From [Using Multiple Render Targets Efficiently](https://docs.imgtec.com/performance-guides/graphics-recommendations/html/topics/using-multiple-render-targets-efficiently.html):
per-pixel render-target data must fit on-chip (**128 bits/pixel** max plus depth) or it spills, which is
"extremely expensive" and reduces USC occupancy. Measured: `partition_size = 512 dwords` over a 16x16
tile = **64 bits/pixel** - half the maximum. **No spilling; hypothesis does not apply.**

## The upstream EOT commit is not a lead

`bfcb88ea9995` "pvr: Order tile buffer EOT emits to be last" is **correctness-only** (dEQP
suballocation), Oct 2023, and the tree already carries a much newer `pvr_setup_emit_state`. Superseded;
not a performance fix.

## What it changes

* **Reclassifies the MSAA result**: 4.87x for 4x MSAA is what the architecture does, not a bug. Removed
  from the defect list.
* **Confirms the per-tile framing** from an independent source: the TA processes tiles and cost scales
  with tile count.
* **Eliminates the spill hypothesis** by arithmetic (64 vs 128 bits/pixel).
* Does **not** explain the 3.68x per-tile deficit on a non-MSAA fill - still open.

## Remaining unread leads

`Balancing Workloads on PowerVR to Eliminate Bottlenecks` and `On-Chip Memory Performance` are the two
pages not yet read that could bear on a per-tile deficit.
