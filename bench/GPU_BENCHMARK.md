# Radxa Cubie A7A (Allwinner A733) — PowerVR GPU benchmark & analysis

**Date:** 2026-06-05
**GPU:** Imagination PowerVR **B-Series BXM-4-64 MC1** (BVNC 36.56.104.183), 1 core
**Driver:** Imagination DDK **24.2@6603887** — kernel `pvrsrvkm` (img-bxm-dkms) + userspace in `/usr/local/lib`
**APIs proven working:** **Vulkan 1.3.277**, **OpenGL ES 3.2**, OpenCL 3.0 (libPVROCL)
**GPU clock:** 600 MHz fixed (DTS sets no DVFS table — see Optimization)

## TL;DR
The GPU was never missing — it was just never wired into the GL stack (the desktop falls back to a software rasterizer). Measured, the GPU does **real GLES 3.2 rendering ~150–175× faster than the CPU's absolute best case** (all 8 cores, NEON, pure math, no rasterization overhead), and **~600× faster than the software rasterizer the desktop actually falls back to (softpipe)**.

## Method
- `glbench.c` — headless EGL + GLES2, renders a fullscreen triangle with an ALU-loop fragment shader into a 1280×720 FBO, N frames, reports frames/s and effective **Mpix/s**. GPU path: `LD_LIBRARY_PATH=/usr/local/lib` + GBM on `/dev/dri/renderD128`.
- `cpubench.c` — the *identical* per-pixel shader math, OpenMP across all 8 cores, `-O3 -ffast-math -ftree-vectorize` (NEON). A **generous upper bound** for software rendering (no raster/framebuffer overhead).
- `vkprobe.c` — direct-ICD Vulkan enumeration (bypasses the too-old bullseye loader).
- Shader "load" = fragment-shader loop iterations (proxy for compositing→heavy-effects). Higher = more ALU per pixel.

## Results — GPU vs CPU best-case (1280×720)

| Shader load | GPU PowerVR (Mpix/s) | CPU 8-core NEON best-case (Mpix/s) | **GPU advantage** | regime |
|---|---|---|---|---|
| loop=4   | **4198** (4555 fps) | 28  | **~150×** | fill-rate bound (~4.2 Gpix/s ceiling) |
| loop=16  | **1216** (1319 fps) | 7   | **~174×** | transition |
| loop=64  | **315**  (342 fps)  | 2   | **~158×** | ALU bound |
| loop=256 | **80**   (87 fps)   | 0.46| **~174×** | heavy ALU bound |

**Reference (the desktop's actual fallback):** Mesa **softpipe** measured **0.5 Mpix/s @ loop=64** → GPU is **~600×** vs the path in use today. (llvmpipe, the *optimized* software rasterizer, could not be measured — system Mesa 20.3.5's headless EGL is broken on this box and the DDK Mesa ships no llvmpipe — but it would land near the CPU best-case ≈ 2 Mpix/s, i.e. GPU still **~150×**.)

## How it performs / scaling
- **Fill-rate ceiling ≈ 4.2 Gpixels/s** at light load — comfortably saturates 1080p60 compositing (~125 Mpix/s) with ~30× headroom.
- **Clean linear ALU scaling**: 4× the shader work ≈ 4× slower (4198→1216→315→80), i.e. the GPU is doing real parallel shading, not falling over. No driver bottleneck visible.
- The **GPU advantage is constant (~150–175×)** across all loads — it's a flat horsepower multiplier over the CPU, exactly what you'd expect from a real GPU vs SIMD CPU.

## Can it be optimized further?
- **Driver:** the numbers are from Imagination's own proprietary DDK (their optimized driver) — there is little driver-side headroom; this is close to the hardware's real throughput.
- **Clock:** GPU runs at a **fixed 600 MHz** (kernel log: *"default clk_rate is NOT set in DTS, set it to default:600000000"*). No DVFS/boost table is configured. If the BXM-4-64 can clock higher, a DTS/clk change could add headroom — needs verification against the chip's rated GPU clock (do NOT assume; risk of instability).
- **The real "optimization" is wiring, not tuning:** today nothing uses the GPU. The win is moving work *off* the 8 CPU cores (currently burning on software GL) — which simultaneously frees CPU for the VE2 encoder and app logic.
- **API choice:** native GLES (tested) is optimal; a Zink (GL-on-Vulkan) path would add a translation layer — only worth it for apps that need newer GL than the DDK exposes.

## What this unlocks (with the numbers behind it)
1. **Desktop/compositing/Chromium** — currently ~0.5 Mpix/s (softpipe) → ~315–4198 Mpix/s. The choppy, CPU-pegged software desktop becomes GPU-trivial.
2. **Frees the CPU** — the 8 cores stop rendering pixels, leaving them for the VE2 H.264 encode + app logic. Directly attacks the Sunshine choppiness (which was CPU contention, not the encoder).
3. **Sunshine GL zero-copy capture→encode** — the original `EGL_BAD_ALLOC` was llvmpipe, not the real GPU; with PowerVR EGL the GL→VAAPI-surface import path may finally work.
4. **box64 + DXVK/VKD3D (Proton) gaming** — on a real Vulkan 1.3 GPU.

## Caveats / honesty
- Numbers are **offscreen FBO shader throughput (Mpix/s)** — a clean GPU-vs-CPU "cost to shade pixels" metric, **not** full-application fps (real apps add draw-call/state/upload overhead).
- The CPU baseline is a **best-case** (pure math, no rasterization), so the **real-world GPU gain is ≥ the stated ~150×**.
- GPU GLES currently requires `LD_LIBRARY_PATH=/usr/local/lib` + a DRM render node; it is **not yet wired** into the desktop/EGL vendor (only llvmpipe/softpipe is registered). That wiring is the next step.

## Repro
```
# GPU:   LD_LIBRARY_PATH=/usr/local/lib /tmp/glbench /dev/dri/renderD128 <loop> <frames>
# CPU:   /tmp/cpubench <loop> <frames>
# Vulkan: XDG_RUNTIME_DIR=/run/user/0 /tmp/vkprobe   (direct IMG ICD)
# sources: /home/radxa/ve2-vaapi/{glbench,cpubench,vkprobe}.c
```
