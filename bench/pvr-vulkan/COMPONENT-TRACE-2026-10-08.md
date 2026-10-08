# Full component trace — open vs vendor, Radxa Cubie A7A (PowerVR BXM-4-64, A733)

Measured 2026-10-08 on the open stack (`powervr` bound), plus the vendor figures from this
repository's own A/B (`results-2026-09-23-ab-vendor.txt`, `render-gap-vendor.txt`).
**Provenance is marked: [M] = measured in this run, [R] = from the repository record.**

## The trace, hardware outward

| # | stage | open | vendor | ratio |
|---|---|---|---|---|
| 0 | GPU fill 512² (`vkrender`) | 94.3 Mpix/s / 377 MB/s [M] | **379.7 Mpix/s** [R] | **4.0×** |
| 0 | GPU fill 1024² | 208.0 / 832 MB/s [M] | **601.9** [R] | **2.9×** |
| 0 | GPU fill 2048² | 283.2 / 1133 MB/s [M] | **696.3** [R] | **2.5×** |
| 0 | GPU fill 4096² | 309.7 / 1239 MB/s [M] | (PASS, no rate) [R] | - |
| 1 | GPU copy bandwidth | **873 MB/s** [M] | **2670 MB/s** [R] | **3.1×** |
| 1b | CPU read / write (1 MiB) | 270 / 2000 MB/s [M] | - | - |
| 1c | alloc+bind 1 MiB | 1.276 ms [M] | - | - |
| 2 | zink render, offscreen 800×600 | 51–149 Mpix/s (204–594 MB/s) [M] | - | - |
| 3 | **zink windowed X11 800×600** | **15–20 Mpix/s (73–80 MB/s)** [M] | **378 Mpix/s** [R] | **~25×** |
| 4 | zink windowed Wayland | 153 Mpix/s (613 MB/s) [M] | - | - |
| 5 | present call (`eglSwapBuffers`) | 5.5 ms [M] | - | - |
| 6 | Xwayland → weston | 19 commits/s, 19 callbacks/s [M] | - | - |
| 7 | weston composite | 0.58 ms [M] | - | - |
| 8 | KMS flip + render, no compositor, 4K | **396 Mpix/s / 1.58 GB/s** [M] | ~380 [R] | ~1.0× |

## What the trace says

**1. The hardware is fine and the KMS path matches the vendor.** `pvranimate` drives page flips with
no compositor at all: **240 frames in 5021 ms = 47.8 fps at 3840×2160 = 396 Mpix/s = 1.58 GB/s**,
zero flip timeouts. That is level with the vendor's ~380 Mpix/s. The display path and the GPU are not
the problem.

**2. The raw render throughput is 2.5–4× below the vendor**, measured off-screen with no compositor
and no presentation in the way (`vkrender` vs the vendor's render probe). The gap **narrows as the
frame grows** (4.0× at 512² → 2.5× at 2048²), which is the signature of a **fixed per-pass cost**:
the open driver's intercept is ~1.96 ms + 3.11 ms/Mpix against the vendor's much smaller fixed term.
That is the per-pass TA→3D overhead the firmware trace quantifies at **41 firmware operations**
(`gpu_wait` 1.181 ms open vs 0.600 ms vendor).

**3. The windowed path compounds it.** Same scene, same harness (`glmark2 -b build:use-vbo=false`):
open **38 FPS** vs vendor **787 FPS**. Xwayland is not the cause — the vendor reaches 787 through the
*same* weston, the *same* Xwayland and the *same* zink.

**4. Where the windowed time actually goes** (800×600, open): frame 26.4 ms =
5.5 ms inside `eglSwapBuffers` + 30.7 ms outside. And the display loop runs slower than the client:

| | rate |
|---|---|
| client renders | 43 fps |
| Xwayland → weston commits | 19/s |
| weston frame callbacks | 19/s |
| weston repaints | 18/s |

So the client renders 43 frames/s and only ~19 reach the screen, and weston's repaint cycle is ~55 ms
while its composite is 0.58 ms — i.e. **weston is idle in that cycle, not working**. `strace` agrees:
98% of weston's syscall time is `epoll_pwait`, 1% is GPU ioctls.

**5. Wayland is the better path on this stack.** Native Wayland (281 FPS uncapped, 60 FPS vsync) vs
X11 (38–43 FPS) for comparable work. The X11 path additionally does ~200 DRM ioctls per presented
frame in Xwayland, overwhelmingly syncobj create/transfer/destroy.

## The one number needed to finish the decomposition

The like-for-like windowed gap is **25×** (glmark2, both stacks). Splitting it into
"render-throughput × windowed-penalty" needs the **vendor's glmark2 off-screen rate**, which is not in
the record — the vendor numbers here are `vkrender`-style render probes and windowed glmark2, not
glmark2 off-screen. Without it, the split can only be bounded, not measured: the pure-fill gap is
2.5–4×, so the windowed penalty accounts for the remaining ~6–10×.

## Caveats

* `vkrender` measures a pure fill (one full-screen triangle, predictable shader); `glmark2` measures a
  scene. Cross-comparisons between those two are workload changes, not driver differences - the
  like-for-like pairs are marked in the table.
* The vendor column is from 2026-09-23 measurements on the same board; the open column is from today.
* `weston-debug timeline` is lossy and internally inconsistent (401 `repaint_finished` against 226
  `repaint_begin` in one capture), so its *counts* are not rates. The commit/callback counts above
  come from `WAYLAND_DEBUG`, which is a complete protocol trace.

---

# Addendum: per-step costs measured after the WSI fix (2026-10-08, later)

## The fix that moved the numbers

`x11_surface_get_capabilities()` in Mesa's `wsi_common_x11.c` read the window geometry with a
**blocking** `xcb_get_geometry_reply()` on every call - purely to fill `currentExtent` /
`minImageExtent` / `maxImageExtent`. zink calls it once per frame via `zink_kopper_update` ->
`vkGetPhysicalDeviceSurfaceCapabilitiesKHR` -> `dri_st_framebuffer_validate`.

| | calls | total | avg | max |
|---|---|---|---|---|
| before | 200 | 3106 ms | **15.5 ms** | 51.9 ms |
| after | 300 | 4.2 ms | **0.014 ms** | 0.06 ms |

**1230x faster on the call**, and the `gles-x11` phase breakdown confirms it directly:
`draw` **17.34 ms -> 0.10 ms**.

Honest accounting: end-to-end FPS rose only **36.3 -> 40.5 (+12%)**, because the round-trip was
overlapping GPU time - `glFinish` moved 8.47 -> 22.81 ms as the exposed cost.

## Per-step costs now (open stack, 800x600 windowed, gles-x11)

| step | cost |
|---|---|
| `glDrawArrays` (draw) | **0.10 ms** |
| `glFinish` | **22.81 ms** |
| `eglSwapBuffers` | 1.78 ms |
| — of which zink `present` | 0.10 ms |
| — of which zink **`acquire`** | **23.02 ms** |

So the client now blocks ~23 ms per frame **acquiring the next swapchain image**.

## The display loop, measured

| | value |
|---|---|
| weston `repaint_begin -> repaint_posted` (composite) | **0.66 ms** |
| weston `repaint_posted -> repaint_finished` (flip) | **6.78 ms** |
| weston `repaint_finished -> commit_damage` (waiting for the client) | **46.11 ms** |
| weston CPU during a client run | **0.00%** |
| Xwayland CPU during a client run | **56.8% of one core** (utime 1.80 s, stime 3.89 s / 10 s) |
| client renders | 38-40 fps |
| Xwayland commits to weston | 19/s |

**Weston does 7.4 ms of work and is idle for 46 ms; Xwayland is the busy one, mostly in kernel
time.** That is ~15 ms of Xwayland CPU per client frame.

## What Xwayland spends it on (DRM ioctls per client frame)

| ioctl | per frame |
|---|---|
| `SYNCOBJ_TRANSFER` | **56.8** (was 73.1) |
| `SYNCOBJ_CREATE` | **41.6** (was 53.5) |
| `SYNCOBJ_DESTROY` | **41.7** (was 53.5) |
| `SYNCOBJ_TIMELINE_WAIT` | ~10 |
| `PRIME_HANDLE_TO_FD` | ~9 |
| `PVR_VM_MAP`/`UNMAP`, `HL_CB`, `GEM_CLOSE` | ~12 each |

~250 DRM ioctls per frame; at ~60 us each that is ~10 ms of kernel time per frame, which is
where the `stime` goes.

## What was missing on the open driver (found this round)

The **powervr** winsys registered only the binary DRM syncobj type and left `sync_types[1] = NULL`,
while its sibling **pvrsrvkm** winsys builds and registers a timeline type from the same point type
(`pvr_srv.c`). Registering it cut the syncobj churn by 22% (table above) - real, but the remaining
~140 syncobj ops per frame are the binary `VkFence`/`VkSemaphore` objects, one DRM syncobj each,
which a type registration cannot fix.

## The hypothesis this now supports

The vendor is fast through the *same* Xwayland and the *same* zink, so Xwayland's per-frame work must
be cheaper under the vendor ICD. The open driver implements `vk_sync` as DRM syncobj operations
(each one an ioctl into the kernel), where the vendor path uses its own driver-native sync type
(`pvr_srv_sync_type`). **That is the missing piece: a driver-native `vk_sync` implementation for the
powervr winsys**, rather than Mesa's generic `vk_drm_syncobj`.

---

# Addendum: the windowed stall is loop latency, measured on one clock

## Where the client blocks

`AcquireNextImageKHR` itself is the wait (instrumented in zink's kopper and in Mesa's WSI):

```
[acq] AcquireNextImage waits=350 total=6894.8ms avg=19.70ms max=52.12ms
[rel] explicit-sync release waits=300 total=6633.5ms avg=22.11ms max=62.18ms images=3
```

Inside it, `x11_acquire_next_image` -> `x11_wait_for_explicit_sync_release_submission` ->
`wsi_drm_wait_for_explicit_sync_release` -> `device->sync->timeline_wait(...,
DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, ...)`. So the client waits on a DRM syncobj timeline point
for the compositor to release the buffer. The `shm_fence` path is not used. zink's
`present_fence` wait is not hit either.

**22.11 ms of a 27 ms frame is this one wait.**

## Weston's repaint cycle, single clock (weston-debug timeline)

| phase | median |
|---|---|
| `repaint_begin -> flush_damage` (frame callbacks go out) | 0.52 ms |
| `flush_damage -> repaint_posted` | 0.11 ms |
| `repaint_posted -> repaint_finished` (flip) | 6.62 ms |
| `repaint_finished -> repaint_exit_loop` | 9.08 ms |
| **`repaint_exit_loop -> commit_damage`** (waiting for the client) | **36.96 ms** |
| `repaint_finished -> repaint_begin` | 7.22 ms |

Total **53.3 ms = 18.8 repaints/s**. Weston does ~7.4 ms of work per cycle and waits ~37 ms.

The client commits **46 ms after** weston finishes repainting, while its own frame is only 27 ms, so
roughly **19 ms is release-signal latency** - the path weston -> Xwayland -> X -> the client's
syncobj. That is the next thing to measure, and it needs either Xwayland instrumentation (stripped,
no symbols) or a from-source build.

## Ruled out by measurement this round

| hypothesis | test | result |
|---|---|---|
| buffer starvation | swapchain depth 3 / 5 / 7 | max wait unchanged at ~60 ms |
| release mechanism | explicit sync vs forced xshmfence | median 36 vs 35 FPS |
| Xwayland CPU-bound | CPU accounting | 35.8% of a core, ~10 ms/frame - not saturated |
| zink present fence | `ACQ_TRACE` on the present_fence path | never hit |

**Xwayland is at 35.8% of a core (utime 11.1%, stime 24.7%) and weston at 0.00%**, so neither is
saturated: the display loop is **latency-bound**, and the largest single component is the ~41 ms
between weston finishing a repaint and the client's release arriving.

---

# Addendum: two more limit floors, and why the sync fix is deferred

## Floors 7 and 8

| limit | was | now | probe |
|---|---|---|---|
| `maxPerStageDescriptorUniformBuffers` | **13** | **64** | `ubos` (new) |
| `maxPerStageDescriptorSampledImages` | **32** | **128** | `samplers` (already existed) |

`13` was the giveaway: every other descriptor limit was a round number (16/32/64) and 13 sits one
above the spec minimum of 12, with no backing constant. The new `ubos` harness declares an array of
N uniform blocks where element i holds the value i - an array element is one descriptor, so the
shader's sum must be the triangular number, which makes a dropped descriptor show as a wrong value:

```
N=13 got 78 want 78    N=32 got 496 want 496    N=64 got 2016 want 2016
```

`maxPerStageDescriptorSampledImages = 32` had already been disproved by the existing samplers probe
(`sampler2D tex[128]` binds 128 combined image samplers = 128 sampled images); I had simply never
applied it. Both per-set values now derive from the per-stage ones (3x), which also preserves the
spec invariant `maxDescriptorSet* >= maxPerStageDescriptor*`.

## Every PVR DRM modifier is FBCDC - the 18x lever is header-confirmed

`img_drm_fourcc.h` defines 30+ `DRM_FORMAT_MOD_PVR_*` constants and **every one is FBCDC**
(`DRM_FORMAT_MOD_PVR_FBCDC_8x8_V1` ... `_LOSSY75_16x4_V14`). There is no plain-tiled PVR modifier.

So the zero-copy flip path (the ~18x windowed factor) requires FBCDC, which requires FBD allocation,
which the mainline UAPI does not expose - `DRM_IOCTL_PVR_*` has 14 entries and none is an FBD
allocation. That is a from-scratch kernel + firmware job, not a driver tweak.

## Consequence for the sync work

The remaining tractable item is the `vk_sync` churn: ~250 ioctls per Xwayland frame, of which ~80% is
the queue's per-job create/destroy and ~20% the null path's temporary syncobjs. Removing it means
timeline syncobjs end-to-end, which requires value plumbing through `pvr_winsys.h`, both winsyses and
the three arch job wrappers (~10 files), because the winsys currently hardcodes `.value = 0` at 10
sites and asserts against `VK_SYNC_IS_TIMELINE` at 8.

**Measured payoff: 37 -> 45-62 FPS on the X11 path (1.2-1.7x).** That is against a 32x gap whose
dominant term (the copy/flip, ~18x) is blocked on FBCDC. So the sync change is worth doing as
correctness-of-architecture work, but it is not the lever that closes the gap, and it should not be
rushed into a 10-file refactor with a silent-corruption failure mode.

---

# Addendum: the shaderFloat16 record corrected, and a measurement trap

## The correction

`shaderFloat16 = false` was kept all session on the stated grounds that enabling it rendered
**20 of 27 glmark2 scenes wrong**. Re-measured:

```
glmark2-es2 --validate:
  shaderFloat16 = false   27 success / 0 failure
  shaderFloat16 = true    27 success / 0 failure
```

**The correctness failures do not reproduce.** The withdrawable claim is withdrawn; the flag stays
false on a different, still-valid ground:

```
MESA_SHADER_CACHE_DISABLE=true, glmark2 -b build:use-vbo=false, 3 runs:
  shaderFloat16 = false   45 47 43  (median 45)
  shaderFloat16 = true    39 38 36  (median 38)
```

**18% faster with it disabled.** So it is a performance decision, not a correctness one.

## The trap that hid this for an hour

With the shader cache **enabled**, both settings gave an **identical glmark2 Score of 35** and
identical per-scene FPS - because Mesa's `~/.cache/mesa_shader_cache` (2.1 MB here) served the same
compiled shaders regardless of the advertised feature. The A/B looked like "no effect" when the
real effect is 18%.

**Rule: any A/B that toggles a shader-compiler or shader-feature setting must set
`MESA_SHADER_CACHE_DISABLE=true`, or the cache will silently answer the experiment with the previous
configuration's binaries.**

## What it actually is

Advertised `shaderFloat16` makes zink implement GLES `mediump` with fp16, so the difference measures
**PCO's fp16 emit against its fp32 emit** - and fp16 is the slower one. That is a compiler-quality
finding: PCO's fp16 path costs 18% more than fp32 for the same scenes. Fixing the emit would regain
the capability (the vendor has it: 54 features on vs our 50) without the performance cost.

## Feature coverage gap (vendor vs open, measured)

```
vendor 54 features on, open 50.
VENDOR HAS, OPEN LACKS (8):
  core.occlusionQueryPrecise          vk11.variablePointers
  vk11.variablePointersStorageBuffer  vk12.bufferDeviceAddressCaptureReplay
  vk12.drawIndirectCount              vk12.shaderFloat16
  vk12.vulkanMemoryModel              vk12.vulkanMemoryModelDeviceScope
OPEN HAS, VENDOR LACKS (4):
  vk12.shaderInputAttachmentArrayDynamicIndexing
  vk12.shaderStorageTexelBufferArrayDynamicIndexing
  vk12.shaderUniformTexelBufferArrayDynamicIndexing
  vk13.descriptorBindingInlineUniformBlockUpdateAfterBind
```

All eight are genuinely unimplemented (no `CmdDrawIndirectCount`, no variablePointers support); they
are hardcoded `false`. `occlusionQueryPrecise` is the one where the feature exists but is
conservative - and the vendor reports it precise, so the hardware can do it.
