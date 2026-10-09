# The render deficit is the PBE WRITE PATH (3.41x) - full three-way decomposition

Added `FRAGDISCARD=1`: a fragment shader that runs and then discards every fragment, so the ALU work
happens but nothing reaches the PBE. With `DISCARD=1` (tiler only) that splits the render three ways:

| component (2048x2048) | open | vendor | ratio |
|---|---|---|---|
| Tiler (geometry + tiling) | 6.989 ms | 3.818 ms | 1.83x |
| Shader execution | 1.114 ms | 0.087 ms | **12.8x** |
| **PBE write** | **6.150 ms** | **1.805 ms** | **3.41x** |
| **total** | 14.253 ms | 5.710 ms | 2.50x |

## What it settles

* **Dominant deficit: the PBE write path, 3.41x - 4.35 ms of the 8.5 ms total render gap.**
* **The shader is disproportionately slow (12.8x) but tiny absolutely (1.1 ms).** This explains why
  every shader-side experiment was a measured null: removing 24 prologue instructions, the DOUTU
  sample-rate mode, and PCO codegen uniformity were all correct observations about a ~1 ms term.
* **The Tiler is 1.83x** - real, smaller.

## The PBE write is not the memory store

`STOREOP=dontcare` changed nothing and the cost is flat across r8 -> rgba8 -> rg16. So this 6.15 ms is
the PBE's **per-pixel processing** (packing, format conversion, tile-buffer writes) *before* any
system-memory store - not bandwidth. Consistent with everything measured: per-tile, per-sample,
format-independent, unaffected by load/store ops.

## The best-defined target in the objective

**The PBE's fixed-function per-pixel write processing, 3.41x the vendor's, ~6.15 ms per 2048x2048
surface.** Not the shader, not the tiler, not bandwidth, not the compositor.

## Instruments

`DISCARD=1` (tiler only) and `FRAGDISCARD=1` (shader without PBE) are new; with `AREA`, `MODE`,
`SAMPLES`, `LOADOP`, `STOREOP`, `TILING`, `FORMAT`, `EXPORTABLE` they separate six independent
components of a render. Any PBE-side change should move 6.15 ms toward 1.81 ms.
