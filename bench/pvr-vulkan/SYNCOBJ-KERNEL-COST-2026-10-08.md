# The per-frame fixed cost is KERNEL syncobj time - target (3) measured at last

Same probe, both drivers, 64x64 x 2000 iterations, wall vs CPU:

| | open (`powervr`) | vendor (`pvrsrvkm`) | ratio |
|---|---|---|---|
| wall | 2.063 s | 1.002 s | 2.06x |
| user | 0.334 s | 0.241 s | |
| **sys** | **0.785 s** | **0.251 s** | **3.13x** |
| reported ms/frame | 1.001 | 0.469 | 2.13x |

The open process blocks in:

```
wchan = drm_syncobj_array_wait_timeout.constprop.0
```

**Per frame: open 0.39 ms kernel vs vendor 0.126 ms - 0.27 ms/frame extra in
`drm_syncobj_array_wait_timeout`.** This is objective target (3) - "the driver implements vk_sync as
DRM syncobj operations (one ioctl each) where the vendor uses a driver-native sync type" - **measured
for the first time rather than suspected.**

## What it is and is not

* **Kernel time, not render**: user time is nearly identical (0.334 vs 0.241 s), so this is the
  submit/wait path, not shading.
* **The cost is the wait, not create/destroy**: the process blocks in
  `drm_syncobj_array_wait_timeout`. Target (1) suggests pooling/timeline-backing the per-job vk_sync
  objects; pooling addresses object churn, but the measured cost is the wait itself, so **pooling
  alone may not move it - measure before doing that work.**
* **Small next to the main gap**: 0.27 ms/frame is ~1.6% of a core at 60 fps. Real, worth having
  measured, and not the main deficit.

## Fit with the decomposition

Gap = render 2.45x (per pixel) x present 5.4-8.9x. This sits inside the render half's fixed term
(0.85 ms open vs 0.52 vendor) and explains about half of that difference; ~0.26 ms remains
unattributed.

## Next

(a) Reduce syncobj waits per frame - how many objects are in the wait array, does the driver wait on
more than it needs, does poll-before-wait help; (b) confirm whether the vendor's native wait is
cheaper or simply called less often, by counting waits per frame on both sides. Both measurable with
the same wall/CPU/wchan method.
