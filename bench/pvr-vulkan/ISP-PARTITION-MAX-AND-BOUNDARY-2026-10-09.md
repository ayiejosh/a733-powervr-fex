# The ISP partition / tiles-in-flight path is at its maximum — last numeric lead closed

`pvr_arch_setup_tiles_in_flight()` derives the ISP partition size from the shader's per-pixel
output-register demand:

```c
usc_min_output_regs = PVR_GET_FEATURE_VALUE(dev_info, usc_min_output_registers_per_pix, 0);
pixel_width = MAX2(pixel_width, usc_min_output_regs);
pixel_width = util_next_power_of_two(pixel_width);
partition_size = pixel_width;      /* bigger pixel_width → fewer tiles in flight */
```

A larger per-pixel register demand would inflate the partition and reduce how many tiles the ISP keeps in
flight — a plausible per-pixel throughput cost. **Checked against the device:**

| feature | our device | implication |
|---|---|---|
| `isp_max_tiles_in_flight` | **6** | the driver's computed 6 is the **device maximum**, not a conservative choice |
| `isp_samples_per_pixel` | 1 | no MSAA partition multiplier at 1 sample |
| `usc_min_output_registers_per_pix` | **2** | a floor the shader's demand is raised to, not an over-request |

**The driver reaches the device maximum of 6 tiles in flight — the ISP is not starved.** Excluded.

## The boundary, stated plainly

**Every numeric and structural configurable the driver exposes for the fragment path has now been read and
is at its correct or maximum value**, and every behavioural candidate has been tested and excluded:

fill rate · bytes/pixel · attachment format · memory layout · tile size (16×16) · macrotile grid (4×4) ·
region-header count · tiles in flight / ISP partitions · ISP AA mode · `process_empty_tiles` ·
`skip_init_hdrs` · MSAA architecture · PBE state · FBCDC · the fragment shader (~15% of the render) · the
geometry/TA job (**faster** than the vendor's) · the per-job sync interface (separate, kernel-side).

**The remaining ~2.47× on the raw render is therefore not a configuration the driver gets wrong that the
source reveals.** Settling it needs either:

- **PVRtune** — not installed; it would name Tiler vs Renderer vs USC utilisation directly; or
- **a diff of the open driver's and the vendor's emitted command streams for the same draw** — the vendor's
  lives inside the closed `libVK_IMG`; or
- **a UAPI timing facility** — `drm/imagination` has none (only static `DEV_QUERY`).

**That is the honest end of what this method reaches on this board.**
