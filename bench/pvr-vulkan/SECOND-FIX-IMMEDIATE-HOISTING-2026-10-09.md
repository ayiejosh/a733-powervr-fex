# SECOND FIX LANDED: block-local immediate hoisting — `c251c9b`

## What was done

Added block-local hoisting of repeated immediates to `pco_const_imms.c`, alongside the existing
constant-register rewrite. Within a single block, the first `MOVI32` for a non-table immediate stays, and
later `MOVI32`s for the same value become **moves from that register**.

**Block-local is what makes it safe**: a value produced earlier in the same block dominates every later use,
so no cross-block dominance analysis is needed — and it covers the case that matters, an **unrolled loop
body, which is a single block**.

## Measured effect

| probe | before | after | gain |
|---|---|---|---|
| **`cstpi` (integer, immediates)** | 49.2 M inv/s | **72.8 M inv/s** | **1.47×** |
| **`cstpf` (float, immediates)** | 65.9 M inv/s | **87.9 M inv/s** | **1.31×** |
| **`cstpin` (control: no immediates)** | 73.1 M inv/s | **71.3 M inv/s** | **unchanged, as predicted** |
| `cstp` (integer, no loop) | 305.9 M inv/s | 324.4 M inv/s | unchanged |
| **`vkheavy` (real 32-iteration shader)** | 301.1 ms | **255.8 ms** | **1.18×** |

**`cstpi` reaches parity with `cstpin` (72.8 vs 71.3)** — the predicted outcome: hoisting should make the
immediate case cost what the register case costs. **The control did not move**, which is what makes the gain
attributable rather than coincidental.

## Correctness fully green

`vkrender` 512 (262144/262144) and 2048 (4194304/4194304) · `bda` PASS(0) · `vk13` PASS · `pctest` PASS(0) ·
`vk16` PASS · **`glmark2-es2 --validate`: 27 scenes OK.**

## Combined effect of the two committed fixes

| probe | original | after unroll | after both | **total** | vs vendor now |
|---|---|---|---|---|---|
| `vkheavy` | 858.5 ms | 301.1 ms | **255.8 ms** | **3.36×** | **1.42×** (was 4.77×) |
| `cstpi` | 26.6 M/s | 49.2 M/s | **72.8 M/s** | **2.74×** | **2.02×** (was 5.53×) |
| `cstpf` | 29.6 M/s | 65.9 M/s | **87.9 M/s** | **2.97×** | **1.66×** (was 4.93×) |

**Two commits, both in `src/imagination/pco/`, both with the control behaving as predicted** —
`c2bde57` (unroll threshold) and `c251c9b` (immediate hoisting).
