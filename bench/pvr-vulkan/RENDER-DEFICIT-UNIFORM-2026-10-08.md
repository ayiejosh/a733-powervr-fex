# The render deficit is UNIFORM (2.4-2.5x) at the SAME clock - not PCO, not clock

## Method

`vkheavy` = vkrender with a 32-iteration fragment shader (2 transcendentals + several ALU per
iteration, ~640 ops/pixel). Both probes are API-portable and need no compositor, so the vendor run
uses only `pvrsrvkm` + `img_icd.json`.

At 2048:

| probe | open (Mesa pvr) | vendor (libVK_IMG) | ratio |
|---|---|---|---|
| trivial fill | 298.8 Mpix/s | **743.7 Mpix/s** | **2.49x** |
| heavy shader | 9.7 Mpix/s | **23.3 Mpix/s** | **2.40x** |

At 1024 vendor: fill 619.6, heavy 23.0 Mpix/s.

## Two categories eliminated

* **Not PCO codegen.** A 640-op shader and a trivial fill show the *same* deficit (2.40x vs 2.49x).
  Pathological codegen for real shaders would show up far worse in the heavy case. It does not.
  **This closes the PCO-as-render-bottleneck hypothesis.**
* **Not the GPU clock.** `pll-gpu` and `gpu0` = **1104000000 under both drivers**, identical. The DT
  overlay supplies `clk_rate` for the vendor driver and the clock is unchanged with the open driver.

## What it means

**A uniform ~2.4-2.5x deficit at the same clock means the hardware runs at ~40% efficiency under the
open driver**, for pixel-fill and ALU-bound work alike. Not a compiler problem, not a clock problem -
it is how the driver configures or feeds the hardware.

Candidates, much narrower now:

1. **Pixel-backend / tile configuration** - tile size, tile-buffer count, PBE setup; would slow fill
   and ALU work alike.
2. **Core/pipe utilisation** - `core_count = 1` is correct for a 1-core BXM-4-64, but other per-core
   setup (ISP/OCLQ stride, phantom handling) may be under-configured.
3. **Per-job submission overhead** - though a 2048 fill is 14.0 ms vs the vendor's 5.6 ms, too large
   for pure submission overhead.

## Next

Compare the driver's hardware setup against the vendor's for the same GPU - tile/PBE configuration and
hardware jobs per frame first. A uniform fill-rate deficit is the signature of pixel-backend
configuration. `vkrender`/`vkheavy` give a fast compositor-free way to measure any change.
