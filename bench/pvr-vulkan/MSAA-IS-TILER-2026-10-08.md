# The MSAA cost is in the TILER (8.16x growth), not the PBE

Ran the three-way render split at 1x and 4x MSAA (`DISCARD` = tiler only, `FRAGDISCARD` = shader
without PBE, full = all):

| component (2048x2048, open) | s1 | s4 | growth |
|---|---|---|---|
| **Tiler (DISCARD)** | 6.972 ms | **56.862 ms** | **8.16x** |
| Shader | 1.105 ms | 0.684 ms | ~0 |
| **PBE write** | 5.778 ms | 10.870 ms | **1.88x** |
| total | 13.855 ms | 68.416 ms | 4.94x |

## Established

1. **The 4.87x MSAA penalty is a TILER effect** - the Tiler grows 8.16x under 4x MSAA while the PBE
   grows 1.88x. **Confirms Imagination's explanation by measurement**: "the increased on-chip memory
   footprint results in a reduction in tile dimensions... which then increases the vertex stages'
   overall processing cost."
2. **At 1 sample the deficit is the other way round: the PBE write dominates** - 5.778 vs the vendor's
   1.805 ms = 3.2x, while the Tiler is 6.972 vs 3.818 = 1.83x.

## Two configuration-dependent targets

| configuration | dominant deficit | size |
|---|---|---|
| **1x MSAA** | **PBE write path** | **3.2x** |
| **4x MSAA** | **Tiler** | **8.16x growth** |

Different units - which is why no single explanation ever fit: the render deficit is not one thing but
depends on the sample count.

## Instruments

`DISCARD=1` + `FRAGDISCARD=1` split a render three ways; `SAMPLES` moves the split between the two
units. A PBE fix should move the s1 PBE term (5.78 ms) toward 1.81 ms; a tiler fix the s4 Tiler term
(56.9 ms).
