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
