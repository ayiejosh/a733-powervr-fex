# Per-surface cost confirmed at ~3.3x by a second, independent method

Both previous per-surface figures rested on varying the *covered area* (`AREA`). This test does the
opposite - **holds the drawn pixels constant and varies the surface** - so it needs no discard and no
area assumption.

`size=512 full`, `size=2048 AREA=quarter`, `size=4096 AREA=sixteenth` all draw **exactly 262144 pixels**:

| surface | tiles | open | vendor | ratio |
|---|---|---|---|---|
| 512 | 1,024 | 1.848 ms | 0.640 ms | 2.89x |
| 2048 | 16,384 | 14.543 ms | 4.427 ms | 3.28x |
| 4096 | 65,536 | **55.772 ms** | **15.619 ms** | **3.57x** |

## Established

* **Identical drawn work costs 1.8 ms on a small surface and 55.8 ms on a large one** - a 30x difference
  from surface size alone. Per-surface, confirmed without area-coverage reasoning.
* **Both drivers scale the same way** (open 7.9x then 3.8x; vendor 6.9x then 3.5x). The per-surface cost
  is **architectural in both** - the open driver is not avoiding/doing something extra, it does the same
  work at **~3.3x the cost per tile**.
* **Agrees with the independent `AREA`-fit figure (14.0 / 4.1 = 3.4x).** Two methods, different
  manipulations, same answer. **The most solid number in the session.**

## Rules out

* **"The driver processes empty tiles unnecessarily"** - the vendor scales identically, so this is not an
  open-driver-specific defect; `process_empty_tiles = 1` is normal here.
* **"It is a fill-rate problem"** - drawn pixels are identical across all three rows.
* Shows the per-surface cost is **catastrophic at large surfaces**: 55.8 ms for 262144 drawn pixels is
  4.7 Mpix/s of effective fill.

## The reliable statement

**Per tile of surface the open driver costs ~3.3x the vendor's, and every frame pays it for every tile
regardless of coverage. At full-surface draw the totals are 13.9 vs 5.6 ms; at a tiny draw 14.2 vs
4.3 ms.** Everything else reported for the render this session was either discard-contaminated or a fit
assuming a coverage term the open driver lacks.
