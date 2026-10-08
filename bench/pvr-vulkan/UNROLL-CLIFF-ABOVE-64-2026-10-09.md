# The unroll cliff recurs above 64 — raising the limit to 256 gives 2.64× on 128-iteration loops

`c2bde57` raised the unroll threshold from 16 to 64 and removed the cliff at 16. **A probe with a
128-iteration loop shows the same cliff recurs above the new limit.**

## The evidence

New probe `cstpi128` (identical body to `cstpi`, 128 iterations):

| probe | limit 64 | **limit 256** | limit 1024 |
|---|---|---|---|
| **`cstpi128` (128 iterations)** | **7.4 M inv/s** | **19.5 M inv/s** | 19.6 M inv/s |
| `cstpi` (32 iterations, already unrolled) | 72.4 | 72.0 | 70.3 |
| `cstpf` (32 iterations) | 84.6 | 84.6 | 88.3 |
| `vkheavy` (real 32-iteration shader) | 255.8 ms | — | 255.3 ms |

**`cstpi128` runs four times the iterations of `cstpi`, so equal efficiency would be ~72.4/4 = 18.1 M
inv/s.** At **7.4** it was **2.4× less efficient per iteration** — the loop was not fully unrolled at 64.

**At 256 it reaches 19.5 M/s — the equal-efficiency figure — so the cliff is gone (2.64×).** The
32-iteration probes are unchanged, as they must be since they were already fully unrolled.

## Why 256 and not 1024

**1024 shows no gain over 256 for the measured case**, so **256 is chosen: the smallest value that removes
the measured cliff.** Larger values increase code size, which is the real trade-off — **and that is why the
limit is not raised arbitrarily high.** Recorded so the choice is a measurement rather than a guess.

## Correctness

Green at 256: `vkrender` 512 (262144/262144) and 2048 (4194304/4194304), `bda` PASS(0), `vk13` PASS,
`pctest` PASS(0), `vk16` PASS, **`glmark2-es2 --validate` 27 scenes**.

## Commits

* `5a1be21` (mesa) — `max_unroll_iterations` 64 → 256
* preceded by `c2bde57` (16 → 64) and `c251c9b` (immediate hoisting)

## Probe files

`cstpi128.c`, `cstpi128.comp`, `cstpi128_comp_spv.h` — built with the same harness as `cstpi`.
