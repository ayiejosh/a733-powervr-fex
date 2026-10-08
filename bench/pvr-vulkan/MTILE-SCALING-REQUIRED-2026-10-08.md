# The mtile sample scaling is required AND correct - two controlled negatives

Tested the suspect line (`tiles_per_mtile *= samples_in_xy`) with two variants:

| variant | s4 DISCARD | s4 full | s4 correct? |
|---|---|---|---|
| baseline (as shipped) | 56.862 ms | 68.416 ms | PASS |
| **scaling removed** | **27.268 ms** | **30.633 ms** | **FAIL** |
| **both tiles and tiles_per_mtile scaled consistently** | 56.982 ms | 67.557 ms | **PASS** |

## Established

1. **The sample scaling is semantically required.** Removing it makes 4x MSAA 2.2x faster and produces
   **wrong output** - the second variant's correctness is the control that proves it, not a perf number.
2. **The asymmetry is not the bug.** Scaling `num_tiles_x/y` and `x_tile_max` in step with
   `tiles_per_mtile` restores correctness and changes performance by nothing (56.982 vs 56.862 ms). So
   the geometry as shipped is already correct and consistent in effect.
3. **So the 2.7x excess tiler work under MSAA is NOT the mtile geometry.** The driver needs exactly the
   work it does; it does that work 2.2x slower than the vendor does the same correct work.

## Corrected framing

The previous note called this "a named line of code" as the suspect. **That was a reading-level
hypothesis and it is now refuted.** What survives is the measured fact:

**For the identical correct 4x MSAA render, the open driver's tiler takes 56.9 ms and the vendor's 12.6
ms - 4.5x - while at 1x they are 6.97 vs 4.11 - 1.7x.** So the MSAA *multiplier* differs (8.16x vs
3.06x) even though both produce correct output. **That is the finding; the code line was not the cause.**

Two variants tested, two negatives, tree clean, baseline restored (57.1 ms s4 DISCARD, bda PASS).
