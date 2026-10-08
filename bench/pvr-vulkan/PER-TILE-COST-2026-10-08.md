# The surface cost is a per-TILE cost, 3.68x the vendor's - mechanism found

## The code

`pvr_arch_job_render.c:1138` computes tiling from the **full render-target dimensions**:

```c
pvr_arch_rt_mtile_info_init(dev_info, &tiling_info,
                            rt_dataset->width, rt_dataset->height, rt_dataset->samples);
```

and inside:

```c
info->num_tiles_x = DIV_ROUND_UP(width, info->tile_size_x);
info->x_tile_max  = info->num_tiles_x - 1;
info->y_tile_max  = info->num_tiles_y - 1;
```

**The tile range comes from the full surface, not the render area** - which is exactly why
`AREA=quarter` did not help: shrinking the render area does not shrink the tile range.

## Refinement: per-tile, not "wasted tiles"

For a real full-surface pass the tile range is correct anyway, so this is not waste from a smaller
render area. Linearity across targets:

| target | open | vendor |
|---|---|---|
| 1024x1024 | 4.20 ms | 1.69 ms |
| 2048x2048 | 14.87 ms | 5.66 ms |
| growth for 4x surface | 3.54x | 3.35x |

**Both scale linearly with surface area, so both pay a per-tile cost - and the open driver's is 3.68x
the vendor's for the same tile count.**

## What it is not

* **Not the tile load/store** - `LOADOP`/`STOREOP=dontcare` changed nothing (294-308 vs 300 Mpix/s).
* **Not the partition or tiles-in-flight** - both match the device info exactly (6144; 6).
* **Not the tiling geometry** - 16x16 tiles, `skip_init_hdrs=1`, correct.
* **Not the shader** - removing 24 prologue instructions changed nothing.

**So it is per-tile work other than load/store: the EOT program and its per-tile dispatch, or the
per-tile ISP/PBE setup.** The EOT is built once per job but *executes per tile* - exactly the shape of
a coverage- and format-independent per-tile cost.

## Next

Inspect what goes into the EOT/PBE program (`hw_render->eot_setup`, the PBE state words; `pbe_emits = 1`
for one colour attachment). `AREA`, `MODE`, `SAMPLES` remain the separators; target is the vendor's
per-tile rate (3.69 ms for a 2048x2048 surface).
