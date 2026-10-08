# CORRECTION: the MSAA cost is a real defect, not just architecture

**Last round I concluded from Imagination's guide that the 8.16x Tiler growth under 4x MSAA was expected
architecture behaviour. The vendor control shows that is wrong.**

| Tiler (`DISCARD=1`), 2048x2048 | s1 | s4 | growth |
|---|---|---|---|
| **open** | 6.972 ms | 56.862 ms | **8.16x** |
| **vendor** | 4.113 ms | 12.566 ms | **3.06x** |

**The vendor's tiler grows 3.06x under the same 4x MSAA; the open driver's grows 8.16x - 2.7x more.**
So MSAA is not simply buying the tile-count multiplication the guide describes; **the open driver does
2.7x more tiler work than the vendor for the identical configuration.** That is a defect.

## The suspect in the code

```c
/* pvr_arch_rt_mtile_info_init() */
info->mtile_x1 = DIV_ROUND_UP(info->num_tiles_x, 8) * 2;      /* 32 for a 2048 surface */
info->tiles_per_mtile_x = info->mtile_x1 * samples_in_x;      /* 64 under 4x MSAA */
info->tiles_per_mtile_y = info->mtile_y1 * samples_in_y;      /* 64 under 4x MSAA */
```

`tiles_per_mtile` is multiplied by the sample layout (2x2 for 4 samples) while `num_tiles_x` and
`x_tile_max` are **not** sample-scaled. **If the hardware already accounts for the sample layout, this
multiplication describes 4x more tiles per macrotile than exist** - producing exactly the 2.7x excess
measured. Whether it is required or redundant is now testable rather than a reading exercise.

## Why the correction matters

* **MSAA moves from the "expected behaviour" list back to the defect list**, with a measured size
  (2.7x excess tiler work) and a named line of code.
* The guide's value is preserved: it correctly explains *why* MSAA costs more (tile-dimension
  reduction), but **it does not say the open driver's 8.16x is right** - only a vendor control settles
  that, and it says 3.06x.
* First time this session that documentation produced a conclusion I retracted on measurement.
  **Documentation explains mechanisms; it does not establish what this driver should cost.**

## Next

Test directly: remove the `samples_in_x/y` multiplication on `tiles_per_mtile_x/y` and measure
`SAMPLES=4 DISCARD=1`. If the s4 tiler cost falls toward the vendor's 12.6 ms, that line is the bug; if
correctness breaks or nothing changes, the scaling is required and the excess is elsewhere. Seconds with
the existing instruments.
