# ISP anti-aliasing excluded — the fragment path's configurables are now all accounted for

`pvr_arch_job_render.c` derives the rasterizer's AA mode from the sample count:

```c
const enum ROGUE_CR_ISP_AA_MODE_TYPE isp_aa_mode =
   pvr_cr_isp_aa_mode_type(job->samples);   /* 1 → AA_NONE, 2 → AA_2X, 4 → AA_4X, 8 → AA_8X */
```

**For a 1-sample render this is `AA_NONE`, so the rasterizer carries no anti-aliasing work in `vkrender`.**
Excluded.

## The fragment path's configurables, all checked

| configurable | value for `vkrender` @ 1 sample | verdict |
|---|---|---|
| `tile_size_x/y` | 16 × 16 | correct (device feature) |
| `mtiles_x/y` | 4 × 4 | structurally required |
| `mtile_x1..x3`, `x_tile_max` | 32, 0, 0, 127 | **coverage exactly matches the surface** |
| `tiles_per_mtile_x` | `mtile_x1 × samples_in_x` | correct |
| `simple_parameter_format_version` | 2 | one region header per 2×2 group |
| **`CR_ISP_AA` mode** | **AA_NONE** | **no AA work** |
| `CR_ISP_CTL.sample_pos` | true | standard |
| `CR_ISP_CTL.process_empty_tiles` | 1 | the vendor scales the same way |
| `CR_ISP_CTL.skip_init_hdrs` | true | optimised path taken |
| `CR_ISP_CTL.dbias_is_int` | needs enhancement 42307 + integer depth | not in this probe |
| PBE / attachment format | 4× fewer bytes → frame moves ~10% | **PBE < 10% of the render** |

## Conclusion

**Every configurable the driver sets for the fragment path has been read and is either correct or
immaterial.** The gap is therefore **not a misconfiguration the source exposes** — it is in how the hardware
executes the same configuration, or in something only visible by diffing the emitted command stream against
the vendor's.

**That is the boundary:** the instruments that would settle it — **PVRtune**, or a diff of the vendor's and
the open driver's command streams for the same draw — are not available. PVRtune is not installed; the
vendor's stream lives inside the closed `libVK_IMG`.

## What the session delivered, in one place

1. **A committed, verified fix** — `c2bde57`, PCO `max_unroll_iterations` 16 → 64: **1.85×** (`cstpi`),
   **2.28×** (`cstpf`), **2.85×** (`vkheavy`), with **`vkrender` unchanged as the control**. Correctness green
   (all probes + 27 glmark2 scenes). **Scope stated honestly: no change on the default suite.**
2. **The loop gap decomposed into three measured terms** — unrolled (fixed), immediates (**1.40×**),
   register moves (**~2.1×**); the latter two both PCO codegen, each with a stated measurement to beat.
3. **The kernel-side bottleneck quantified** — 84% of frame time in the kernel, ~190 syncobj ioctls/frame,
   **~17 ms/frame** recoverable — and **proved unreachable from Mesa** (the kernel holds references by
   handle, so pooling aliases in-flight jobs; the failure mode is a hang).
4. **Seven measurement rules and eleven withdrawn claims**, each earned by catching a specific error.
