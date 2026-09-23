# Mesa pvr: push constants are broken for Vulkan 1.0–1.3 applications (two issues)

Target: `mesa-25.3.0`, driver `src/imagination/vulkan` (`pvr`), device PowerVR B-Series
BXM-4-64 MC1 (BVNC 36.56.104.183) on Allwinner A733, kernel `powervr` 1.0.0 (v6.8 sources
adapted to a 6.6 BSP kernel).

Reproducer: `trixie-prep/bench/pvr-vulkan/pvranimate.c` — an animated offscreen render whose
per-frame phase arrives in a push constant, presented with `drmModePageFlip`, with each frame's
first pixel read back and compared against the value the shader should have produced.

## Issue 1: `vkCmdPushConstants` is a silent no-op (patch provided)

The generated dispatch table expects the core entry point:

```
build/src/imagination/vulkan/pvr_entrypoints.c:
    .CmdPushConstants = pvr_CmdPushConstants,
```

but the driver only defines `pvr_CmdPushConstants2KHR` (the Vulkan 1.4 spelling), so
`vkCmdPushConstants` resolves to the generated entrypoint **stub**. Applications written against
Vulkan 1.0–1.3 get no push constants, no error, and no validation message — the values are simply
never applied.

Observed: pushing `0.0` and then `16.0` produced byte-identical frames (`pixel 0 = 2,2` in both
cases; the expected value for `16.0` is `66`).

Fix: `mesa/0003-pvr-implement-core-CmdPushConstants.patch` — implement the core entry point by
delegating to the 2KHR variant. With it, pushes have an effect (see issue 2 for the remaining
problem).

## Issue 2: a pushed value is applied one submission late (not fixed)

After issue 1 is fixed, the value a frame renders with is the value pushed for the **previous**
submission. Measured with a four-case self-test, each case a separate submission of the same
command buffer, verifying pixel 0 of the rendered image:

```
phase  0, push after bind   -> 2     (expected 2)
phase 16, push after bind   -> 2     (expected 66)
phase 16, push before bind  -> 66    (expected 66)
phase  0, push before bind  -> 66    (expected 2)
phase 32 pushed twice       -> 129   (expected 129)   <- steady state is correct
```

The value observed in a given submission is always the one pushed in the previous submission,
independent of whether the push comes before or after `vkCmdBindPipeline`; pushing the same value
twice is a workaround. In a continuous animation the effect is a one-frame lag, which is why the
presented frames do advance (`2 0 2 18 34 50`), but a single update is never visible.

The first case's value also varies between runs (`225` in one run, `2` in another), which suggests
the stale data is not a simple shadow copy but something involving suballocated upload buffers.

What is known about the mechanism, from reading the code (hypotheses, not results):

- `pvr_cmd_upload_push_consts()` uploads `state->push_consts[stage].data` into a **newly
  suballocated** buffer via `pvr_cmd_buffer_upload_general()` and stores the resulting
  `dev_addr`. The address is what gets baked into the pipeline's special-buffer state.
- The graphics draw path does re-upload when `dirty` (`pvr_cmd_buffer.c`, the
  `PVR_STAGE_ALLOCATION_FRAGMENT` / `VERTEX_GEOMETRY` checks before descriptor emission) and marks
  the descriptors dirty, and the compute path does the same in `pvr_cmd_dispatch()`. So the flag
  handling looks right.
- That leaves the address baked into the pipeline state: if the descriptor/special-buffer emission
  that runs at draw time still refers to the address from the previous upload, the draw reads the
  previous data. Re-uploading into the *same* buffer, or re-emitting after the upload, would be
  the place to look.

## Why this matters beyond one test

Push constants are how a compositor or a game passes per-draw data without a descriptor write.
A silent no-op means a port of anything non-trivial renders wrongly with no diagnostic — and the
latency means even a fixed build lags by a frame unless the value is pushed twice.

## Reproducing

```sh
cd trixie-prep/bench/pvr-vulkan && ./build.sh
# with the mainline powervr driver loaded (see kernel/open-driver-spike/stage4-mainline-vulkan.sh):
sudo ./pvranimate 1920 1080 240        # prints the phase self-test and the presented pixels
```

Without issue 1's patch applied, the self-test reports `push constants: IGNORED` and the first six
presented frames are all identical.
