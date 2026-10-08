# CORRECTION: the extra moves are legalization for ISA operand limits, not register allocation

## What I said last round

> "One bypass per ALU operation is inherent … **the extra ~80 moves on multi-operand code are PCO
> register-allocation overhead**."

## What the source shows

**`pco_legalize.c` is where `mbyp`/`bbyp` are emitted**, and it emits them to make operand forms encodable:

```c
if (instr->op == PCO_OP_MBYP && needs_s124 && !pco_ref_has_mods_set(*ref)) {
   instr->op = PCO_OP_MBYP;
}
...
mov_instr = pco_mbyp(&b, new_ref, *ref, .exec_cnd = exec_cnd);
...
pco_instr *mbyp = pco_ref_is_reg(src) && ... ? ... : pco_mbyp(&b, dest, src, ...);
```

**`needs_s124` — whether an operand fits the ISA's signed-12-bit form.** When it doesn't, the legalizer
inserts a bypass to materialise it. **So the extra moves are the legalizer working around operand-encoding
limits, not the allocator spreading values across register files.**

## Why the correction matters

**It changes the headroom estimate.**

- If the moves were **pure allocation waste**, removing them is a **pure win**.
- If they are **ISA-required legalization**, a large part is **inherent and the vendor pays it too** —
  which is consistent with the vendor being **only 1.39× faster** than PCO's near-optimal `cstpi1` (1.19×
  ideal), rather than the several-fold difference a pure-waste model would predict.

**The earlier claim that one bypass per op is inherent stands — and now has a mechanism: operand
legalization. The attribution of the remainder to "allocation" is withdrawn**: the remainder mixes
ISA-required legalization with whatever allocation spread exists, and **the source does not separate them.**

## Consequence for the target

**The bounded target from the previous entry — "reduce the extra moves, ceiling 2.01× → 1.39×" — is less
attractive than it read**, because some of those moves are required to encode operands at all.

**A proper improvement would need PCO to choose operand forms that avoid legalization** — better
immediate/register selection *before* legalization — **which is a narrower and different change than
"reduce allocation moves".**

**Stated rather than quietly dropped: the previous entry's attribution was too confident, and this is the
correction.**
