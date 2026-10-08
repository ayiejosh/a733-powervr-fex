# Register-move overhead is real (45% of instructions) — but temp allocation is not its cause

## What the codegen looks like

`vkheavy`'s final IR for the user fragment shader (`PCO_DEBUG_PRINT=passes,fs`):

```
temps: 18
total instructions: 110
  mbyp              40   <- register-file bypass/move
  bbyp0bm_imm32     10   <- bypass with immediate
  fmad              10
  fmul               7
  msk_bbyp0s1        5
  fadd               5
  movwm.phase2end    4
  bbyp0s1            3
  cndst.if           3
  fsinc              2
  frsq               2
  ... (sin/cos/sqrt/fract/normalize)
```

**~50 of 110 instructions (45%) are register-file moves, not computation.** The loop body should need
~25 compute ops; PCO emits roughly **twice the ideal count**, and the excess is move traffic — consistent
in size with the measured **2.40×** deficit on this shader.

## Hypothesis tested: min-temps allocation causes the moves

PCO minimises temps (18) via an "optimal" pass before falling back to "maximum". If aggressive reuse
bought the move traffic, forcing maximum temps should reduce it.

| vkheavy, 2048 | fragment jobs |
|---|---|
| optimal temps (default) | 427.78 + 430.77 = **858.55 ms** |
| forced maximum temps | 428.45 + 431.77 = **860.22 ms** |

**No change (within noise); correctness PASS both ways. Refuted** — the temp allocation strategy is not
what produces the move traffic.

## Note

**PCO already has `PCO_DEBUG(RA_SKIP_OPT)` for exactly this experiment** (`pco_ra.c`:
`bool alloc_max = PCO_DEBUG(RA_SKIP_OPT);`). The temporary knob duplicated an existing facility and has
been reverted.

## Where the float/shader lead stands

* The **move overhead is real and measurable in the IR** (45% of instructions).
* The **temp-allocation strategy is excluded** as its cause.
* Remaining candidates: **PCO's instruction selection and scheduling** — how it lowers float ops and
  orders them across the USC's register files — rather than its register budget. **That is a deeper
  codegen question than a knob can answer.**
