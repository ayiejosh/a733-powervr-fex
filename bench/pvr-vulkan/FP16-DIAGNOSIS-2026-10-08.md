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

---

# ANSWERED: it is a miscompile, not precision (minimal reproducer)

`fp16cmp.c` renders one `mediump` fragment shader and reads the framebuffer back with
`glReadPixels`, at several uniform inputs, with a `highp` control for the same maths.

```
shader        fp16 OFF              fp16 ON
highp  (ctl)  u=0.10 -> 255 173  87  u=0.10 -> 255 173  87   (identical)
mediump       u=0.10 -> 255 173  87  u=0.10 ->   0   0   0   (BLACK)
```

All six probe inputs: correct gradients with fp16 off, **`0/0/0` for every one with it on**
(`black_pixels=6`).

* `glGetError = 0x0` in every case, so the pipeline is valid and the draw succeeds.
* The `highp` control gives a **byte-identical checksum** (`5960197479723617773`) in both modes,
  which proves the harness is sound and isolates the fault to the `mediump` (fp16) path.
* The shader's own checksum with fp16 on is `0` - it computes nothing at all.

## So the "precision" hypothesis is refuted

A tolerance argument would show a few LSB of difference. This is total black output at every input:
**PCO's fp16 emit produces a shader that does not compute its result.** The original framing - that
advertising `shaderFloat16` makes 20 of 33 glmark2 scenes render wrong - was right.

## Why this matters for the fix

The reproducer is one 12-line fragment shader in a single process, so the compiler can now be
inspected on a case small enough to read:

```
fp16cmp's fragment shader:
  precision mediump float;
  uniform float u;
  void main(){
    mediump float x = u;
    for (int i=0;i<8;i++) x = x*1.1 + 0.1;
    gl_FragColor = vec4(x, x*0.5, x*0.25, 1.0);
  }
```

Dump its PCO IR with `PCO_DEBUG_PRINT=all` - bounded, to the SD card - and diff fp16 against fp32.
The `pco_opt*` passes and the fp16 `unpck`/`roundzero` lowering are the places to look, and the
negative 19-pass bisect already tells us it is shared lowering rather than one optional pass.

**Reproduce with:**
```
MESA_SHADER_CACHE_DISABLE=true ./fp16cmp          # fp16 off
MESA_SHADER_CACHE_DISABLE=true PVRSRV_FP16=1 ./fp16cmp   # fp16 on -> black
```
