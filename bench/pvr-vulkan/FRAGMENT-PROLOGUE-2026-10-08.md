# The fragment prologue is 56 instructions for a trivial shader - and it is per-sample

## What was dumped

`vkrender`'s own shaders via `PCO_DEBUG_PRINT=passes,fs,internal,vs,nir,binary`, cache cleared:
2.1 MB / 53,528 lines, 27 user shaders. Internal programs are lean (`clear_attach` is 1-2
instructions), so the overhead is specific to the user fragment shader.

## The user fragment shader: 56 instructions

Its GLSL is only `fract(gl_FragCoord.x/64)`, `fract(gl_FragCoord.y/64)`, write:

```
0000: mov %0, sh0;
0001: savmsk.vm %1, _;                                   <- sample mask
0002-0004: movi32 9; movi32 16; ubfe
0005: bcsel
0006: logical.and
0007-0010: mov sr53; movi32 1; shift.lsl; logical.and
0011: min.u32                                            <- ~11 instrs of coverage work
0013-0018: mov sh0; movi32 26; movi32 1; ubfe sh0,26,1; tstz; csel   <- \
0020-0025: mov sh0; movi32 26; movi32 1; ubfe sh0,26,1; tstz; csel   <- / identical, twice
0026-0035: fmul/fflr/fneg/fadd x2                        <- the actual fract() math
0036-0054: pck.cov, packing, masks, alphaf.olchk branch
```

## Two concrete observations

1. **Unconditional per-invocation coverage-mask work** (`savmsk.vm` plus the
   `ubfe`/`bcsel`/`and`/`shift`/`min` sequence, ~11 instructions) in a **non-multisampled** pipeline.
   Mask/coverage work is precisely what gets more expensive with samples, matching the measured
   signature: **2.4x at 1 sample, 4.45x at 4**.
2. **A redundant duplicate**: `0013-0018` and `0020-0025` compute an identical
   `ubfe sh0,26,1` + `tstz` + `csel` twice. `pco_opt_fwd_prop` and `pco_dce` should have merged them.

**Together the prologue is ~30 of the 56 instructions and does nothing the shader asked for.**

## Caveat

Instruction count alone does not prove the 2.45x, and the vendor's codegen cannot be dumped for
comparison. What this gives is a **specific, falsifiable target**: if the coverage work is hoisted out
of the per-invocation path (or the duplicate removed) and the rate moves, that is the cost.
`vkrender` measures it in seconds and `SAMPLES=4` should move furthest if mask work is the cause.

## Next

Find where the prologue is generated - the fragment prologue lowerings in `pco_nir_pvfio.c` /
`nir_lower_*`, or the `savmsk`/coverage emission in the PCO NIR lowering - and check whether it is
emitted unconditionally where coverage is not observable.
