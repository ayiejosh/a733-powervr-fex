# open-driver-perf — current results (2026-10-27)

Board: Radxa Cubie A7A, PowerVR BXM-4-64 (BVNC 36.56.104.183), kernel 6.6.98. Stacks: vendor DDK 24.2
(pvrsrvkm + vendor GL/Vulkan) and the open stack (mainline drm/imagination backport + Mesa `pvr` Vulkan /
zink / the project's own `pvr_native` gallium driver).

## 1. Native driver (`src/gallium/drivers/pvr_native`) — integrated draw path

- Real `pipe_context::draw_vbo` path: takes the bound vertex buffer's three clip-space vertices and the bound
  render-target size, builds the compute-fallback job (pco-compiled shader, PDS programs, CDM/firmware
  streams, heap allocations, compute context), submits through the driver's own VM, and returns after the
  completion fence. Pixel-exact on hardware and under the drm-shim.
- Async submit path exists (`PVR_COMPUTE_ASYNC`, completion syncobjs batched, flushed by
  `pipe_context::flush`).
- Rasteriser optimised to a 4x2 pixel block per invocation (7x over the naive 1-thread-per-pixel version).

Benchmark (`PVR_PROBE_COMPUTE_BENCH=1`, 200 frames, synchronous round trip):
| case | ms/frame | fps | Mpix/s |
|---|---|---|---|
| 512x512 triangle | **1.42** | 702 | 184 |
| 1366x768 full-screen | 5.30 | 189 | 198 |
| 1920x1080 full-screen | 10.04 | 100 | 207 |
All pixel counts exact; probe `--- ALL PASS` with and without the benchmark and under drm-shim.
Re-verified on 2026-10-27 after the register/trace experiments: full probe `--- ALL PASS`, benchmark
512x512 = 1.337 ms/frame (748 fps, 196 Mpix/s), 1366x768 = 5.77 ms, 1080p = 10.61 ms (run-to-run noise
vs the numbers above is a few percent).

QUC (2026-10-27 08:0x): native probe `--- ALL PASS`, bench 512x512 = 1.395 ms/frame (717 fps, 188 Mpix/s),
1080p = 10.35 ms (97 fps); open Vulkan render correctness PASS (262144/262144); open-stack glmark2
(800x600, Wayland) score 594 this run (range across runs 543-594; still ahead of vendor native GL 511-533).

## 2. Vendor parity

- Vulkan, open Mesa `pvr` vs vendor Vulkan (A/B matrix, medians): **parity** (r2048 0.89x, heavy 0.95x,
  cstp 1.20x, the rest within noise).
- OpenGL, fresh same-session glmark2 (default scene set): open stack through the Wayland frontend
  (zink -> open Vulkan) is *ahead* of vendor native GL: 546 vs 522 at 800x600, 265 vs 202 at 1920x1080.
  The X11/Xwayland route on the open stack is unusable (open DRM node has no dumb buffers).
  Reproduce: `bench/glmark2-wayland-open.sh OUT`; logs in `captures/bench-2026-10-27/`.
- The native driver's fixed-function 3D path is blocked (see 3), so it has no glmark2 number.

## 3. Blocked: DM2/TA hardware stall (needs IMG input)

The native client's geometry kick is processed identically by the firmware up to `Kick TA`, but the hardware
never raises the DM2 completion event `EVENT_STATUS=0x20`; firmware runs its DM2 progress/HCR checks and
reports the timeout that ends in `GPU has locked up`. The working open-Vulkan client on the same firmware
raises 0x20 and reaches `TA finished`.

Every in-board differential has been closed: firmware trace (full mask, enlarged ring), pre/post-kick
registers (0–64 KiB), cCCB/user/firmware streams, HWRT dataset arguments and contents, context creation
arguments, static context state, FWCtx and RTData contents. The remaining difference is inside the
hardware/DM2 start path and needs vendor/IMG firmware-side observability. The open firmware is the **official IMG open-ABI 36.56.104.183 build** (from Imagination's own
linux-firmware fork, branch `powervr`, commit `8a58f818`; sha256 verified byte-identical). The newest
community open driver (`apercel`: upstream drm/imagination + 30 backported fixes) was also tested and shows the
same stall, so both firmware image and driver version are ruled out. A re-scoped request for DM2/TA
firmware-side observability is in
`docs/IMG-FIRMWARE-REQUEST-36.56.104.183.md`. `tools/firmware/check-open-firmware.sh` watches for a genuine
release.

## 4. Board safety

All GPU experiments are done by switching stacks with `open-perf/open-enter.sh` / `open-exit.sh` and return
to vendor + desktop; the open firmware file is restored to its original bytes (`sha256 1db1c399…`) after
any temporary firmware variant. Kernel source worktree ends clean.
