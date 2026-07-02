# GPU-accelerated Direct3D 9/10/11 on PowerVR — DXVK-Sarek (trixie / 6.6)

The headline new capability of the trixie branch: **GPU-accelerated Direct3D 9/10/11
(feature level 11_0)** on the PowerVR BXM-4-64, via a **native arm64ec DXVK-Sarek** build
that targets the closed PowerVR Vulkan blob. On the bullseye branch DirectX/DXVK was a
documented dead-end; here it **works** for a wide D3D11 surface — verified rendering, not
just demoable, and without wedging the board.

> Stack: `D3D11 -> DXVK-Sarek (arm64ec) -> wine/winevulkan -> PowerVR BXM-4-64 Vulkan
> blob`. Wine runs under Hangover (FEX/box64 WoW64). The arm64ec DXVK build is the key —
> it runs DXVK as native ARM code, not emulated x86.

> **2026-07-02:** shipping build rebased onto upstream DXVK-Sarek `2c80828f`
> (+4,625 commits: UMA heap-budget fix, `lowerSinCos` crash fixes, config sync) with the
> BC1-5 patch as commit `25dce6a8`. All capability tests + BC1-5 decode re-verified,
> perf unchanged. Note `dxvk.tilerMode` is a no-op on this stack (see docs/FINDINGS.md).
> The dll SET ships together: d3d11 + dxgi + d3d10core (dxgi carries the BC format table).

## The recipe

1. **DXVK-Sarek, built arm64ec.** DXVK-Sarek is the lower-feature-level DXVK fork that
   fits a mobile Vulkan; build it with the arm64ec mingw toolchain
   (`x86_64-w64-mingw32-clang++` from an llvm-mingw arm64ec build), `ninja -C build-ec`.
   The shipping `d3d11.dll` is the **BCn** build (in-driver BC1-5 decode, below).
2. **Software-GL window via llvmpipe.** The native PowerVR **GL** blob deadlocks wine's
   graphics init, so the *window* (swapchain present surface) is created with a software
   llvmpipe GL context. The D3D **render** still runs on the real GPU via Vulkan; only the
   final backbuffer blit to the X11 window is CPU software (~1.9 ms/frame).
3. **PowerVR Vulkan via `img_icd`.** Point winevulkan at the closed ICD
   (`/usr/share/vulkan/icd.d/img_icd.json` -> `libVK_IMG.so`).
4. **The feature-strip Vulkan layer.** The blob under-reports / lies about features; a
   small implicit Vulkan layer adjusts the advertised feature set so DXVK proceeds and
   then strips features the blob can't actually honor at `CreateDevice`. (Same layer used
   by the zink path — see `../zink-trixie.md`.)
5. **`dxvk.conf`.** Ship with:
   ```
   dxvk.enableGraphicsPipelineLibrary = False
   dxvk.dyasync = False
   ```
   Leave the rest at defaults. Do **not** mutate it casually — the shipping conf is what
   the verified runs used.

A `d3drun <app.exe>` launcher wires all of this (prefix `~/.wine-dxvk`, the right env,
`DXVK_CONFIG_FILE`). Run a D3D11 app with `d3drun app.exe`.

## What works (capability matrix)

| Feature | Status | Note |
|---|---|---|
| D3D11 feature level **11_0** | ✅ | `FL=0xb000`; D3D9/10 via DXVK too |
| Instancing | ✅ | ~370k tris/s |
| Compute / GPGPU | ✅ | |
| Render-to-texture, depth/Z, MRT | ✅ | |
| Textures incl. **BC1-5** | ✅ | decoded **in-driver** (a patch added to this DXVK-Sarek build) |
| Windowed present | ✅ | software-llvmpipe window, ~227 fps trivial scene |
| Geometry shaders | ⚠️ emulated | compute-based, ~80x slow, gated OFF (below) |
| Tessellation / MSAA (native) | ❌ | the blob lacks the Vulkan features |
| **D3D12** (vkd3d-proton) | ❌ | infeasible — needs Vulkan features the blob lacks |

Verified end-to-end (headless RTT + windowed): `tri.exe` (D3D11 triangle, 6000 frames),
`cube.exe` (textured cube + depth + **BC1**, the most app-like test) — both `RC=0`, no
wedge. Performance characterization (draw-call ceiling, GPU-fill-bound real frames,
present cost) is in `../../docs/BENCHMARKS.md`.

## BC1-5 in-driver texture decode

The PowerVR blob does not expose `textureCompressionBC`. Rather than fail BC-compressed
content, this DXVK-Sarek build **decodes BC1-5 in-driver** so D3D apps that ship
BC-compressed textures (most of them) load and render correctly (`cube.exe`'s BC1 cube
texture creates with `hr=0x0` and renders). This is the difference between "most real D3D
content runs" and "BC textures fail".

## Geometry-shader compute-emulation (branch `gs-compute`)

The blob has no usable geometry-shader support (it advertises `geometryShader=1` but GS
*pipelines* crash). A **compute-based GS emulation** (libpoly-style) is implemented on
branch `gs-compute` and **proven to render**:

- **Architecture (proven):** GS DXBC -> compute SPIR-V codegen (in DXVK's own DXBC->SPIR-V
  emitter, not a post-pass), plus a **3-pass runtime driver**: (1) capture the VS outputs
  into an SSBO, (2) dispatch the compute-GS (atomic-append emitted vertices + a counter),
  (3) copy the counter into an indirect-draw arg and draw the generated vertices with a
  DXVK-native passthrough VS. The probe `gs.exe` renders the GS-tinted output
  (`GS_OK` / green / `RC=0`), reproducibly, on a clean build.
- **Penalty: ~80x** slower than a native draw (measured: ~1179 us/draw emulated vs
  ~14.7 us baseline). Each emulated GS draw replaces one native draw with a serialized
  copy + 2 compute dispatches + a counter->indirect chain; auto-barriers serialize the
  stages, so it's per-draw dispatch/submit overhead, not GS math. **Compatibility-grade
  only.**
- **Gated OFF** behind `dxvk.conf d3d11.emulateGeometryShaders` (default false). The
  shipping BCn `d3d11.dll` is unaffected.
- **Known gap:** Pass-1 is currently a probe-specific capture shortcut; a **general
  VS-as-compute** lowering (format-aware vertex-buffer input gather for arbitrary app
  vertex shaders) is scoped but **not implemented**. The XFB route is dead on this blob
  (`transformFeedback=0`), and `vertexPipelineStoresAndAtomics=0` is why the passthrough
  VS reads its data as a **vertex buffer**, not a VS-stage SSBO. So GS emulation works for
  topology/identity-class probes today; arbitrary-VS apps need that remaining milestone.

## ⚠️ Critical warnings

- **NEVER set `DXVK_HUD`.** It engages the GPU on the display/overlay path and **wedges
  the GPU** (the same live-compositor `pvrsrvkm` kernel-deadlock class — see
  `../../kernel/`). There is no safe HUD on this board.
- **D3D12 is infeasible.** vkd3d-proton needs Vulkan features the blob doesn't have.
- The windowed **present** is software (llvmpipe) because the PowerVR GL blob deadlocks
  wine graphics init — the *render* is GPU, the window blit is CPU. For maximum
  throughput, render offscreen and avoid per-frame window present.
- Keep `dxvk.conf` as shipped (`enableGraphicsPipelineLibrary=False`, `dyasync=False`);
  these were the verified-stable settings.
