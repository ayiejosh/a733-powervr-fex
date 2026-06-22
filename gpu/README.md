# GPU on A733 / PowerVR BXM-4-64 (trixie / 6.6)

The PowerVR GPU is **usable per-workload** on the trixie stack — for **Direct3D**,
**off-screen OpenGL**, and Vulkan compute/render — but it **cannot drive a
GPU-composited desktop** (a live compositor on `pvrsrvkm` deadlocks the kernel). Pick the
path for your workload:

| Want | Use | Doc |
|---|---|---|
| **GPU Direct3D 9/10/11** (windowed or headless) | `d3drun` (DXVK-Sarek -> PowerVR Vulkan) | [`dxvk/`](dxvk/) |
| **GPU OpenGL, off-screen / EGL** | `glrun` (zink -> PowerVR Vulkan) | [`zink-trixie.md`](zink-trixie.md) |
| **GPU Vulkan compute / render** | native `libVK_IMG` ICD (off-screen) | `../bench/` |
| A **GPU desktop** | — not possible — | [`../kernel/`](../kernel/), [`../docs/FINDINGS.md`](../docs/FINDINGS.md) |

## 1. Direct3D (the headline new capability) -> [`dxvk/`](dxvk/)

GPU-accelerated **D3D9/10/11 (FL 11_0)** via a native arm64ec **DXVK-Sarek** build over
the closed PowerVR Vulkan blob: instancing, compute, render-to-texture, **BC1-5** textures
(in-driver decode), depth/MRT, windowed present. Geometry shaders are compute-emulated
(slow, gated off); D3D12 is infeasible. Full recipe, capability matrix, and the critical
`DXVK_HUD` warning in [`dxvk/README.md`](dxvk/README.md).

## 2. OpenGL via zink (off-screen only) -> [`zink-trixie.md`](zink-trixie.md)

GPU OpenGL via **zink** on the **system Mesa 25.0.7** + the PowerVR Vulkan ICD + the
feature-strip layer. **glmark2-es2 `--off-screen` = 661.** GLES2/GL2.1 class; **windowed
and desktop GL do NOT work**. Note the **dual-Mesa conflict** (system 25.0.7 vs
`/usr/local` IMG 24.0.1) — `glrun` scopes the env around it. Details in
[`zink-trixie.md`](zink-trixie.md).

## 3. The kernel wall: no GPU-composited desktop

A live compositor driving the PowerVR GPU to scan out HDMI **deadlocks the kernel**
(`pvrsrvkm` mutex spin-on-owner in IRQ -> power-cycle). The off-screen kmsro/renderonly
bridge on `card0` is *proven* (a `SCANOUT|RENDER` gbm buffer + zink/PowerVR succeeds), so
the wall is the closed `pvrsrvkm`, not Mesa. The desktop stays software-rendered X11. See
[`../kernel/`](../kernel/) and [`../docs/FINDINGS.md`](../docs/FINDINGS.md).

> The `sway/` configs (a GPU-composited Wayland desktop, from the bullseye line) are kept
> for reference, but on this stack any **live** GPU compositor hits the kernel deadlock
> above — they are not a working desktop here. Off-screen GPU render is unaffected.

## Kernel patch

The `pvrsrvkm` DRM PRIME-import patch (needed for zink/kmsro buffer sharing) is in
[`../kernel/`](../kernel/) and still applies on 6.6.
