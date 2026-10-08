# Tiling geometry correct; per-sample PBE path is where the 2.45x must live

## Tiling actually used (PVR_TILE_TRACE)

```
[tile] rt 2048x2048 samples=1 -> tiles 128x128 mtiles 4x4 tiles_per_mtile 32x32 x_tile_max=127 y_tile_max=127
[tile] features: simple_internal_parameter_format=1 gpu_multicore_support=1 process_empty_tiles=1 -> skip_init_hdrs=1
[tile] rt  512x512  samples=1 -> tiles  32x32  mtiles 4x4 tiles_per_mtile  8x8  x_tile_max=31  y_tile_max=31
```

16x16-pixel tiles, correct for this BVNC. `skip_init_hdrs=1` active. **Tiling is not the problem**,
and the mtile grid is 4x4 at both sizes (constant), so it cannot explain an area-scaling deficit.

## Caution: "KMS matches the vendor" is not a render rate

`pvranimate` presents 4K by page flip and is **vsync-capped** (flat 45-56 fps across sizes). Its
396 Mpix/s is a *present* rate. It shows the KMS **present** path is fine; not that the renderer is at
vendor speed. Open renderer ~300 Mpix/s vs vendor ~743.

## Excluded for the render half

shader instruction stream (measured null), PCO codegen (uniform trivial vs 640-op), GPU clock (1104
both), tile partition (6144 both), tiles in flight (6), phantoms (1), user sample shading,
bytes/pixel (flat r8->rg16), memory layout/exportability, tiling geometry (correct), kernel/userspace
split (blocked by 24.2-vs-1.17 ABI).

## The remaining clue, stated precisely

Per-sample, bytes-independent, area-scaling: ~2.4x at 1 sample, 4.45x at 4. And the asymmetry is the
key - the **vendor gets more efficient per sample as MSAA rises** (2.58x for 4x) while the **open
driver gets less** (4.87x). That is not what bandwidth or a fixed per-pixel cost does; it points at
sample-position/coverage handling in the raster/PBE setup.

## Next

Check what the driver programs for sample locations and `rasterization_samples` into the ISP/PBE
state (`pvr_arch_cmd_buffer.c` around the `BITFIELD_MASK(ms.rasterization_samples)` use,
`pco_fs_data.sample_locations`) and whether MSAA takes a per-sample path the vendor avoids.
`SAMPLES=4` ratio is the discriminator: a fix should move 4.87x toward 2.58x.
