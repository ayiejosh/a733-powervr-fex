# A733 / PowerVR BXM-4-64 — capability matrix & walls

Hardware: Allwinner A733 (`sun60iw2`), GPU **Imagination PowerVR B-Series
BXM-4-64 MC1** (BVNC `36.56.104.183`), Radxa Debian 11 BSP, kernel
`5.15.147-21-a733`, Imagination DDK `24.2@6603887` (kernel `pvrsrvkm` +
`img-bxm-dkms`). Vulkan ICD `libVK_IMG`.

The short version: **the GPU works great per-workload; it cannot be the system
default renderer or drive a desktop-GL environment.** Details below.

## ✅ Proven working

**GPU compute / graphics**
- **Vulkan 1.3** — vendor ICD (`libVK_IMG`, `img_icd.json`) enumerates
  `PowerVR B-Series BXM-4-64 MC1`, INTEGRATED_GPU, ~5.9 GB device-local. Works
  via direct ICD probe; the bullseye system loader (1.2.162) is too old for a
  1.3 ICD, so use a newer loader or load the ICD directly.
- **OpenGL via Zink-on-Vulkan** — Mesa 25.3 `zink` on the vendor Vulkan ICD →
  `zink (PowerVR B-Series BXM-4-64 MC1)`. Needs one Mesa patch (see `gpu/`).
- **Native GLES 3.2** at full GPU speed — offscreen (GBM render node) **and** on
  a native glamor X11 server. Offscreen FBO shader throughput ~1.2 Gpix/s at a
  64-deep ALU loop (≈150–600× the CPU/llvmpipe path).
- **OpenCL** — `libPVROCL` present.

**Kernel / compositor**
- **DRM PRIME import** — the vendor `pvrsrvkm` shipped without
  `prime_fd_to_handle` / `gem_prime_import` (ENOSYS), which blocked zink/wlroots
  buffer sharing. The patch in `kernel/` implements it; foreign dma-buf import
  (e.g. from `/dev/dma_heap/system`) is proven GPU-renderable.
- **GPU-composited Wayland desktop** — `sway` (wlroots, vendor GLES2) composites
  on the GPU; captured by `wayvnc` → noVNC for a browser desktop. This is the one
  reliably-capturable GPU desktop. Recipe in `gpu/sway/`.

**Media**
- **H.264 hardware encode** works (VE2). HW decode H.265/VP9/AVS2 per datasheet.

## 🧱 Walls (with the reason)

**Hard — driver/silicon class, not fixable on the shipped vendor stack**

- **KDE Plasma / KWin GPU = impossible.** KWin needs desktop OpenGL
  (GL ≥ 3 / GLSL > 1.20). Reachable *display* GL here is **GLES2-class**:
  - native vendor GLES 3.2 only works **offscreen** (GBM) — no window-system
    path (vendor Mesa built `xorg_release`/X11-only; even X11 DRI3 resolves the
    driver name to `zink`, not the native PowerVR driver);
  - **Zink** gives a display path but is capped at **GL 2.1 / GLSL 1.20** because
    the mobile Vulkan lacks features Zink needs for higher GL (geometryShader,
    fillModeNonSolid, …).
  Result: KWin can't create a GL context (X11) / has no client EGL (Wayland) and
  falls back to software (XRender / QtQuick-software). Tested many ways.
- **No transparent / default EGL→GPU.** GLVND only ships the Mesa (llvmpipe) EGL
  vendor; the vendor EGL is a **closed, X11-only Mesa fork** with a private
  `__DRI` ABI. There's no clean drop-in PowerVR EGL vendor to register
  (reproducing one from mainline = re-forging Imagination's private patches —
  abandoned). **Default stays llvmpipe; GPU is opt-in per app.**
- **Native Wayland GL *clients* crash** — zink's `kopper` WSI present path
  SIGSEGVs on this Vulkan ICD, and the vendor Mesa has no Wayland EGL platform.
  So `sway` *composites* on GPU, but GPU Wayland client apps don't present.
- **HEVC hardware *encode*** — dead end (no IDR; vendor lib limit). H.264 encode
  is the ceiling.
- **DDK can't be upgraded** — `24.2@6603887` is identical across the Radxa apt
  repo, BSP v1.4.8, the 6.6 BSP, and every community build.

**Soft — environmental / version, not silicon**

- **Native X11 GPU desktop can't be screen-scraped** — glamor GPU→CPU readback
  hangs `x11vnc`; `AccelMethod none` hangs the X server. Only `sway` + `wayvnc`
  (wlroots screencopy) captures cleanly.
- **System Vulkan loader too old** (1.2.162) for the 1.3 ICD.

## 🔮 The only lever that lifts the hard walls

Mainline **`drm/imagination` + Mesa `pvr`** (clean open Vulkan → clean EGL; Zink
could then expose GL ≥ 3; transparent wiring; Wayland clients; potentially
KDE-GPU). The BXM family is covered by that driver, but **A733 mainline is at the
bare-DTS upstreaming stage** (first DTS series posted mid-2026 — CPUs/MMC/UART/SD,
no GPU/ethernet yet). Months+ away, but it's the only path that raises the ceiling.

## x86 emulation (FEX / box64) — see `fex/`, `box64/`

- **FEX** runs x86/x86-64; a **custom Vulkan thunk** forwards x86 Vulkan to the
  native ARM PowerVR GPU (compute + WSI verified). Chrome (x86) runs and paints.
- **box64** (upstream, dynarec) runs x86-64 userspace.
- **Not viable here:** DirectX/DXVK gaming (PowerVR is missing DXVK-required
  Vulkan extensions) and Steam's CEF UI under FEX (bwrap/pressure-vessel blocker).
  Documented as findings, not as working features.
