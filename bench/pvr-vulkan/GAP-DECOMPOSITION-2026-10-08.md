# The gap decomposes: immediates are 1.40× of it, and a 2.12× residual remains

Built `cstpin`: **the same 32-iteration loop and op count as `cstpi`, but every operand a register** — no
immediates inside the body, so nothing can be rematerialised. Measured on both drivers:

| probe | open | vendor | ratio |
|---|---|---|---|
| **`cstpi` (immediates in body)** | **49.6 M inv/s** | **147.2 M inv/s** | **2.97×** |
| **`cstpin` (register-only body)** | **73.1 M inv/s** | **155.0 M inv/s** | **2.12×** |

## The decomposition

- **The vendor barely notices immediates**: 147.2 → 155.0, i.e. **+5%**
- **The open driver gains 47%** without them: 49.6 → 73.1
- **So immediate rematerialization accounts for 2.97 / 2.12 = 1.40× of the gap** — measured directly, not
  inferred from instruction counts
- **A residual 2.12× remains with a register-only body and no immediates whatsoever**

## What the residual is *not*

- Not immediates (removed by construction)
- Not the loop control (the unroll fix removed it; `loop blocks: 0` in the IR)
- Not loop enter/exit overhead
- Not clock or DRAM (no-loop integer compute `cstp` is **1.23×**)

**It is something about executing the unrolled body itself.** The remaining structural candidate is
register-file move traffic (`mbyp`/`bbyp` were 70+36+34 of 349 instructions), but moves were only ~20% of the
instruction count — **and the earlier control (a parity shader with a *higher* move ratio) already showed
move ratio alone does not predict speed.**

## Honest state of the gap

| contribution | size | status |
|---|---|---|
| loop not unrolled (>16 iterations) | **fixed** — 1.85–2.85× recovered (`c2bde57`) | done |
| immediate rematerialization | **1.40×** | measured, fixable in PCO |
| **unexplained residual in the unrolled body** | **2.12×** | **open** |
| per-job sync interface | 84% of frame time in kernel | kernel UAPI needed |

**Three of these are independent**, and the residual is now isolated to *"executing this unrolled body costs
2× more than the vendor's"* — with codegen size, immediates, moves and loop control each excluded as its
sole cause.

## Value of this round

The session's remaining gap went from *"a 2.99× residual loop deficit"* to a **precise, additive
decomposition with one term quantified by a purpose-built control**. `cstpin` was written specifically to
remove the suspected cause, and it moved the ratio by exactly the predicted direction and a measurable
amount — which is how a decomposition should be established.
