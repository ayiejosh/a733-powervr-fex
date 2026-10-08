# The register-move mechanism is the ISA's S{0,2,3} / S{1,2,4} source-group constraint

## The finding

`pco_legalize.c` documents the constraint in the function signature itself:

```c
/**
 * \param[in] needs_s124 Whether the mapping needs to use S{1,2,4}
 *                       rather than S{0,2,3}.
 */
static void insert_mov_ref(pco_instr *instr, pco_ref *ref, bool needs_s124)
{
   if (instr->op == PCO_OP_MBYP && needs_s124 && !pco_ref_has_mods_set(*ref)) {
      instr->op = PCO_OP_MOVS1;
      return;
   } else if (instr->op == PCO_OP_MOVS1 && !needs_s124) {
      instr->op = PCO_OP_MBYP;
      return;
   }
   ...
   mov_instr = needs_s124 ? pco_movs1(&b, new_ref, *ref, ...)
                          : pco_mbyp(&b, new_ref, *ref, ...);
}
```

**An instruction can only source its operands from one register group — `S{0,2,3}` or `S{1,2,4}`.** When an
operand sits in the wrong group, **the legalizer must insert a move to bring it into the right one.**

## Why this is the right explanation

**It predicts the measured correlation exactly:**

| shader | distinct operands | moves per op |
|---|---|---|
| `cstpi1` | 1 | **1.1** — the floor; no group conflict is possible with one operand |
| `cstpi` | 4 | **2.1** — operands land in different groups |

**So the moves are neither pure allocation waste nor ISA-mandated:** they are required **by the group choice —
and the group choice is PCO's to make.** That resolves the back-and-forth of the previous two entries: the
mechanical *emitter* is the legalizer (correct), and the *cause* is which group each value is assigned to
(also correct). **It identifies a real, bounded optimisation target rather than a mystery.**

## The fix shape

**An assignment that keeps co-used operands in the same source group**, so `needs_s124` is satisfied without a
move — a register-group assignment problem in PCO, adjacent to the existing allocator rather than a tweak to
the legalizer.

**Measurement to beat:** `cstpi` 72.8 M inv/s against the vendor's 146.6, with `cstpi1`'s **1.1 moves/op** as
the floor and `cstpi`/`cstpin` at **2.1** as the current cost. **A shader whose operands all share a group is
the natural control** — `cstpi1` behaves as one.

## Note on the two preceding entries

The first attributed the extra moves to **register allocation**; the second corrected that to **legalization
for ISA operand limits** and called the first too confident. **Both were partly right and neither was
complete** — legalization emits the move, and the group assignment is why it is needed. **This entry
supersedes both rather than adding a third guess.**
