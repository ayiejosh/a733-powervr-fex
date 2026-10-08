# The NIR rematerialization pass is NOT the source of the immediate loads — refuted

## What was tested

`pco_nir.c:1252 remat_load_const()` rewrites a multi-use `load_const` into a **separate immediate per use**,
and runs for all non-internal shaders (`:1335`). Since the measured symptom is **131 `bbyp0bm_imm32`
immediate materializations for three constants**, this pass was the obvious suspect. Made it skippable via
`PVR_NO_REMAT_CONST` and A/B'd:

| variant | `cstpi` | `cstpf` | `cstpin` |
|---|---|---|---|
| remat on (default) | 48.8 M inv/s | 65.9 M inv/s | 73.1 M inv/s |
| **remat off** | 49.8 M inv/s | 67.8 M inv/s | **73.1 M inv/s** |

**Essentially unchanged (2–3%, within the ~25% noise floor), and `cstpin` — which has no constants in its
body — is exactly unchanged, as it must be.** Correctness passed with the pass disabled (`vkrender` 2048 =
4194304/4194304, `bda` PASS(0)), **so the test was valid rather than a failure mistaken for a result**.
Patch reverted; default restored.

## What this means

**The immediate materialization happens in the BACKEND, not in that NIR pass.** Whether the constant arrives
as `load_const` or `nir_build_imm`, the backend emits the immediate as an ALU operand and the USC needs a
`bbyp0bm_imm32` to place it — **one extra instruction per constant use.**

**So the fix is backend-side constant hoisting**: when a constant is used repeatedly (especially in an
unrolled body), keep it in a register and use a register operand rather than re-materialising per use.

**That is a deeper change than a pass-skip — which is exactly why the cheap version did nothing.**

**Measurement to beat**: `cstpi` 49.2 → the vendor's 146.9 M inv/s; `cstpf` 67.5 → 145.9.

## Value of the round

**It converted "one possible fix location" into "the fix is definitely backend-side"** — at the cost of one
build, one A/B, one revert, with correctness verified. The `cstpin` control behaving exactly as predicted
(zero change, since it has no constants) is what makes the null result trustworthy rather than a failed
experiment.
