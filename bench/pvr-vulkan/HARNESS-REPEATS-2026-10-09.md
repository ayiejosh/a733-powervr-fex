# The harness now repeats the speed phase and reports a range

**After five attempts** — the first four failed on string-pattern assumptions, one of which briefly broke the
file — **the tweak is in, and the discipline that made it work is the point.**

## What it does

Wraps the phase-2 speed run in a repeat loop (`HARNESS_REPEATS`, default 3) and prints, after the speed line:

```
  samples    : n=3  5.273-5.310  median 5.292  spread 0.7%
```

It also stores `samples` and `spread_pct` in the JSON record.

## Why it exists

The jitter finding: **the variance is a fixed ~0.2 ms absolute jitter, so the percentage spread scales
inversely with frame time.** A single sample therefore hides the thing that matters at small sizes — **a 0.2 ms
jitter is 4% of a 5.5 ms frame and 27% of a 0.67 ms frame.**

## It reproduces the finding

| probe | samples | median | spread |
|---|---|---|---|
| `vkrender` 2048 | n=3, 5.273–5.310 | 5.292 | **0.7%** |
| `vkrender` 512 | n=3, 0.649–0.698 | 0.653 | **7.5%** |

**The smaller frame shows the larger percentage spread**, as predicted.

## The five attempts, and what the fifth did differently

| # | approach | outcome |
|---|---|---|
| 1 | 4-space print block pattern | **0 matches** (real one is 6-space) |
| 2 | corrected 6-space block | **still 0 matches** |
| 3 | line-based lookup for `out, wall = run(...)` | **that string does not exist** |
| 4 | line-based, first `t0 = time.time()` | **matched the one inside `def run`** → IndentationError |
| **5** | **print the matched region, verify, then edit** | **worked** |

**Attempt 5's first step printed both occurrences** — `t0 = time.time()` at lines 147 and 172 — **and showed
which was which**, selecting the phase-2 one by its surrounding comment. **That is the check attempts 1–4
skipped**, and it is the same error as the session's four wrong register-move mechanisms and the dead-code
claim: **acting on a plausible mental model of an artifact instead of the artifact.**

**The lesson is now demonstrated rather than merely recorded.**
