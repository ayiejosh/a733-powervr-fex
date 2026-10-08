# weston's compositing shaders: 100-300 instructions, no loops - PCO bloat not established

## Capture

Started weston with `PCO_DEBUG_PRINT=passes,fs,internal,vs,nir,binary`, cache cleared, **stderr
redirected** (the `--log` option swallows driver output):

```
stderr            32,346,970 bytes
PCO pass dumps    25,886
shader names      34,621
user shaders      70 MESA_SHADER_FRAGMENT, 50 MESA_SHADER_VERTEX (unnamed, with source_blake3)
```

## Instruction histogram of the compositing fragment shaders

```
  0- 99: 107
100-199: 115
200-299: 121
300-399:  18
400-499:  34
loops:     0
```

Substantial (weston's GL renderer does colour management and blending) but **not an obviously
pathological lowering, and not enough by itself to explain a 10x gap.**

Correction: an earlier figure of "median 564 instructions" in this session was the maximum
instruction *index*, not the count. Wrong; do not quote.

## The weakness in the 10x attribution

"compositor term = 69.5 ms = 30 Mpix/s" was derived by **subtracting the vkrender model from the
composited frame**. That assumes the client's render costs the same in the composited case as
vkrender's offscreen render - **unverified**. In the composited case the client renders into a
dma-buf-backed exportable swapchain image, and cache/coherency handling for exportable memory can
differ. If the client's render is slower there, the 10x is partly misattributed.

**So 30 Mpix/s is a derived number on an unchecked assumption, not a measured property of weston's
composite.**

## What stands

The **direct KMS-vs-composited A/B is unaffected**: 55.8 vs 13 fps at 1080p, 56.2 vs 58 at 640x480.
A ~64 ms/frame cost exists in the composited path at 1080p; its internal split between the client's
render and weston's composite is **not** established.

## Next

Measure the client's render cost *in the composited case* rather than assuming it equals the
offscreen model: bracket the client's own submit, or compare against rendering to a non-exportable
image. Use the explicit-sync release to separate the client's GPU work from the composite.
