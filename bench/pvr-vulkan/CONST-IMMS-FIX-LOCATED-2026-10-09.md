# The immediate-materialization chain is fully traced — fix located to `pco_const_imms.c`

## The chain, end to end

1. **NIR `load_const`** → `pco_ref_nir_def()` (`pco_trans_nir.c:81`) maps **every** def, including a constant,
   to an SSA ref via `pco_ref_ssa(def->index, ...)`. A constant is **not** an immediate at this stage.
2. **The backend turns the constant into an immediate operand** on the ALU instruction.
3. **`pco_const_imms()`** (`pco_const_imms.c`, 214 lines — *"PCO constant immediates lowering pass"*) then
   tries to avoid materializing it, by looking the value up in the hardware's **constant-register table**:

```c
static const struct const_reg_def const_reg_defs[] = {
   { 0x00000000, 0, ... }, { 0x00000001, 1, ... }, { 0x00000002, 2, ... }, ...
};
static const struct const_reg_def *constreg_lookup(uint32_t imm)   /* bsearch */
```

4. **The table holds small values.** `cstpi`'s constants are **`0x9E3779B9`, `0xFFFFFF00`, `0xFF`** — the
   first two miss. **A miss means the encoder emits `bbyp0bm_imm32` per use** — the measured **131
   instructions for three constants.**

## The fix is precise and bounded

**Hoist a repeated immediate into one temp register and rewrite its uses to that temp** — a new pass in
`pco_const_imms.c` or an extension of it, sitting exactly where the constant-register lookup already is.
**The pass already exists to avoid materializing immediates; it just cannot help for arbitrary 32-bit
values.**

**Shape of the change:** count uses of each distinct immediate within a function; where a value is used more
than N times, emit one `movi32` into a temp SSA value and replace the uses. **The RA then keeps that temp in
a register, so each use costs a register operand instead of a `bbyp0bm_imm32`.**

**Measurement to beat:** `cstpi` 49.2 → vendor 146.9 M inv/s; `cstpf` 67.5 → 145.9. The two probes differ
*only* in whether the loop body's operands are immediates (`cstpi`) or registers (`cstpin`, 73.1 M/s) — so
they are the natural A/B.

## Why this is the right stopping point

**The fix is now located to a specific 214-line file, with a named function and a clear mechanism**, rather
than "somewhere in the backend". **Implementing a new SSA-rewriting pass is a multi-step change with the RA
interacting** — and half-applying it in a shader compiler is the same class of risk that made the timeline
migration attempt get reverted. **The investigation is complete; the implementation is scoped.**
