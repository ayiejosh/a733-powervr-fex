# The tile size is 16x16 (hypothesis refuted) - and the correctness gate caught a second phantom

**Hypothesis:** Imagination's guide says PowerVR tiles are 32x32, but the device info reports 16x16. If the
hardware were really 32x32 the driver would process **4x the tiles** - matching the 3.3x deficit. Tested by
forcing `PVR_TILE_SIZE=32`, with a correctness gate attached per the previous round's lesson:

| 2048 | frame | correctness |
|---|---|---|
| tile 16 (default) | 14.054 ms | **PASS** - 4194304/4194304 correct |
| **tile 32** | **5.040 ms** | **FAIL** - 4194304/4194304 wrong, got `0,0,0,0` |

**The tile=32 "2.8x speedup" is the same empty render as the macrotile case.** With 32-pixel tiles a 2048
surface has 64 tiles where 128 are needed, so half of it is never written.

**And the same trap reproduces:** `512` with tile=32 **PASSES** (262144/262144 correct), because that
surface is small enough that the under-coverage does not reach the verified region.

**So the device info's `tile_size = 16` is correct, the hardware is 16x16, and the hypothesis is
refuted.** Reverted; tree clean; 2048 verification restored to PASS.

## The systematic trap, now named

**Any change that shrinks the tile geometry produces a large apparent speedup, because less is rendered.**
It has now appeared **twice** (macrotiles, tile size), and both times **the small test passed while the
real size failed.**

**The correctness gate at the size that matters is what catches it - and the gate must run at the
largest size, not a convenient one.** One command, every time.

## Recorded so it is not attempted a third time

The tile geometry is **not a lever**:

| knob | value | why |
|---|---|---|
| tile size | 16x16 | device feature; forcing 32 under-covers the surface |
| macrotile grid | 4x4 | structurally required by the driver's four macrotile offsets |

Both are correct as shipped. The 3.3x per-surface deficit is **not** explained by either.
