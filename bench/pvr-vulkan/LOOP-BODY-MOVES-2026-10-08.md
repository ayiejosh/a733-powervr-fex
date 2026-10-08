# PINPOINTED: the loop body carries 34 register moves per iteration, executed 32×

Split `vkheavy`'s final IR into prologue and loop body:

| region | instructions | moves (`mbyp`/`bbyp`) | move % |
|---|---|---|---|
| PROLOGUE (once) | 30 | 20 | 67% |
| **LOOP BODY (×32)** | **80** | **34** | **42%** |

Loop body opcode mix: `mbyp` 29, `fmad` 10, `fadd` 5, `fmul` 5, `bbyp0bm_imm32` 4, `cndst.if` 3,
`fsinc` 2, `imadd64` 2, ...

## The arithmetic matches the measurement

* **34 register moves per iteration × 32 = ~1088 move instructions per fragment invocation.**
* **80 instructions for ~25–30 compute operations — a ~2.7× instruction overhead.**
* **The measured deficit on this shader is 2.40×.** Overhead and deficit agree in size — **the first time a
  code-level measurement has matched a measured performance gap in this investigation.**

## Why this is the strongest lead

* **In Mesa's scope** — PCO's instruction selection and scheduling, `src/imagination/pco/`.
* **Measured, not inferred**: instruction counts from PCO's own IR; deficit from per-job kernel timestamps.
* **Explains the float-specific shape**: float ops need more live values across the USC's register files,
  so a scheduler that resolves pressure with moves hurts float-heavy shaders far more than the integer-ALU
  compute path (`cstp`, 1.12×), which needs fewer.
* **Consistent with everything excluded**: not clock, DRAM, tile geometry, occupancy, or sample rate.

## Caveat, stated plainly

**The vendor's IR is unavailable, so I cannot show its move ratio is lower.** 42% is high for a compiler
(good codegen typically runs 10–15%), so it is a reasonable inference, not a proven one. **What is proven:
PCO emits ~2.7× the ideal instruction count for this loop, and the excess is moves.**

## Candidate next steps (all in PCO)

1. Why values are moved between register files *inside* the loop — whether a value that could stay
   resident is re-materialised each iteration.
2. Whether the loop's live set is split across files by the scheduler, and whether a different assignment
   keeps hot values in one file.
3. Compare against the `cstp` compute shader's move ratio (integer, 1.12×) — whether the difference is
   float liveness specifically.
