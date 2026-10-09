# The exact source-slot constraint, in code

`ref_src_map_valid()` in `pco_internal.h` is what enforces the `S{0,2,3}` / `S{1,2,4}` split:

```c
ref_src_map_valid(pco_ref ref, enum pco_io mapped_src, bool *needs_s124)
{
   if (needs_s124) *needs_s124 = false;
   /* Restrictions only apply to hardware registers. */
   if (!pco_ref_is_idx_reg(ref) && !pco_ref_is_reg(ref)) return true;

   if (pco_ref_is_idx_reg(ref))
      return (mapped_src == PCO_IO_S0) || (mapped_src == PCO_IO_S2) || (mapped_src == PCO_IO_S3);

   switch (pco_ref_get_reg_class(ref)) {
   case PCO_REG_CLASS_COEFF:
   case PCO_REG_CLASS_SHARED:
   case PCO_REG_CLASS_INDEX:
   case PCO_REG_CLASS_PIXOUT:
      return (mapped_src == PCO_IO_S0) || (mapped_src == PCO_IO_S2) || (mapped_src == PCO_IO_S3);
   case PCO_REG_CLASS_SPEC:
      if (needs_s124) *needs_s124 = true;
      return (mapped_src == PCO_IO_S1) || (mapped_src == PCO_IO_S2) || (mapped_src == PCO_IO_S4);
   default:
      return true;
   }
}
```

## What this says

- **TEMP registers (and anything unlisted) can be sourced from ANY slot** — the `default` case returns true.
  **A shader whose operands are all TEMPs has no class constraint at all.**
- **`COEFF`/`SHARED`/`INDEX`/`PIXOUT` and index registers are restricted to `S{0,2,3}`.**
- **`SPEC` registers are restricted to `S{1,2,4}`.**
- **`S2` is the only slot common to both restricted groups.** An instruction mixing a `SHARED` operand and a
  `SPEC` operand has **exactly one legal arrangement**; anything else forces a move.

## The honest resolution of the three failed attributions

**`cstpi`'s operands are all TEMPs — flexible — yet it still needs 2.1 moves per op.** So **the class table alone
does not explain `cstpi`.** What forces those moves is that **an instruction encoding offers only certain source
slots**, and two operands wanting the same slot can't both have it.

**That is a slot-assignment problem** — which is what the first entry said ("register allocation") and what the
second denied. **The legalizer is the mechanism; the slot assignment is the cause.**

## The target, now precise and checkable

**Assign source slots so that no instruction's operands contend for the same slot.** That is narrower than
"reduce moves", **it is checkable directly in the IR**, and **`cstpi1` (1.1 moves/op) shows the floor is
reachable.**

## Note

Three entries, three attributions, and the third only settled it **because each test narrowed the space**:
allocation → legalization → class table → and finally the observation that `cstpi`'s operands are all TEMPs,
which **excludes the class table** and leaves slot contention. **The wrong turns are on the record, which is
what made the final step identifiable.**
