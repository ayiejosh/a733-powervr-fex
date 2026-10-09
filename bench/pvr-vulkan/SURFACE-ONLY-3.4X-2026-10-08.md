# The render deficit is 100% per-surface, 0% per-covered-pixel - non-discard proof

After the discard-control failure, measured the one variable needing no discard at all: **how much of the
surface is actually drawn** (`AREA` shrinks the render area; no fragment survival changes).

| covered area | open | vendor |
|---|---|---|
| full (4.19 Mpix) | 13.898 ms | 5.778 ms |
| quarter (1.05 Mpix) | 14.127 ms | **4.124 ms** |
| sixteenth (0.26 Mpix) | 14.337 ms | 4.093 ms |
| **tiny (0.004 Mpix, 1/1000th)** | **14.177 ms** | **4.284 ms** |

## The cleanest characterisation of the render deficit in this session

* **The open driver is FLAT.** Drawing 1/1000th of the surface costs the same 14 ms as drawing all of it.
  **100% per-surface, 0% per-covered-pixel.**
* **The vendor has both terms**: drops 29% from full to quarter (5.778 -> 4.124) then plateaus -
  **~4.1 ms surface floor + ~0.53 ms/Mpix of coverage**.
* **Surface floor ratio: 14.0 / 4.1 = 3.4x**, and the open driver has *no* coverage term at all.

## What it means

**The open driver pays a per-tile cost for every tile of the surface, whether or not anything is drawn
in it, and that per-tile cost is ~3.4x the vendor's.** At full coverage both pay it; at low coverage the
vendor's bill falls away and the open driver's does not.

Consistent with the tile-geometry code (`num_tiles_x/y`, `x_tile_max/y_tile_max` from the full surface)
and with **`process_empty_tiles = 1`** in the driver's own tile trace - it is configured to process empty
tiles.

## Why this is trustworthy where the discard measurements were not

**No fragment survival changes.** `AREA` alters only the render area, so both drivers run the same
shader, same tile count, same fixed-function path - the only difference is how many pixels are covered.
There is no compiler-eliminable case for either driver to special-case, which is exactly what broke
`FRAGDISCARD` and `PATTERNDISCARD`.

## Supersedes

| claim | status |
|---|---|
| "raw render 2.5-4x down / fill-rate deficit" | **wrong** - not fill-rate at all |
| "3.68x per-tile" | superseded - the fit assumed a coverage term the open driver lacks |
| "PBE write 3.2x" | **withdrawn** - discard-contaminated |
| "Tiler 1.84x" | discard-based; directionally consistent but not the same measurement |

**Reliable statement: open 14.0 ms surface-only vs vendor 4.1 ms surface + 0.53 ms/Mpix.**
