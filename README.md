# a733-powervr-toolkit

Patches, recipes, and hard-won findings for getting the **PowerVR BXM-4-64 GPU**
and **x86 emulation** working on the **Allwinner A733** (`sun60iw2`) — as shipped on
the Radxa Cubie A7A / A7S boards.

This is the result of an extended bring-up effort on a Cubie A7A running the Radxa
Debian 11 BSP (kernel `5.15.147-21-a733`, Imagination DDK `24.2@6603887`). It
documents what genuinely works, the recipes to reproduce it, and — just as
importantly — the walls that are **not** crossable on the shipped vendor stack and
why.

## Install

```sh
./install.sh          # guided: kernel patch + GPU sway desktop (prompts each step)
./install.sh kernel   # just the pvrsrvkm PRIME patch (dry-runs first)
./install.sh sway      # just the GPU sway+wayvnc desktop
```
(Vendor blobs + the Zink Mesa build are manual — see below.)

## What's here

| Dir | Contents |
|-----|----------|
| [`kernel/`](kernel/) | **`pvrsrvkm` DRM PRIME-import patch** — adds standard `gem_prime_import` / `prime_fd_to_handle` (which the vendor left unimplemented) so zink/wlroots can share buffers with the GPU. The single most useful patch here. |
| [`gpu/`](gpu/) | **Zink-on-Vulkan** GL recipe (incl. the one Mesa patch needed) + a **GPU-composited `sway` + `wayvnc`** Wayland desktop (configs + service files). |
| [`fex/`](fex/) | **Custom FEX Vulkan thunk** (x86 Vulkan → native PowerVR GPU) + FEX setup/launcher scripts + Chrome-on-FEX recipe. |
| [`box64/`](box64/) | Usage notes for box64 on A733 (links upstream; nothing forked). |
| [`docs/FINDINGS.md`](docs/FINDINGS.md) | **The capability matrix** — every proven-working path and every confirmed wall, with the *why*. Read this first if you're deciding what's worth attempting. |

## ⚠️ What you must supply yourself (not in this repo)

This repo contains **only** open / original work and patches. It deliberately does
**not** include, and cannot legally redistribute:

- **The proprietary PowerVR userspace blobs** — `libGLESv2_PVR_MESA`, `libVK_IMG`,
  `libEGL`, and the `rgx.fw.*` firmware. Get them from the vendor channel
  (`radxa/allwinner-target`, branch `target-a733-v1.4.x`) and the `img-bxm-dkms`
  package in the Radxa apt repo.
- **The `img-bxm-dkms` kernel source** — install it from the Radxa apt repo
  (`a733-bullseye`); the kernel patch here applies on top of it.
- **Any FEX x86 rootfs** or **proprietary apps** (Steam/Proton/etc.). The FEX
  scripts build/use a rootfs you create; no rootfs or third-party binaries are
  bundled.

## Honest summary

The GPU is **fully usable per-workload** — Vulkan, GL via Zink, native GLES on X11,
a GLES2-class `sway` desktop, H.264 hardware encode. It **cannot** be the *default*
renderer or drive a desktop-GL environment like KDE Plasma; that ceiling is the
closed vendor stack and is only liftable by mainline (`drm/imagination` + Mesa
`pvr`), which for A733 is still at the bare-DTS upstreaming stage. See
[`docs/FINDINGS.md`](docs/FINDINGS.md) for the full matrix.

## License

Original code/patches/scripts here are MIT (see `LICENSE`). They are intended to be
applied on top of vendor/upstream sources that carry their own licenses; obtain
those from their respective sources.
