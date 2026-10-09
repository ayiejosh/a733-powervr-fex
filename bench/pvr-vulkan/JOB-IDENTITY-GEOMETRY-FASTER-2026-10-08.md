# Job identity resolved — the geometry/TA job is FASTER on the open driver

Identified the three jobs by elimination across three modes:

| job | normal | `DISCARD` (no rasterization) | `MODE=empty` (no draw) | interpretation |
|---|---|---|---|---|
| **A** | 0.35 ms | ~0.40 | 0.24 | **geometry / TA** — tiny, vanishes without a draw |
| **B** | **9.31 ms** | 2.77 | — | fragment-side (raster-dependent) |
| **C** | **13.01 ms** | 6.45 | — | fragment-side (raster-dependent) |

Cross-referenced with the vendor's own fence timelines (`VV`/`PV`/`QV`, from its `rogue-ta3d` /
`rogue-tq3d` / `rogue-cdm` contexts):

| job | open | vendor | ratio |
|---|---|---|---|
| **A (geometry/TA)** | **0.35 ms** | 0.49 ms | **open is FASTER** |
| **B (fragment-side)** | **9.31 ms** | 1.94 ms | **4.8x slower** |
| **C (fragment-side)** | **13.01 ms** | 5.31 ms | **2.45x slower** |

## The localisation

* **The geometry / tile-accelerator job is FASTER on the open driver** (0.35 vs 0.49 ms). The geometry
  path, the vertex stage and the TA's primitive binning are **not** the problem.
* **The entire deficit is fragment-side**, and **not uniform within it**: 4.8x on one job, 2.45x on the
  other.
* Within job C, `DISCARD` separates ~6.45 ms of tile/coverage processing from ~6.56 ms of
  fragment/raster.

## What this changes

**"The render is 3.3x slower per surface" is too coarse.** Precisely: *geometry is faster, and the
fragment-side jobs are 2.45-4.8x slower.* That is a different problem statement, pointing at the
fragment path (PBE, tile coverage/storage, fragment setup) rather than the tiler.

It also revises the earlier discard-based "Tiler 1.84x / PBE 3.2x" split, which treated the two as
comparable. **At job level, geometry is *ahead* of the vendor.**

## Instrument note

**This measurement is worth trusting more than any wall-clock FPS figure this session**: per-job
durations from kernel timestamps, three frames per run, repeating to within 1% (9.31/9.34/9.41 and
13.01/13.00/13.11). **Far above the ~25% wall-clock noise floor.**
