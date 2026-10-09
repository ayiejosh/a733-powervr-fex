# NEARLY REPORTED A PHANTOM 2.8x SPEEDUP - the macrotile grid is structurally fixed

Tested whether the macrotile grid is a lever. Forcing `PVR_MTILES=2` appeared to make the 2048 render
**2.8x faster** (13.920 -> 4.957 ms), which looked like the answer to the 3.3x per-surface deficit.

**It was a failed render.** Correctness check:

```
2048, PVR_MTILES=2:   RESULT: FAIL - 4194304/4194304 pixels wrong
                      (first at 0,0: want 2,2,64,255 got 0,0,0,0)
2048, PVR_MTILES=1:   RESULT: FAIL - all pixels wrong
512,  PVR_MTILES=1..8: PASS - 262144/262144 correct
```

**All-zero output - nothing was drawn**, so of course it was quick.

## Why the grid cannot be reduced

`pvr_arch_rt_mtile_info_init()` defines offsets for **exactly four** macrotiles:

```c
info->mtile_x1 = DIV_ROUND_UP(info->num_tiles_x, 8) * 2;   /* offset of macrotile 1 */
info->mtile_x2 = info->mtile_x1 * 2;                       /* non-simple path */
info->mtile_x3 = info->mtile_x1 * 3;
```

**Three offsets plus the origin = four macrotiles**, so `mtiles_x/y` must equal 4 for the grid to cover
the surface. Anything smaller leaves part of the surface uncovered - exactly the observed all-zero
output.

**At 512 the smaller grids still passed** because that surface is small enough that the under-coverage
does not reach the verified region - precisely how a wrong knob looks correct on a small test and is
broken at the size that matters.

## Correction

**There is no macrotile-grid lever.** `mtiles_x/y = 4` is structurally required. The 2.8x "speedup" is
withdrawn; it measured an empty render. Reverted; tree clean; 2048 verification restored to PASS.

## The lesson

**A speedup measurement without a correctness gate is worse than no measurement.** Every timing this
session should carry its verification. The discard controls had it (and that's how their failure was
found); this one did not until I checked - and the check took one command.
