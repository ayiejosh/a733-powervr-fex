# MSAA amplifies the deficit 2.4x -> 4.45x: it is the tile resolve path

## Sweep (vkrender, 2048, 20 iters, both drivers)

| samples | open | vendor | ratio |
|---|---|---|---|
| 1 | 303.7 Mpix/s | 717.7 Mpix/s | 2.36x |
| 2 | 164.7 Mpix/s | 418.7 Mpix/s | 2.54x |
| **4** | **62.4 Mpix/s** | **277.7 Mpix/s** | **4.45x** |

Scaling differs in kind:

```
vendor  717.7 -> 418.7 -> 277.7   (4x MSAA costs 2.58x - better than linear)
open    303.7 -> 164.7 ->  62.4   (4x MSAA costs 4.87x - worse than linear)
```

The vendor gets more efficient per sample as MSAA rises; the open driver gets less. **The gap nearly
doubles with multisampling on.**

## What it names

MSAA's dominant extra cost is tile-memory traffic: the tile buffer holds N samples per pixel and must
be **resolved/stored** at end of tile. A deficit of 2.4x at 1 sample and 4.45x at 4 samples is
therefore in the **tile resolve/store path** - not the shader (excluded: trivial fill and a 640-op
shader both 2.4x) and not the partition size (identical 6144).

It also explains the flat ~300 Mpix/s plateau at 1 sample: a fixed per-pixel tile store/resolve cost
caps throughput independently of area, which is the measured shape.

## Next

Inspect the end-of-tile (EOT) / resolve path: `pvr_arch_cmd_buffer.c` EOT setup,
`pvr_arch_job_render.c` PBE state, and tile-buffer allocation (`pvr_device_tile_buffer_ensure_cap`,
`pvr_get_tile_buffer_size`). A resolve writing more than it should - or an EOT program running per
tile rather than per render - gives exactly this signature.

**`SAMPLES=4` is now a sharp discriminator**: any fix should move 4.45x toward the vendor's 2.58x
scaling, measurable in seconds with the compositor-free probe.
