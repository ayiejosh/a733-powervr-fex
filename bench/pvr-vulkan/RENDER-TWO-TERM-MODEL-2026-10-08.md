# Render decomposed: per-pixel 2.45x, per-frame only 1.63x

Size sweep at high iteration count on both drivers so the fixed cost falls out of the slope:

| size | open | vendor |
|---|---|---|
| 64x64 (0.004 Mpix) | 0.863 ms/frame | 0.524 ms/frame |
| 512x512 (0.262 Mpix) | 1.75 ms/frame | 0.924 ms/frame |
| 2048x2048 (4.19 Mpix) | 14.209 ms/frame | 5.952 ms/frame |

Fit `frame_ms = a + b * Mpix`:

| term | open | vendor | ratio |
|---|---|---|---|
| fixed per-frame | **0.85 ms** | **0.52 ms** | 1.63x |
| per-Mpix (slope) | **3.19 ms** | **1.30 ms** | **2.45x** |

Open's fit agrees with the earlier 512/1024/2048 sweep (0.93 + 3.11).

**The render deficit is overwhelmingly per-pixel: 2.45x.** The fixed per-frame term is only
0.33 ms/frame worse (~2% of a core at 60 fps). Marginal rates: **open 314 Mpix/s, vendor
769 Mpix/s.**

## Excluded for this 2.45x

| hypothesis | evidence |
|---|---|
| shader codegen | uniform across a 1-instruction fill and a 640-op shader |
| GPU clock | 1104 MHz under both drivers |
| tile partition | 6144 both, exactly |
| tiles in flight | 6, matching isp_max_tiles_in_flight |
| phantoms | 1, asserted and by design |
| user sample shading | set from sample_shading_enable, correct |
| bytes/pixel | flat r8 -> rgba8 -> rg16 |
| linear/exportable memory | same rate as optimal |

## Conclusion

**The driver executes each pixel ~2.45x slower than the vendor, for the same shader and the same
hardware.** A per-invocation cost in how the driver feeds the USC/ISP - not any configuration value
checked so far.

## Next

Dump the same trivial fragment shader from the open driver
(`PCO_DEBUG_PRINT=passes,fs,internal,vs,nir,binary`) and count what is emitted per invocation. If the
driver emits materially more per-pixel work (PDS/parameter loads, extra state) than a minimal fill
needs, that is the 2.45x. `vkrender`/`vkheavy` + `SAMPLES=4` remain the fast measurement.
