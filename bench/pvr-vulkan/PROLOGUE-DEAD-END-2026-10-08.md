# The fragment-prologue lead was a dead end: measured null, patch reverted

## The experiment

`pco_nir_lower_sample_mask_out` (`pco_nir_pvfio.c:501`) inserts an unconditional per-sample check into
every user fragment shader:

```c
insert_sample_check(&b, NULL);   /* mask = (1 << gl_SampleID) & gl_SampleMaskIn; discard_if(0) */
```

That is the ~11-instruction sequence visible in the dumped IR, and at 1 sample it is provably a no-op
(`gl_SampleID = 0`, `1 << 0 = 1`, `gl_SampleMaskIn = 1`). I guarded it on the sample count and measured:

| | baseline | with patch |
|---|---|---|
| 2048 s1 | 303.7 Mpix/s | 302.9 Mpix/s |
| 2048 s2 | 164.7 | 160.6 |
| 2048 s4 | 62.4 | 62.5 |
| heavy | 9.7 | 9.7 |

**Null.** A fill at this size is not instruction-bound, so "56 instructions for a trivial shader",
while true, does not explain the 2.45x.

## Established along the way

1. **`data->fs.rasterization_samples` is never assigned anywhere.** Declared at `pco_data.h:103`, read
   at `pco_nir.c:1143`, written nowhere; an instrumented build printed `samples=0`. **A latent bug in
   its own right**, and the reason the guard degenerated to "always skip".
2. **The `savmsk` sequence is not from the patched pass.** It is already present in
   `shader ir before passes`, predating the whole PCO pass pipeline. Disabling
   `pco_nir_lower_alpha_to_coverage` removed only 7 of 56 instructions and left `savmsk`.
   `insert_sample_check`, `lower_sample_mask_in`, `lower_sample_pos` and alpha-to-coverage are all
   ruled out. **Origin still unidentified.**

## Why it had to be reverted regardless

`rasterization_samples` is always 0, so `samples <= 1` is always true: the guard would skip the sample
check **for genuinely multisampled pipelines too**, silently removing sample-coverage discarding. A
correctness regression that a fill benchmark would never show. Reverted; `git diff` clean, `bda`
passes, baseline restored (292.7 Mpix/s).

## Lesson

I inferred a performance cause from **instruction count** without measuring first, and the measurement
said no. The 2.45x per-pixel deficit is **not in the shader instruction stream** - it is downstream, in
fixed-function/raster/tile processing, consistent with it being independent of bytes/pixel and scaling
per sample.

## Next: kernel-level vs user-level driver

The open stack pairs Mesa userspace with the mainline `powervr` module; the vendor pairs its userspace
with `pvrsrvkm`. Since the deficit is per-pixel, per-sample and bytes-independent, the fixed-function
setup the userspace hands the kernel - and what the kernel/firmware programs into the ISP/PBE - is
where to look next.
