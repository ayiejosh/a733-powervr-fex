# The loop gap is now FULLY attributed — instruction count explains it, one bypass per op is inherent

## The discriminator

Built `cstpi1`: **the same 32-iteration loop and 4 ops per iteration as `cstpi`, but every operation uses the
same single register operand** — minimum distinct operands.

| shader | distinct operands | instructions | vs ideal | moves | throughput |
|---|---|---|---|---|---|
| **`cstpi1`** | 1 | **152** | **1.19×** | **75 (49%)** | **112.1 M inv/s** |
| `cstpi` | 4 | 224 | 1.75× | 146 (65%) | 72.8 M inv/s |

**Fewer operands → fewer moves → faster.** The opcode mix shows the structure:

```
cstpi1:  imadd32 67  +  bbyp0s1 65   ← ONE bypass per ALU op
cstpi:   imadd32 68  +  mbyp 70 + bbyp0bm 36 + bbyp0s1 34   ← extra ~80 moves
```

**One bypass per ALU operation is INHERENT** — the USC needs operand setup and the vendor must emit it too.
**The extra ~80 moves on multi-operand code are PCO register-allocation overhead.**

## The cross-driver closure

| probe | open | vendor | gap | instr vs ideal | **gap ÷ instr ratio** |
|---|---|---|---|---|---|
| **`cstpi1` (1 operand)** | **112.1** | **155.8** | **1.39×** | **1.19×** | **1.17** |
| `cstpi` (several) | 72.8 | 146.6 | **2.01×** | **1.75×** | **1.15** |
| `cstpin` (registers) | 71.3 | 154.6 | **2.17×** | **1.70×** | **1.28** |

**Gap ÷ instruction-count ratio = 1.15–1.28 across all three probes.** So **instruction count fully explains
the remaining loop gap**, with a small consistent ~1.2× execution factor on top.

## What this closes and opens

**Closes** the loop gap as a mystery: it is instruction count, and instruction count is
`1 bypass per op (inherent)` + `extra moves from PCO's allocation (fixable)`.

**Opens a bounded target**: **reduce the extra register-file moves for multi-operand code.** The ceiling is
quantified — closing them entirely would take `cstpi` from **2.01× to roughly `cstpi1`'s 1.39×**.

**And it retroactively justifies both landed fixes:** `c2bde57` removed loop-control instructions and
`c251c9b` removed **125 of 131** immediate materializations — **both reduced instruction count, which is
exactly what this analysis shows the gap is made of.**

## Session scoreboard

| fix | commit | measured |
|---|---|---|
| unroll threshold 16 → 64 | `c2bde57` | 1.85× / 2.28× / 2.85× |
| block-local immediate hoisting | `c251c9b` | 1.47× / 1.31× / 1.18× |
| **combined** | | **2.74× / 2.97× / 3.36×** — gap **5.53× → 2.02×** |
| **remaining ceiling** | | **→ ~1.39×** if the extra moves are removed |
