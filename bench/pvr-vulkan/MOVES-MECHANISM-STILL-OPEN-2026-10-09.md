# Verification eliminates the class table for cstpi — and my previous "resolution" with it

## The check

My previous entry claimed `cstpi`'s operands are all TEMPs and used that to exclude the class table. **I
checked it rather than assuming:**

```
cstpi:  srNN (shared/SPEC-class) references = 0,  rNN references = 600
cstpi1: srNN references = 0,                     rNN references = 425
```

**The claim holds: both shaders' operands are TEMP-class.** So the class table really does not explain
`cstpi`'s 2.1 moves per op.

## But the same fact breaks the conclusion I drew from it

`ref_src_map_valid()` returns **true** for anything in the `default` case — where TEMPs land. **So for a shader
whose operands are all TEMPs, `insert_mov_ref()` should never insert anything.** Yet `cstpi` has 140 moves.

**Therefore the moves are not produced by `insert_mov_ref()` at all**, and my previous entry's "resolution" —
slot contention enforced by that function — **was an assertion I did not verify. Withdrawn.**

## What is actually established about the moves

| claim | status |
|---|---|
| produced by `insert_mov_ref` / `needs_s124` | **excluded for `cstpi`** — all operands are TEMPs, so that path cannot fire |
| caused by class mixing (SHARED vs SPEC) | **excluded for `cstpi`** — zero shared-register references in the IR |
| caused by register allocation | not established |
| caused by the encoder's own bypass generation | **not yet examined** |

**After four attributions, the honest position:** the moves are real, measured, scale with the number of
distinct operands (**1.1/op at one operand, 2.1 at four**), and are produced by a part of the backend **I have
not yet read** — most likely the encoder that emits `bbyp*`, **not the legalizer**.

## Next step, specifically

**Read where `bbyp0s1` and `bbyp0bm` are emitted during assembly.** If they are an operand-collector requirement
of the encoding, **a large part is inherent and the vendor pays it too**; if they follow from a choice PCO
makes, **that choice is the target.**

## Why this entry is a next step, not a conclusion

**Four successive "explanations" have each been partly wrong.** This one is deliberately written as a next step
rather than a fifth conclusion — the previous four were all stated with more confidence than the evidence
supported, and the error in each case was the same: **asserting a mechanism from a plausible-looking source
without testing it against the shader in hand.**
