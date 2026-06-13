# x86 on A733 via FEX — incl. a Vulkan GPU thunk

[FEX-Emu](https://github.com/FEX-Emu/FEX) runs x86/x86-64 binaries on this ARM64
board. This dir has the **custom Vulkan thunk** (x86 Vulkan → native PowerVR GPU),
plus the setup/launcher scripts used here.

> Bring your own x86 rootfs and apps — none are bundled. The scripts build/use a
> rootfs you create; no proprietary binaries are in this repo.

## The novel bit: a Vulkan GPU thunk

By default a Vulkan app inside FEX would hit a *software* x86 Vulkan (or nothing).
The thunk forwards Vulkan from the x86 guest to the **native ARM `libVK_IMG`**, so
x86 apps render on the real PowerVR GPU. Compute dispatch + WSI forwarding are
verified working (identical results to native).

- `vkthunk_render.c` — standalone render test/reference exercising the thunked
  path (GBM render node → import → GPU fill → readback).
- `ThunkConfig.json`, `ve2-thunks.json` — FEX thunk wiring (maps the guest Vulkan
  lib to the host thunk).

Build the host/guest thunk libraries from a FEX checkout's `ThunkLibs` (the custom
`libVK_IMG`/`libEGL`/`libdrm` thunks), install the guest stubs into your rootfs,
and point `~/.fex-emu/Config.json` at the thunk config. (See FEX's thunk docs;
this is the same mechanism FEX uses upstream for host GL/Vulkan, retargeted at the
vendor PowerVR ICD.)

## Setup / launcher scripts

| Script | What it does |
|--------|--------------|
| `complete-fex-env.sh` | Brings an x86 rootfs to app-readiness (i386 multiarch, codecs, X-session libs, audio socket). |
| `fex-install` | Install an x86 `.deb` into the rootfs and create a normal-looking host launcher (de-branded, real menu category). arm64/all `.deb`s go to native apt instead. |
| `fexrun` | Run an x86 command under FEX with the right env. |
| `glx-run` | Run an x86 GL app via a nested Xephyr (software/llvmpipe GLX) — the `:0` PowerVR Xorg has no desktop GLX. |
| `chrome-fex-gpu.sh` | Launch x86 Chrome under FEX (stable; flags tuned: `--in-process-gpu`, long IPC timeout, stripped flags). |
| `steam-fex` | Launch Steam under FEX (**WIP** — see below). |

## What works / what doesn't (be realistic)

- ✅ **Chrome (x86) runs and paints** under FEX — stable.
- ✅ **x86 Vulkan → native PowerVR GPU** via the thunk (compute + WSI verified).
- ⚠️ **Steam** — client renders, but the CEF `steamwebhelper` loops on a
  bwrap/pressure-vessel mkdir failure under FEX. Not solved.
- ❌ **DirectX / DXVK / Proton gaming** — not viable: the PowerVR Vulkan is missing
  DXVK-required extensions. Native-Vulkan Linux games are the only realistic
  GPU-gaming path, and those are slow under x86 JIT.
- ⚠️ Cold start is slow (FEX JIT compile-bound); an AOT/code cache helps.

See `docs/FINDINGS.md` for the GPU-side reasons behind the DX/DXVK wall.
