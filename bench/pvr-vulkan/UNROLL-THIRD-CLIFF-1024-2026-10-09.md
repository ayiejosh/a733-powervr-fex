# A third cliff above 256 — raising the limit to 1024 gives 2.68× on 512-iteration loops

`5a1be21` raised the unroll threshold to 256, removing the cliff above 64 for 128-iteration loops. **A probe
with a 512-iteration loop shows a third cliff above the new limit.**

## The evidence

New probe `cstpi512` (identical body, 512 iterations):

| probe | **limit 256** | **limit 1024** |
|---|---|---|
| **`cstpi512` (512 iterations)** | **1.9 M inv/s** | **5.1 M inv/s** → **2.68×** |
| `cstpi128` (128 iterations) | 19.6 | 19.6 |
| `cstpi` (32 iterations) | 70.7 | 71.6 |
| `cstpf` (32 iterations) | 84.6 | 87.7 |
| **full glmark2 suite** | **49** | **49** |

**`cstpi512` does 4× the work per invocation of `cstpi128`, so equal efficiency predicts ~19.6/4 = 4.9
M inv/s.** At **1.9** it was **2.6× less efficient** — not fully unrolled at 256. **At 1024 it reaches 5.1,
the equal-efficiency figure, so the cliff is gone.**

## A correction to the previous entry

**`5a1be21` concluded "1024 shows no gain over 256" — but that was measured only on the 128-iteration probe,
which both limits cover.** On a 512-iteration probe 1024 is **2.68× better**, so the earlier conclusion rested
on too narrow a test. **Recorded rather than quietly superseded.**

## The trade-off, stated rather than hidden

**A larger unroll limit increases code size.** This suite is **neutral (49 → 49)** because **none of its
scenes has a long loop** — so the cost is **real but invisible here.** The limit is set by **the longest loop
actually measured (512)** rather than raised arbitrarily high.

## Cumulative effect of the four committed fixes

| fix | commit | measured |
|---|---|---|
| unroll 16 → 64 | `c2bde57` | 1.85× / 2.28× / 2.85× |
| immediate hoisting | `c251c9b` | 1.47× / 1.31× / 1.18× |
| unroll 64 → 256 | `5a1be21` | 2.64× on 128-iteration loops |
| **unroll 256 → 1024** | **`167a943`** | **2.68× on 512-iteration loops** |

**All four in `src/imagination/pco/`, correctness green throughout, full suite 46 → 49.**

## Probe files

`cstpi512.c`, `cstpi512.comp`, `cstpi512_comp_spv.h` — same harness as `cstpi`.
