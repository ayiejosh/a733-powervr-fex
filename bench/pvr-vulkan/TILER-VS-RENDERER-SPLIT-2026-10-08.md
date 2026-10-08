# Tiler vs Renderer SPLIT - the deficit is predominantly the RENDERER (3.90x)

The guide said distinguishing Tiler from Renderer needs PVRtune. It does not - `rasterizerDiscardEnable`
does it. Added `DISCARD=1` to vkrender (static pipeline state): geometry and tiling still run, no
fragments are shaded, nothing reaches the PBE.

| 2048x2048 | open | vendor | ratio |
|---|---|---|---|
| full render | 14.758 ms | 5.692 ms | 2.59x |
| **DISCARD (Tiler only)** | **7.509 ms** | **3.830 ms** | **1.96x** |
| **Renderer (full - discard)** | **7.25 ms** | **1.86 ms** | **3.90x** |

## What this establishes

* **The Renderer (fragment/PBE) carries the larger deficit: 3.90x**; the Tiler is 1.96x.
* **The two drivers have different shapes**: the vendor's Tiler dominates (3.83 vs 1.86 ms of Renderer),
  while the open driver's are equal (7.51 vs 7.25). So the open driver's Renderer is disproportionately
  expensive relative to its own Tiler, not just in absolute terms.
* **This answers the guide's question by measurement**, without PVRtune: of the five resources, ALU and
  Texturing were already excluded, ISP work is minimal, and Tiler and Renderer are now **separately
  measured** rather than inferred.

## Where the render deficit stands

**From "the render is 2.5-4x down" (wrong model - it was never fill rate) to: a per-tile cost of which
the fragment/PBE side is 3.90x and the geometry/tiling side 1.96x.** The most localised the render half
has been.

Everything previously excluded still applies to the Renderer side: bytes/pixel flat, load/store
irrelevant, format-independent, sample-rate mode irrelevant, shader instruction count irrelevant. **What
remains for the Renderer is its fixed-function per-sample/per-tile work - now the single best-defined
target in the objective.**

## Instruments

`DISCARD=1` (new), alongside `AREA`, `MODE=empty|render|copy`, `SAMPLES`, `LOADOP`, `STOREOP`, `TILING`,
`FORMAT`, `EXPORTABLE`. Any Renderer-side change should move 7.25 ms toward 1.86 ms.
