# Region-header count is correct — hypothesis refuted, and a macro trap documented

## The hypothesis

`pvr_rt_get_isp_region_size()` allocates one region header **per 2×2 tile group** only when
`simple_parameter_format_version == 2`, otherwise one **per tile** — 4× as many headers for a 2048 surface
(16384 instead of 4096). If our device did not report version 2, that would be a 4× per-surface cost, close
to the measured **~3.3×**.

## The check

`bxm-4-64.h:79` declares **`.simple_parameter_format_version = 2U`**, so the `/= 4` branch **is** taken and
the header count is correct. **Hypothesis refuted.**

## The trap I fell into, worth recording

The guard reads:

```c
if (PVR_FEATURE_VALUE(dev_info, simple_parameter_format_version, &version)) {
   version = 0;
}
```

which *looks* inverted — as if it zeroed the version precisely when the read succeeded, skipping the
`/= 4`. It is not. The macro returns **0 on success** and `-EINVAL` on failure:

```c
#define PVR_FEATURE_VALUE(dev_info, feature, value_out)   \
   ({ int __ret = -EINVAL;                                \
      if (__dev_info->features.has_##feature) {           \
         *(value_out) = __dev_info->features.feature;     \
         __ret = 0;                                       \
      } __ret; })
```

So the condition is true only when the lookup **failed**, and `version = 0` is the intended fallback.
**The code is correct.**

> **Note for future reading: `PVR_FEATURE_VALUE` uses the 0-is-success convention, so `if (PVR_FEATURE_VALUE(...))`
> means "if the feature is MISSING".** Reading it as a normal success test inverts the logic — the kind of
> thing that produces a confident but wrong bug report.

## Per-surface cost: the candidate list keeps shrinking

Now excluded, each by measurement or code reading:

fill rate · bytes/pixel · **attachment format** · memory layout · tile geometry (16×16) · macrotile grid
(4×4) · **region-header count** · MSAA architecture · PBE state · FBCDC · empty-tile processing · the
fragment shader (≈15% of the render) · the geometry/TA job (**faster** than the vendor's).

**What is left is per-tile work in the fragment job that does not depend on coverage, format or tile
geometry.**
