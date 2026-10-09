# Both jobs scale with tile count — a per-tile sweep signature

## The measurement

Per-job durations from the harness's one-frame phase, open driver, `vkrender`:

| size | job A | job B | total |
|---|---|---|---|
| 512 | 0.339 ms | 0.991 ms | 1.77 |
| 1024 | 2.584 | 2.716 | 5.78 |
| 2048 | 9.437 | 12.103 | 22.37 |
| 4096 | 36.694 | 52.773 | 91.96 |

**Both jobs grow ~4× per surface doubling at the larger sizes.** That is the signature of a **per-tile
sweep** — the cost tracks tile count, exactly what the PR job's "full fragment-shaped pass over the tile
range" would do.

**It supports the fix direction:** if the PR job's cost is a tile sweep, **shortening its range or early-outing
is what closes the 72%-vs-39% gap** — the vendor's cheaper PR pass is a cheaper sweep.

## The caveat, which matters

**The job names in the open driver's trace are opaque hashes** (`a5120c#1`, `e39b0e#2`, …), so **which is the
PR job and which is the fragment job cannot be told from the trace alone.** The ratios from my quick
heuristic — **34% / 95% / 78% / 70%** — **are not trustworthy**; the 95% at 1024 is probably a misassignment.

**The scaling is trustworthy because it doesn't depend on identification**: both jobs grow with tile count,
and so does the total.

## For anyone continuing

**Identify the jobs by ORDER, not by size.** The submit array is `[0]` geometry, `[1]` PR, `[2]` fragment —
so trace order within a frame gives the mapping. **A size heuristic will mislabel them, as it just did.**
