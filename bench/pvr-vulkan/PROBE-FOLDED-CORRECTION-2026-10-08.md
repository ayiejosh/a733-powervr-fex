# CORRECTION: a synthetic probe was constant-folded by the vendor, invalidating it

## Variant table (2048, sum of the two fragment jobs)

| shader | open | vendor | ratio | valid? |
|---|---|---|---|---|
| `vkmul` (mul-only, `acc *= k` 64x) | 274.3 ms | **6.83 ms** | **40x** | **NO — vendor folded it** |
| `vkalu` (fmad-heavy float ALU) | 661.4 ms | 128.8 ms | 5.13x | yes |
| `vkheavy` (SFU: sin/cos/sqrt) | 858.5 ms | 357.2 ms | 2.40x | yes |
| `vkrender` (trivial fract) | 21.7 ms | 7.4 ms | 2.9x | yes |

## Why `vkmul` is invalid

**The vendor's `vkmul` (6.83 ms) is indistinguishable from its trivial shader (7.4 ms)** — its compiler
constant-folded the 64-multiply chain away. The open driver did not, hence 274 ms. **So `vkmul` measures
optimizer quality, not float throughput.**

That is a real and separate finding — **PCO does not constant-fold a chain of multiplies the vendor's
compiler eliminates** — but it is *not* evidence about execution rate and must not be reported as one.

## Why `vkalu` and `vkheavy` remain valid

* **`vkalu` depends on `gl_FragCoord`** (through `fract`/`min`/`max`) so it cannot be folded. The vendor's
  128.8 ms is **17x its trivial 7.4 ms**, confirming the loop is present on both.
* **`vkheavy` uses `sin`/`cos` of a varying value**, likewise unfoldable, 48x its trivial time.

## The general hazard, recorded

**Synthetic shaders must be verified to survive both compilers' optimizers before their timings mean
anything.** This is the third time this session a probe measured something other than what it claimed
(discard controls, the macrotile/tile-size "speedups", now constant folding). **The gate is cheap: check
that the probe's time is far above the trivial shader's on BOTH drivers.**

## What survives

**Float-heavy, unfoldable workloads are 2.40–5.13x slower on the open driver**, while integer-ALU compute
(`cstp`) is 1.12x. The spread between 2.40x (SFU-heavy) and 5.13x (float ALU) is consistent with different
operation mixes rather than one hardware rate, and **PCO's weaker constant folding is a separate, genuine
optimisation gap** worth pursuing on its own.
