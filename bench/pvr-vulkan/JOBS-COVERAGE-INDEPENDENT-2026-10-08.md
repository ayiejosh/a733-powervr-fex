# The fragment-side jobs are 100% coverage-independent — confirmed at job level

Using the new per-job instrument (1% repeatability), traced the three jobs at four coverage levels on a
2048 surface:

| coverage | job A (geometry/TA) | job B | job C |
|---|---|---|---|
| full (4.19 Mpix) | 0.35 ms | **9.32 ms** | **13.12 ms** |
| quarter (1.05 Mpix) | 0.37 | **9.36** | **13.16** |
| sixteenth (0.26 Mpix) | 0.35 | **9.33** | **13.15** |
| **tiny (0.004 Mpix, 1/1000th)** | 0.36 | **9.34** | **13.14** |

**Both fragment-side jobs take identical time whether 4.19 Mpix or 0.004 Mpix are drawn.** Job B is
9.32–9.44 ms and job C 13.12–13.27 ms across all four levels.

## Pinned down

* **The open driver's fragment-side work sweeps the entire surface at full cost regardless of coverage** —
  measured per job, not inferred from frame totals, and matching the independent frame-level `AREA` sweep.
* **The vendor does have a coverage-dependent component**: its frame drops 5.78 → 4.12 ms from full to
  quarter (1.66 ms) then plateaus. **The open driver has no coverage term at all** (flat 13.9–14.2 ms).
* Comparison: **vendor ≈ 4.1 ms surface + 1.7 ms coverage; open ≈ 13.5 ms surface and no coverage work.**

## The precise open question

**Why does the same surface work cost ~3.3x more on the open driver, when its geometry job is *faster*
than the vendor's?** The deficit is entirely in the two fragment-side jobs (4.8x and 2.45x), both
coverage-independent, with geometry ahead.

That is far sharper than "the render is slow", and it is answerable: **any change can now be judged by
its effect on job B and job C specifically, in one run, at 1% repeatability.**
