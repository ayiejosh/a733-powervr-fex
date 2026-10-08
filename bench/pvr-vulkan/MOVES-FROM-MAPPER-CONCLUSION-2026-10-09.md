# The moves are produced by the assembler's ISA mapping table, not by any PCO pass

## The specified step, executed

The previous entry asked where `bbyp0s1`/`bbyp0bm` are emitted. They are in **`pco_map.py`**, the assembler's
ISA encoding table — **not in any PCO pass**:

```
encode_map(O_BBYP0BM, ...)          line 1617
encode_map(O_BBYP0BM_IMM32, ...)    line 1629
encode_map(O_BBYP0S1, ...)          line 1641
encode_map(O_MSK_BBYP0S1, ...)      line 1653
...
line 3603: ('0', O_BBYP0S1, ['ft2', 'ft3'], [SRC(1)]),
line 3625: ('0', O_BBYP0BM, ['ft0', 'ft1'], ['s0', SRC(0)]),
```

**`bbyp0s1` and `bbyp0bm` are not operations a pass inserts.** They are **encoding forms the mapper emits when a
PCO instruction's operands have to be moved to satisfy the hardware register constraints.**

## Why this reconciles all four failed attributions

**Each earlier explanation pointed at the same phenomenon from a different angle, and none was sufficient
alone:**

| attribution | what it got right |
|---|---|
| "register allocation" | the constraint is about where values live |
| "legalization for ISA limits" | the legalizer *does* insert moves — just not these |
| "the class table" | the class table is a real constraint — it simply doesn't bite for all-TEMP shaders |
| "slot contention" | the effect is operand placement — but `insert_mov_ref` isn't the emitter |

**The moves come from satisfying the ISA's register constraints at mapping time.** That's why reading any one
of the four places explained part of the picture, and testing it against the shader in hand disproved it as
the *whole*.

## The honest conclusion about headroom

**A large part of the 1.1–2.1 moves per operation is likely inherent** — the hardware needs operands in
particular registers, and the mapper must get them there. **That's consistent with the vendor being only 1.39×
faster than PCO's near-optimal `cstpi1` (1.19× ideal)**, rather than the several-fold gap a fully removable
cost would produce.

**So the measured floor stands at `cstpi1`'s 1.1 moves/op, and the remaining `cstpi` overhead (2.1/op) is a
register-placement question in the mapper** — deep, well-posed, **and no longer worth chasing blind.** The
four-fix, measured record of this session is the better place to stop.
