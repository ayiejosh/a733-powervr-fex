# a733-powervr-fex

Patches, recipes, and hard-won findings for getting the **PowerVR BXM-4-64 GPU**
and **x86 emulation** working on the **Allwinner A733** (`sun60iw2`) — as shipped on
the Radxa Cubie A7A / A7S boards.

This is the result of an extended bring-up effort on a Cubie A7A running the Radxa
Debian 11 BSP (kernel `5.15.147-21-a733`, Imagination DDK `24.2@6603887`). It
documents what genuinely works, the recipes to reproduce it, and — just as
importantly — the walls that are **not** crossable on the shipped vendor stack and
why.

## Tested environment (read this before reproducing)

All findings/benchmarks here are on the **stock Radxa BSP — Debian 11 + kernel 5.15**,
**not** Trixie and **not** the 6.6 BSP. Other A733 efforts use those; results can
differ. Exact baseline:

| | |
|---|---|
| Board / SoC | Radxa **Cubie A7A** · Allwinner **A733** (`sun60iw2`), 2×A76 @2.0 + 6×A55 @1.79, ~6 GB LPDDR5, UFS storage |
| OS | **Debian 11 (bullseye)** — *not* Trixie |
| Kernel | **`5.15.147-21-a733`** (Radxa BSP) — *not* mainline, *not* 6.6 |
| GPU | PowerVR **BXM-4-64 MC1**, DDK **`24.2@6603887`**, firmware BVNC `36.56.104.183` |
| Toolchain | glibc **2.31**, gcc **10**, Python **3.9.2** |
| Userspace GL | stock Debian **Mesa 20.3.5** (+ vendor PVR blobs); the Zink-GL recipe uses a *separately-built* **Mesa 25.3** |
| x86 layer | FEX-Emu (built from upstream) · box64 **v0.4.3** |
| Vendor pkgs | `img-bxm-dkms`, `xserver-xorg-img-bxm` from **`radxa-repo.github.io/a733-bullseye`** |

> Bullseye + Python 3.9 are why some things are pinned/built-from-source (e.g. the
> Zink Mesa). Bullseye LTS EOL is ~2026-08-31.

## Install

```sh
./install.sh          # guided: vendor fetch -> kernel patch -> GPU sway desktop (prompts each step)
./install.sh vendor   # fetch the proprietary PowerVR stack from the vendor (not bundled — see below)
./install.sh kernel   # just the pvrsrvkm PRIME patch (dry-runs first)
./install.sh sway     # just the GPU sway+wayvnc desktop
```
(The Zink Mesa build + FEX rootfs stay manual — see `gpu/README.md`, `fex/README.md`.)

## What's here

| Dir | Contents |
|-----|----------|
| [`kernel/`](kernel/) | **`pvrsrvkm` DRM PRIME-import patch** — adds standard `gem_prime_import` / `prime_fd_to_handle` (which the vendor left unimplemented) so zink/wlroots can share buffers with the GPU. The single most useful patch here. |
| [`gpu/`](gpu/) | **Zink-on-Vulkan** GL recipe (incl. the one Mesa patch needed) + a **GPU-composited `sway` + `wayvnc`** Wayland desktop (configs + service files). |
| [`fex/`](fex/) | **Custom FEX Vulkan thunk** (x86 Vulkan → native PowerVR GPU) + FEX setup/launcher scripts + Chrome-on-FEX recipe. |
| [`box64/`](box64/) | Usage notes for box64 on A733 (links upstream; nothing forked). |
| [`docs/FINDINGS.md`](docs/FINDINGS.md) | **The capability matrix** — every proven-working path and every confirmed wall, with the *why*. Read this first if you're deciding what's worth attempting. |
| [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) | Measured numbers — CPU, FEX x86→ARM overhead, GPU vs CPU, RAM, UFS, thermal. |

## Benchmarks (highlights)

Full tables in [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md). Headlines:

- **CPU (native ARM):** 875 ev/s single-core (A76 @ 2.0 GHz), 3204 ev/s all-8 (sysbench).
- **FEX x86→ARM overhead** (native ARM = 1.0×): most code **1.1–2.2×** — atomics 1.08×, flags 1.26×, x87 1.44×, branchy 2.17×; unaligned-atomics a pathological **187×**. Real-app cost is dominated by **JIT compile on cold start**, not steady-state.
- **GPU vs CPU** (PowerVR BXM, offscreen GLES shader): **~150–175×** the CPU's best case, **~600×** vs the software (softpipe) fallback. Fill-rate ceiling ~4.2 Gpix/s.
- **RAM (LPDDR5 4800 MT/s):** ~15.5 GB/s read (8-thread). **UFS:** 1.64 GB/s read / 255 MB/s write / 115k IOPS 4K-read.

## ⚠️ What can't be redistributed here — and the workaround

This repo is **only** open / original work (patches, scripts, docs). It cannot
legally include the proprietary pieces — **but every one of them is fetchable from
its official source**, so the workaround is *"install script pulls from the vendor;
this repo layers the patches on top."* Nothing is bundled here.

| Can't ship here | Where it actually comes from | How the workaround gets it |
|---|---|---|
| **Entire PowerVR userspace + firmware** — `libGLESv2_PVR_MESA`, `libVK_IMG`, `libsrv_um`, `libEGL`, `rgx.fw.*` (all in one package: **`xserver-xorg-img-bxm`**) | Radxa / Imagination vendor channel — the Radxa Cubie A7A image, the Radxa apt repo, or `radxa/allwinner-target` (branch `target-a733-v1.4.x`) | `install.sh vendor` installs the vendor `.deb` from your configured source (it does **not** download it from us) |
| **`img-bxm-dkms`** kernel module source | Radxa apt repo (`a733-bullseye`) | `apt-get install img-bxm-dkms`, then **our patch** (`kernel/`) applies on top |
| **FEX x86 rootfs** (3.8 GB) | you build it | `fex/complete-fex-env.sh` rebuilds it from a base image |
| **Steam / Proton / Chrome** etc. | their own vendors | install them into your rootfs yourself |

So the model is **patch + recipe + fetch-from-vendor**, the same pattern DKMS /
proprietary-driver installers use: we never host the closed bits, we point the
installer at the vendor's own distribution and apply the open work over it.
Run `./install.sh vendor` to do the fetch step (it tells you exactly where to point
it if the package isn't already in your apt sources).

## Honest summary

The GPU is **fully usable per-workload** — Vulkan, GL via Zink, native GLES on X11,
a GLES2-class `sway` desktop, H.264 hardware encode. It **cannot** be the *default*
renderer or drive a desktop-GL environment like KDE Plasma; that ceiling is the
closed vendor stack and is only liftable by mainline (`drm/imagination` + Mesa
`pvr`), which for A733 is still at the bare-DTS upstreaming stage. See
[`docs/FINDINGS.md`](docs/FINDINGS.md) for the full matrix.

## Contributing — this is meant to be a living baseline

The point of publishing is so others can **reproduce, test, optimize, and extend** —
and push their findings back so the project grows. Concretely:

- **Reproduce the numbers:** the harnesses are in [`bench/`](bench/); add your
  board's results to [`RESULTS.md`](RESULTS.md). Cross-board data makes regressions
  and wins visible.
- **Challenge a finding:** [`docs/FINDINGS.md`](docs/FINDINGS.md) is dated
  observations, not gospel — if a "wall" falls for you (newer DDK, mainline, a flag),
  that's a great PR.
- **Open problems / help wanted** (the live walls): Wayland GPU *clients* (zink
  kopper crash), transparent EGL→GPU, FEX AOT/code-cache cold-start, Steam-CEF under
  FEX, mainline `drm/imagination` tracking, HEVC HW encode. See
  [`CONTRIBUTING.md`](CONTRIBUTING.md).

Issue templates (benchmark result / bug / board test) and a PR checklist are set up.
**Rule:** no proprietary blobs, rootfs, app binaries, or secrets in commits.

## Acknowledgments
Built on the Linux kernel DRM subsystem, the Imagination PowerVR DDK, the Radxa /
Allwinner BSP, FEX-Emu, box64, Mesa/Zink, and sway/wlroots/wayvnc — plus the wider
A733 community (NickAlilovic, OctaneOS, crescenzo77, Orange Pi, dok2d). Full list +
links in [`ACKNOWLEDGMENTS.md`](ACKNOWLEDGMENTS.md).

## License

Original code/patches/scripts here are MIT (see `LICENSE`). They are intended to be
applied on top of vendor/upstream sources that carry their own licenses; obtain
those from their respective sources.
