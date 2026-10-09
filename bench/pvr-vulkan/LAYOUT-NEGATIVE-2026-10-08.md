# The MSAA sample layout is not the cost either - third negative

The guide says MSAA shrinks the tile in **one** axis (16x16 -> 16x8) implying a **2x** tile multiply, but
`pvr_get_samples_in_xy()` returns **(2,2)** for 4 samples - a 4x multiply. Changed it to (1,2):

| 4x MSAA, 2048 | s4 DISCARD | s4 full | correct? |
|---|---|---|---|
| baseline, layout (2,2) | 56.862 ms | 68.416 ms | PASS |
| **layout (1,2)** | **56.484 ms** | **65.652 ms** | **PASS** |
| layout (1,1) (prev round) | 27.268 ms | 30.633 ms | FAIL |

**Changing (2,2) -> (1,2) changes performance by nothing and still passes correctness.** The tile-count
multiply is not what drives the s4 cost. The only fast variant (1,1) is the incorrect one.

## Three lines tested and refuted

1. `tiles_per_mtile *= samples_in_xy` - required for correctness; removing it is fast but wrong.
2. scaling `num_tiles_x/y` + `x_tile_max` consistently - correct, zero performance change.
3. the sample layout itself, (2,2) vs (1,2) - both correct, zero performance change.

**The fact that survives all three: the open driver's 4x-MSAA tiler is 8.16x its 1x cost while the
vendor's is 3.06x, for identically correct output.** A real, reproducible inefficiency whose mechanism is
not the tile geometry.

## Where that leaves it

**Three reading-level hypotheses about the tiling code have been tested and refuted. The next step
should not be another reading hypothesis.** The measurable facts: 8.16x vs 3.06x growth, correct output,
Tiler-side, nothing in the tile-geometry computation explains it. That points at how tiles are
*processed* under MSAA (ISP/PBE per-tile state) - which needs either a CSB comparison against the
vendor's (closed) or the firmware-side view PVRtune would give.
