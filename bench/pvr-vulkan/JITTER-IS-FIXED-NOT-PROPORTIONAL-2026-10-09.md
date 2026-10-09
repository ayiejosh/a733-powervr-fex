# The spread is a fixed ~0.2 ms jitter, not proportional instability

## The anomaly and the measurement

| size | **absolute jitter** | frame time | percentage |
|---|---|---|---|
| 512 | **0.179 ms** | 0.673 ms | **26.6%** |
| 2048 | **0.250 ms** | 5.527 ms | **4.5%** |

**The absolute jitter is similar (0.18–0.25 ms) while the frame time differs 8×.** The percentage spread
therefore scales **inversely with frame duration** — the small-size "instability" is **a fixed jitter becoming a
large fraction of a small number**, not the driver behaving worse at 512.

**Outliers are the same phenomenon:** `0.833` at 512 and the earlier `7.260` at 2048 are single scheduling
hiccups, not a second mode.

## Consequences for anyone measuring here

1. **Small workloads need more samples.** At 0.67 ms/frame, 0.2 ms is 30%; at 5.5 ms it is 4.5%.
   **A 3-run median at 512 proves nothing.**
2. **Quote ratios from larger sizes.** The 2048 figures (2.39× median, **1.94× worst case**) rest on 4.4–4.5%
   spreads; the 512 ratio's ~20% spreads make it the weaker of the two.
3. **Anything under ~2% at 2048 is unmeasurable** — that is why the GEOM+FRAG timeline change's apparent 1.5%
   gain was correctly judged "no measurable gain."

## Where this is recorded

- Recovery log: `Radxa-A7A/Recovery/GPU-FIRMWARE-RE-2026-10-06.md` (entry "the small-size spread is a fixed
  ~0.2 ms jitter")
- `harness-log.jsonl` + `HARNESS-LOG-SUMMARY.md` carry the raw ranges.

## Not implemented

**`harness.py` still reports one speed sample per run.** An intended change to repeat phase 2 and print a
range was **abandoned** — two string-pattern edits failed to match and, correctly, asserted before writing, so
the file is unmodified. **The finding is documented here instead; the tweak is small and worth doing**
(loop the speed phase N times, print min/median/max, and store `samples` in the record).
