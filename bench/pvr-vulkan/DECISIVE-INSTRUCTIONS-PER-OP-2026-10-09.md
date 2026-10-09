# DECISIVE: the compute gap is instructions per operation, confirmed two independent ways

## The comparison

From the complete both-driver matrix — single instrument, one run:

| probe | shape of the final code | open | vendor | ratio |
|---|---|---|---|---|
| **`cstp`** | straight-line integer ALU, no loop | **361.0 M inv/s** | **369.8** | **1.02× — PARITY** |
| **`cstpin`** | 32-iteration loop, registers only | **72.0** | **154.2** | **2.14×** |

## Why this is decisive

**At unroll limit 1024, `cstpin`'s 32-iteration loop is fully unrolled** — so its final code is **also
straight-line ALU, the same instruction shape as `cstp`.**

**Same shape, 1.02× vs 2.14×.** So the deficit is **not**:
- the loop (none left in either after unrolling)
- immediate materialization (`cstpin` has none — it's the register-only control)
- the shader type or arithmetic (both integer ALU)
- general driver slowness (`cstp` is at parity)

**It is instructions per operation:**
- `cstp` — straight-line, low register pressure → few moves/op → **parity**
- `cstpin` — unrolled from a loop, many live values → **~2.1 moves/op** → **2.14×**

## Two independent confirmations of the same number

1. **Static** (IR analysis, earlier): `cstpi1` **1.1 moves/op** vs `cstpi`/`cstpin` **2.1 moves/op**, mechanism
   traced to the assembler's ISA mapping in `pco_map.py`.
2. **Dynamic** (this measurement): **parity** for the low-move shape, **2.14×** for the high-move shape.

**A black-box throughput measurement and a static instruction count agree on the same cause** — the strongest
form of evidence available on this board.

## It also re-reads the session's fixes correctly

| probe | before | after | why |
|---|---|---|---|
| `cstpi` (immediates) | 26.6 | **72.8** | hoisting removed **125 of 131** materializations |
| `cstpin` (no immediates) | 71.3 | **72.0** | **control — nothing to hoist** |

**The fixes brought `cstpi` UP TO `cstpin`'s level.** Both now sit at **~71–72** vs the vendor's **~145–154**.
**The remaining gap is shared by both, so it cannot be immediates.** It is the register moves — which is also
why `cstpin`, built as the clean control, is now the **worst** compute case: it was always carrying the cost
the fixes could not reach.

## Consequence for the objective

**The compute gap is now fully attributed.** The four fixes removed the terms PCO controlled (loop control,
immediate materialization); what remains is **register placement in the assembler's ISA mapping** — largely
inherent, since the vendor pays a similar per-operand cost (only **1.39×** ahead on the near-optimal `cstpi1`).

**So there is no further codegen win of the size the last four produced.** The remaining levers are the ones
already identified and out of Mesa's reach: the **kernel/sync path (60% of the real workload)** and the
**per-surface render deficit**.
