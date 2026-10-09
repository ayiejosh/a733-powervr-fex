# The hoisting fix confirmed in the IR — and the next target is the register-file moves

Re-dumped `cstpi`'s IR after both fixes:

| | before (unroll only) | **after both fixes** |
|---|---|---|
| total instructions | 349 (2.73× ideal) | **224 (1.75× ideal)** |
| moves | 271 (78%) | **146 (65%)** |
| **`bbyp0bm_imm32` (immediate materialization)** | **131** | **6** |

**The immediate loads went 131 → 6 — the fix did exactly what it was designed to do**, and the instruction
count fell from 2.73× to 1.75× the ideal, **tracking the measured gain**.

## What remains

```
imadd32        68    the real work (32 iterations × 2 ops = 64)
mbyp           70   ┐
bbyp0bm        36   ├ 140 register-file moves
bbyp0s1        34   ┘
bbyp0bm_imm32   6    the hoisting fix's residue
control        ~7
```

**140 register-file moves for 68 arithmetic operations — a 2:1 move-to-work ratio — and the instruction
count (1.75× ideal) now tracks the remaining gap (2.02×).** The next codegen target is clear: **reduce the
register-file moves** — a register-allocation / operand-collector question, not a constant or loop one.

## Caveat stated before it is assumed

**The vendor's IR is unavailable**, so it is **not proven** that the vendor needs fewer moves — it is
inferred from the instruction count tracking the gap. Each PowerVR ALU operation may genuinely need
operand-setup instructions, in which case much of the 140 is **inherent rather than PCO-specific**.

**The discriminator would be a shader with fewer distinct operands**: if its move ratio falls
proportionally, the moves are operand-setup driven; if not, they are allocation driven.

## Session scoreboard of committed fixes

| fix | commit | measured |
|---|---|---|
| unroll threshold 16 → 64 | `c2bde57` | 1.85× (`cstpi`), 2.28× (`cstpf`), 2.85× (`vkheavy`) |
| block-local immediate hoisting | `c251c9b` | 1.47× (`cstpi`), 1.31× (`cstpf`), 1.18× (`vkheavy`) |
| **combined** | | **2.74× / 2.97× / 3.36×** — gap to vendor **5.53× → 2.02×** on looped compute |
