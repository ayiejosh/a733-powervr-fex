# On-chip memory: the mechanism is real but the driver's usage is already minimal

From [On-Chip Memory Performance](https://docs.imgtec.com/performance-guides/graphics-recommendations/html/topics/on-chip-memory-performance.html):

> "Every PowerVR Rogue, Volcanic (and later) architecture GPU contains some amount of on-chip memory,
> typically 256 bits on high end GPUs and 128 bits on low end GPUs... This memory is used to accelerate
> some of the per-fragment fixed-function pipeline such as alpha blending, depth testing, and stencil
> testing."
>
> "On-chip memory has a finite amount of bandwidth; Bits used for storage cannot be used elsewhere, such
> as for register space."

Their GX6250 measurements show frametime rising with usage: 96bit+D32 = 20 ms, 160bit = 23 ms,
256bit = 29 ms, 288bit = 39 ms.

**A real mechanism for a PBE deficit** - more bits per pixel means less bandwidth for the fixed-function
pipeline and fewer register bits for shaders. It also explains the MRT page's note about reduced USC
occupancy.

## But it does not explain this deficit - usage is already minimal

```
partition_size = pixel_width(2) * 16 * 16 = 512 dwords over 256 px = 64 bits/pixel
pixel_width = MAX2(job->pixel_output_width, usc_min_output_registers_per_pix = 2)
```

**64 bits/pixel against a 128-bit recommendation and a 96-bit floor in Imagination's own table** - the
driver is *below* the tested range and allocating the minimum. No excess to remove.

## Tally and boundary

**Seventeen hypotheses tested and refuted**, including every variable the driver exposes and both
remaining guide leads. The two open performance terms (PBE write 3.2x; Tiler 4x-MSAA excess 8.16x vs
3.06x) are both at the closed-firmware boundary:

* the UAPI exposes **no timing facility** - only static `DEV_QUERY` (gpu_info, runtime_info, quirks)
* PVRtune is not installed
* the vendor's command stream is inside `libVK_IMG`

**Further progress needs an instrument this board does not have, not another hypothesis.**
