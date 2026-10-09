# The residual is CODEGEN SIZE — 141 register-file moves for 67 real operations

Counted the final IR of both probes (unrolled, register-only vs immediates):

| shader | instructions | vs ideal (128) | moves | measured gap to vendor |
|---|---|---|---|---|
| **`cstpin` (register-only)** | **218** | **1.70×** | **141 (65%)** | **2.12×** |
| `cstpi` (immediates) | 349 | 2.73× | 271 (78%) | 2.97× |

**The instruction count tracks the measured gap across both probes** — 2.73× ideal → 2.97× slow, and 1.70×
ideal → 2.12× slow. **The residual is not an execution-rate mystery; it is codegen size.**

## What is in the body

```
cstpin:  imadd32       67   ← the real work (32 iterations × 2 ops = 64)
         mbyp          70  ┐
         bbyp0bm       34  ├ 141 register-file moves
         bbyp0s1       35  ┘
         + 10 control/misc
```

**PCO needs two register-file moves for every arithmetic operation in this body** — the moves are twice the
real work.

## Correction to an earlier refutation

An earlier entry "refuted" the move hypothesis using `cstp`: a **loopless** shader with a 52% move ratio at
parity. **That control was itself flawed** — it compared a loopless shader's move ratio against a looped
one's, and the caveat was noted at the time. **With a matched comparison (both unrolled loop bodies), move
count does track the gap.**

## The complete, evidence-based decomposition of the loop gap

| contribution | measured by | size |
|---|---|---|
| loop not unrolled (>16 iterations) | the cliff at 16; the 1.85–2.85× recovery | **fixed** (`c2bde57`) |
| immediate rematerialization | `cstpi` vs `cstpin`, cross-driver | **1.40×** |
| **register-file moves for the unrolled body** | **instruction counts tracking the measured ratios** | **the rest, ~2.1×** |

**Both remaining terms are PCO codegen, both live in `src/imagination/pco/`, and both have a stated
measurement to beat**: `cstpin` 73.1 M/s vs the vendor's 155.0; `cstpi` 49.6 vs 147.2.

## Where the whole gap now stands

| area | gap | state |
|---|---|---|
| looped compute (PCO codegen) | 2.12–2.97× | **decomposed into two fixable codegen causes** |
| raw render, no loop (`vkrender`) | **2.47×** | **open** — not explained by any codegen term above |
| no-loop integer compute (`cstp`) | 1.23× | near parity |
| per-job sync interface | 84% of frame time in kernel | kernel UAPI; refuted in Mesa |
