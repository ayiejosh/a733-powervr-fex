# Diagnosing the shaderFloat16 failures (round 46; recovered after an ENOSPC stall)

## The failure signature

`glmark2-es2 --validate` with `shaderFloat16 = true` and `MESA_SHADER_CACHE_DISABLE=true`:
**7 success / 20 failure** (with it false: 27/0).

Failing (20):
```
texture (all 3 filters)  shading=blinn-phong-inf  shading=phong
bump (high-poly/normals/height)                    effect2d (both kernels)
desktop blur-radius=5    buffer (all update methods)
conditionals fragment-steps=5                      function (low and medium)
loop (all 3 variants)
```
Passing (7 of the validated set):
```
build (both)  shading=gouraud  pulsar  desktop effect=shadow
conditionals fragment-steps=0 (both)
```

**The pattern is shader complexity**, and it is sharp: `conditionals fragment-steps=0` passes while
`fragment-steps=5` fails *on the same scene*. `loop` and `function` are pure arithmetic with no
textures, so it is not texture- or sampler-specific.

Single-scene reproduction (~10 s, used for the bisect):
```
glmark2-es2 -s 320x240 -b 'function:fragment-complexity=low:fragment-steps=5' --validate
  shaderFloat16=false -> Validation: Success
  shaderFloat16=true  -> Validation: Failure
```

## Compiler output does change

`PCO_DEBUG_PRINT=all` works; `stats` and `binary` alone produce no output. With the cache disabled the
same scene yields ~1986 lines matching f16/p16/half/pack with the feature on versus 4 with it off. The
only fp16 ISA mnemonics seen are `unpck.f16f16` and `roundzero.f16f16` (116 each).

Per-shader stats are comparable: mean shader code size 40.6 (off) vs 41.2 (on), **zero spills in
both**.

## The pass bisect is negative

Skipping each of PCO's 19 passes in turn (`PCO_SKIP_PASSES=<pass>`) against the single-scene
reproducer with the feature on:

| outcome | passes |
|---|---|
| still fails | `pco_bool`, `pco_cf`, `pco_const_imms`, `pco_dce`, `pco_end`, `pco_opt_back_prop`, `pco_opt_comp_only_vecs`, `pco_opt_fwd_prop`, `pco_pre_ra_legalize`, `pco_schedule`, `pco_opt_prop_hw_comps`, `pco_opt_lower_mods`, `pco_shrink_vecs` |
| crashes (skip invalidates codegen, expected) | `pco_ra`, `pco_opt_prep_mods`, `pco_post_ra_legalize`, `pco_opt` |
| breaks codegen (no frames) | `pco_group_instrs` |

**No pass removal makes validation pass**, so this is not one optional transformation.

## The competing hypothesis that must be ruled out FIRST

**It may not be a miscompile at all.** Advertising `shaderFloat16` makes zink implement GLES
`mediump` with fp16 - *lower* precision than the fp32 it used before. glmark2's reference images were
generated with the previous, more precise path, and `--validate` compares with a fixed tolerance.

Every failing scene is maths-heavy, and `conditionals` flips from pass to fail purely on
`fragment-steps`, which is exactly what accumulated rounding error looks like.

If that is what is happening, the feature is being **correctly** implemented and "20 of 27 scenes
wrong" was the wrong framing from the start.

## The decisive experiment (next)

Render one failing shader twice - fp16 and fp32 - read the framebuffer back with `glReadPixels`, and
measure the actual per-channel difference:

* a few LSB -> precision; feature is correct; the validation tolerance is the issue;
* garbage / inverted / NaN -> genuine miscompile.

**Those two outcomes call for opposite actions, so no PCO change is made until this is answered.**

## Operational note

The PCO dumps for this work are ~1.2M lines each. They filled `/tmp`, which is a **2.9 GB tmpfs
(RAM-backed)** while the root fs and SD card were fine, which stalled the session for several rounds
because the harness writes tool output to `/tmp` and every command failed with ENOSPC before running.
`PCO_DEBUG_PRINT=all` output must be bounded (`| head -c`) and written to the SD card
(`/mnt/sdcard/_REVIEW/emulation/dumps/`), not to `/tmp`.
