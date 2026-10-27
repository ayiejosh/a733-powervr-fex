# Vendor parity status (2026-10-26, end of goal round 4)

Same board, same workloads. "Parity" = open stack within run-to-run noise of the vendor stack.

## 1. Vulkan: open driver vs vendor Vulkan - PARITY (near)

A/B matrix on open HEAD `c737360`, 3 alternating rounds, medians, **open / vendor** (`~/open-perf/ovov-1012.txt`):

| test | open | vendor | open/vendor |
|---|---|---|---|
| r512 (ms) | 0.362 | 0.359 | 1.01x |
| r2048 (ms) | 1.708 | 1.927 | **0.89x (-11%)** |
| b512 (ms) | 0.644 | 0.607 | **1.06x (+6%)** |
| b2048 (ms) | 5.341 | 5.335 | 1.00x |
| c512 (ms) | 0.237 | 0.244 | 0.97x |
| c2048 (ms) | 2.546 | 2.546 | 1.00x |
| heavy (fps) | 171.2 | 179.8 | **0.95x (-4.8%)** |
| cstpf (fps) | 148.2 | 146.6 | 1.01x |
| cstpi (fps) | 151.5 | 147.9 | 1.02x |
| cstp (fps) | 445.4 | 372.7 | **1.20x (+19.5%)** |
| many512 (ms) | 0.807 | 0.804 | 1.00x |
| many2048 (ms) | 5.765 | 5.687 | 1.01x |

Verdict: the open Vulkan driver is at parity with the vendor Vulkan driver overall (three scenes outside noise:
r2048 -11% host-side, heavy -4.8%, cstp +19.5% faster).

## 2b. OpenGL, fresh same-session benchmark (2026-10-27): Wayland frontend is functional, open stack ahead

glmark2 2023.01, GLES2, default scene set, 800x600, same board; logs in captures/bench-2026-10-27/:
- vendor native GL, X11 desktop: **511, 533** (median 522)
- open stack: zink -> open Vulkan, headless weston + Wayland frontend: **543, 570, 546** (median 546)
  (GL_RENDERER: zink Vulkan 1.3(PowerVR B-Series BXM-4-64 MC1 (IMAGINATION_OPEN_SOURCE_MESA)))
i.e. the open stack's GL frontend is functional and is ~5-10% ahead of vendor native GL on this benchmark.
At 1280x720 the open stack scores **429** vs vendor native GL **359** (~1.19x ahead); at 1920x1080 it scores
**265** vs **202** (~1.31x ahead); at 3840x2160 it scores **84** vs **53** (~1.58x ahead). The open stack's
advantage grows with resolution, which suggests the vendor GL stack scales worse (its per-frame overhead at
high resolutions) rather than a zink frontend advantage at large pixel counts. Raw logs for all resolutions
are in captures/bench-2026-10-27/.
Caveats: different window systems (vendor is X11 with the KDE desktop running; open is headless weston +
Wayland), single scene set, 800x600. The earlier recorded numbers (open 289-309 vs vendor 720) came from a
different session and measured the open stack through the X11/Xwayland path, which on the open DRM node
currently dies on `DRM_IOCTL_MODE_CREATE_DUMB: Permission denied` before rendering. Reproduce the working
path with `bench/glmark2-wayland-open.sh OUT`.

Control measurement of "zink -> vendor Vulkan" under the same frontend was not obtainable on this image:
weston's GL renderer initialises against system Mesa ("EGL vendor: Mesa Project") and the client then fails
`eglInitialize() ... 0x3001` (EGL_NOT_INITIALIZED) both on Wayland and X11, because the built Mesa's EGL/DRI
path cannot drive the vendor kernel driver; the older 293 figure for zink->vendor Vulkan came from a
different environment and is not comparable to the fresh numbers above.

## 2. OpenGL: zink on open Vulkan vs vendor native GL (earlier session recording, Xwayland path)

glmark2 scores (same board): vendor native GL **720**, zink -> vendor Vulkan **293**, zink -> open Vulkan
**289-309**. That is **0.40-0.43x** of vendor. The gap is the zink GL->Vulkan frontend (CPU-bound), not the
driver: open and vendor Vulkan are at parity, and zink on vendor Vulkan measures the same 293.

## 3. Native GL driver (`src/gallium/drivers/pvr_native`) - working draw path, no GL frontend yet

- Draws execute on the GPU through the real `pipe_context` API via the compute fallback (pixel-exact
  readback), benchmarked with `PVR_PROBE_COMPUTE_BENCH=1` (200 frames, synchronous round trip).
  After the 4x2-pixel-block rasteriser (5.7-7.5x faster than the original 1-thread-per-pixel version,
  because the per-thread parameter reads were the dominant cost):
  **512x512 triangle: 1.42 ms/frame, 702 fps, 184 Mpix/s**;
  256x256: 0.48 ms/frame; 1366x768 full-screen: 5.30 ms/frame, 189 fps, 198 Mpix/s;
  1920x1080 full-screen: 10.04 ms/frame, 100 fps, 207 Mpix/s; covered pixel counts verified exactly.
  (Previous 1-thread-per-pixel numbers: 10.2 ms / 40.0 ms / 75.2 ms for the same cases.)
  For scale, the vendor Vulkan stack runs its equivalent 512x512 triangle scene at ~0.36 ms/frame, so the
  native fallback draw is now ~4x that instead of ~28x. An asynchronous-submit path
  (`PVR_COMPUTE_ASYNC`, batching completion syncobjs, flushed by `pipe_context::flush`) exists but shows
  no gain at these sizes - the path is GPU-bound, not wait-bound.
- The fixed-function 3D/TA path is **blocked by a hardware DM2 stall** that only affects this client (see
  HANDOFF.md / TA-KICK-DISASSEMBLY.md): firmware-visible state is identical to the working Vulkan client,
  yet the TA DM is never started by the hardware. Until that is cracked (needs vendor/IMG-level
  observability), the driver cannot run real GL scenes, so there is no glmark2 number for it to compare.

## Bottom line

- Vulkan: parity with vendor.
- OpenGL through zink: ~2.5x slower than vendor native GL - the remaining gap.
- Native GL is the intended fix for that gap, but its 3D hardware path is stalled by the DM2 issue; its
  compute-fallback draw path is benchmarked and correct.
