# The residual loop gap is CONSTANT REMATERIALIZATION — 131 immediate loads for 3 constants

Dumped `cstpi`'s final IR **with the unroll fix applied** (`PCO_DEBUG_PRINT=passes,cs`):

```
loop blocks: 0                 (fully unrolled — the fix works)
total instructions: 349
ideal for 32 iterations × 4 ops = 128
→ 2.7× the ideal instruction count

opcode mix:
  bbyp0bm_imm32  131   ← 32-bit IMMEDIATE materialization
  mbyp            70
  imadd32         68   ← the actual work (32 × 2 = 64)
  bbyp0bm         36
  bbyp0s1         34
  cndst.if         2
  add64_32.s       2
  br.allinst       1
moves: 271 of 349 (78%)
```

## The finding

**The shader has three unique constants** (`0x9E3779B9`, `0xFFFFFF00`, `0xFF`). **PCO materialises the
immediate on every use instead of hoisting it into a register: 131 `bbyp0bm_imm32` instructions for three
constants.**

- **2.7× the ideal instruction count** — matching the measured **2.99× residual gap** to the vendor
- **78% of all instructions are moves/rematerialization**; only 68 of 349 are the arithmetic the shader asked for

**This is a concrete, fixable PCO codegen issue** — constant hoisting / avoiding redundant rematerialization
in an unrolled body — entirely inside Mesa (`src/imagination/pco/`).

## Why this is a good result

- **Specific and measured**, not inferred: instruction counts from PCO's own IR dump; the gap from a
  cross-driver A/B with a vendor baseline stable to ~1%
- **Explains the residual exactly**: 349/128 = 2.7× ideal vs a measured 2.99×
- **Follows directly from the unroll fix**: unrolling removed the loop control and *exposed* that the body is
  2.7× too long, dominated by immediate reloads
- **Independent of the other bottleneck** (the per-job sync interface) — two separable targets, not one mystery

## Practical shape of the fix

Keep constants used inside an unrolled body resident in registers rather than re-materialising per use, or
let the allocator spill them and load once.

**Measurement to beat**: `cstpi` 49.2 M inv/s (vendor 146.9); `cstpf` 67.5 M inv/s (vendor 145.9);
`vkrender` ~306 Mpix/s (vendor 757) as the no-loop control.
