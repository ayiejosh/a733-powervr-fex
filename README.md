# a733-powervr-fex

Patches, recipes, and hard-won findings for getting the **PowerVR BXM-4-64 GPU**
and **x86 emulation** working on the **Allwinner A733** (`sun60iw2`) — as shipped on
the Radxa Cubie A7A / A7S boards.

> **Branches:** this is the **`trixie` / kernel 6.6** line (Debian 13, kernel
> `6.6.x-aw2511`). The original Debian 11 / kernel 5.15 work lives on the
> [`bullseye`](../../tree/bullseye) branch. They are different OS + kernel + Mesa
> stacks; cross-check the **Tested environment** table below before reproducing.

This branch is the result of an extended bring-up on a Cubie A7A running **Debian 13
(trixie)** on the **6.6 BSP**. It documents what genuinely works — including
**GPU-accelerated Direct3D 9/10/11**, **GPU OpenGL via zink**, and **Windows apps via
Hangover** — the recipes to reproduce it, and — just as importantly — the walls that
are **not** crossable on the shipped vendor stack and why (the big one: a live
GPU-composited desktop **deadlocks the kernel**).

## Tested environment (read this before reproducing)

All findings/benchmarks here are on **Debian 13 (trixie) + kernel 6.6**. The bullseye
branch is a *different* stack (Debian 11 / 5.15). Exact baseline:

| | |
|---|---|
| Board / SoC | Radxa **Cubie A7A** · Allwinner **A733** (`sun60iw2`), **heterogeneous big.LITTLE** — cores **0-5 LITTLE** (cap 385), cores **6-7 BIG** (cap 1024); max ~1716 MHz (firmware-capped; 1794 unreachable), ~6 GB LPDDR5 |
| OS | **Debian 13 (trixie)** — *not* bullseye |
| Kernel | **`6.6.x-aw2511`** (Radxa A733 BSP, `pvrsrvkm` out-of-tree DKMS) — *not* mainline |
| GPU | PowerVR **B-Series BXM-4-64 MC1**, BVNC `36.56.104.183`, closed `pvrsrvkm` + closed `libVK_IMG` Vulkan blob (**Vulkan 1.3.277**) |
| Display | split: `card0` = `sunxi-drm` (HDMI scanout) · `card1`/`renderD128` = `pvrsrvkm` (render), bridged by DRM-PRIME/renderonly |
| Userspace GL | **TWO Mesa installs (conflict)** — `/usr/local` IMG Mesa **24.0.1** (`pvr_dri`, X11-only, drives the desktop, ldconfig-priority) vs system Mesa **25.0.7** (has `zink`+`kmsro`); version-incompatible, scope each per app |
| x86 layer | **FEX-Emu** (the default for x86-64 Linux ELF via binfmt) · **box64 v0.4.3** (explicit-invoke) |
| Windows | **Hangover 11.9** (wine 11.9 + FEX/box64 WoW64) · **DXVK-Sarek** (native arm64ec) for D3D |
| Vendor pkgs | `img-bxm-dkms`, `xserver-xorg-img-bxm` from the Radxa A733 repo |

> The dual-Mesa split is load-bearing: the desktop runs on the `/usr/local` IMG Mesa
> (X11), while zink/kmsro live in the system Mesa 25.0.7. Keep both; scope the env per
> use (see `gpu/README.md`).

## Install

```sh
./install.sh          # guided: vendor fetch -> kernel patch -> GPU sway desktop (prompts each step)
./install.sh vendor   # fetch the proprietary PowerVR stack from the vendor (not bundled — see below)
./install.sh kernel   # just the pvrsrvkm PRIME patch (dry-runs first)
```
(The Mesa/zink build, FEX rootfs, DXVK-Sarek, and Hangover stay manual — see the
per-component READMEs.)

## What's here

| Dir | Contents |
|-----|----------|
| [`kernel/`](kernel/) | **`pvrsrvkm` DRM PRIME-import patch** (still applies on 6.6) + the finding that a **live GPU compositor through `pvrsrvkm` deadlocks the kernel**. |
| [`gpu/`](gpu/) | **zink OpenGL** on PowerVR Vulkan (off-screen) + **`gpu/dxvk/`: GPU-accelerated Direct3D 9/10/11 (FL 11_0)** via native arm64ec DXVK-Sarek — the headline new capability. |
| [`windows/`](windows/) | **Windows apps via Hangover 11.9** — `winrun` (CLI) and `guirun` (GUI, software-GL) launchers; verified 7-Zip / Notepad / WordPad. |
| [`fex/`](fex/) | FEX setup + the custom Vulkan/GLES thunks; FEX is now the **default** x86-64 binfmt interpreter, with trixie tuning (`TSOEnabled=0` + `Multiblock=1`). |
| [`box64/`](box64/) | box64 **0.4.3** (built from source) usage + the static-glibc-MT → FEX routing. |
| [`docs/FINDINGS.md`](docs/FINDINGS.md) | **The capability matrix** — every proven path and every confirmed wall, with the *why*. Read this first. |
| [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) | Measured numbers — CPU, FEX/box64 overhead, the D3D draw-call / fill-rate characterization. |

## Capabilities at a glance (trixie)

- **x86 / x64 Linux** → **FEX** by default (binfmt); **box64 0.4.3** on explicit invoke. Single-thread emulation reaches ~71% (FEX-tuned) / ~64% (box64-tuned) of native on a big core.
- **Windows apps** (Hangover 11.9): CLI (`winrun`) and GUI (`guirun`, software GL). Verified 7-Zip, Notepad, WordPad.
- **GPU Direct3D 9/10/11 (FL 11_0)** via DXVK-Sarek → PowerVR Vulkan (`d3drun`): instancing, compute, render-to-texture, BC1-5 textures, depth/MRT, windowed present. (D3D12 infeasible; geometry/tessellation/MSAA not native.)
- **GPU OpenGL** via **zink** → PowerVR Vulkan (`glrun`): **off-screen only** (GLES2/GL2.1 class).
- **NOT possible:** a **GPU-accelerated desktop** — a live compositor on `pvrsrvkm` hard-hangs the kernel. The desktop stays software-rendered. See `docs/FINDINGS.md`.

## Benchmarks (highlights)

Full tables in [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md). Headlines (trixie):

- **CPU single-thread** (sum-ms CPU bench, big core): native **5488** / box64-tuned **8564** (~64% of native) / **FEX-tuned 7746** (~71% of native).
- **box64 0.4.3** built from source: clone3 fixed, **~9% faster** than the Debian 0.3.4; `CALLRET=1 + SAFEFLAGS=0` ≈ **-15%** on the CPU bench.
- **FEX tuning:** `TSOEnabled=0 + Multiblock=1` ≈ **-7%** on the CPU bench (load-bearing knob is TSO-off).
- **GPU D3D11** (DXVK-Sarek → PowerVR): instancing ~**370k tris/s**; trivial windowed present ~**227 fps** (~530 fps for clear-only); the draw-call submission ceiling is ~**1000–1100 PSO-swap-bound draws/s** of distinct pipeline state — a realistic textured/depth frame is **GPU-fill-bound**, not emulation-bound.
- **GPU OpenGL** (zink, off-screen): **glmark2-es2 `--off-screen` = 661**.

## ⚠️ What can't be redistributed here — and the workaround

This repo is **only** open / original work (patches, scripts, docs). It cannot legally
include the proprietary pieces — **but every one of them is fetchable from its official
source**, so the workaround is *"install script pulls from the vendor; this repo layers
the patches on top."* Nothing is bundled here.

| Can't ship here | Where it comes from | How the workaround gets it |
|---|---|---|
| **PowerVR userspace + firmware** — `libVK_IMG`, `libsrv_um`, `libEGL`, `rgx.fw.*` (package: **`xserver-xorg-img-bxm`**) | Radxa / Imagination vendor channel | `install.sh vendor` installs the vendor `.deb` from your configured source |
| **`img-bxm-dkms`** kernel module source | Radxa apt repo | `apt-get install img-bxm-dkms`, then **our patch** (`kernel/`) on top |
| **DXVK-Sarek / Hangover / wine** | their own upstreams | build/install yourself (see `gpu/dxvk/`, `windows/`) |
| **FEX x86 rootfs**, Steam / Chrome / Windows apps | you build / their vendors | build the rootfs; install apps into it yourself |

So the model is **patch + recipe + fetch-from-vendor** — we never host the closed bits.

## Honest summary

The GPU is **fully usable per-workload** — Vulkan, **GPU D3D9/10/11**, GL via zink
(off-screen), windowed-present. It **cannot** drive a GPU-composited desktop: a live
compositor on `pvrsrvkm` **deadlocks the kernel** (mutex spin-on-owner in IRQ →
power-cycle). That ceiling is the closed kernel driver and is only liftable by a fixed
`pvrsrvkm` / mainline `drm/imagination` (not present for A733 on this kernel). The
desktop stays on **software-rendered X11**. See [`docs/FINDINGS.md`](docs/FINDINGS.md)
for the full matrix.

## Contributing — this is meant to be a living baseline

The point of publishing is so others can **reproduce, test, optimize, and extend** —
and push their findings back. Concretely:

- **Reproduce the numbers:** harnesses are in [`bench/`](bench/); add your board's
  results to [`RESULTS.md`](RESULTS.md).
- **Challenge a finding:** [`docs/FINDINGS.md`](docs/FINDINGS.md) is dated observations,
  not gospel — if a "wall" falls for you (newer DDK, fixed `pvrsrvkm`, mainline), that's
  a great PR.
- **Open problems / help wanted:** the GPU-desktop kernel deadlock, the draw-call
  submission ceiling, general VS-as-compute for the GS-emulation path, D3D12. See
  [`CONTRIBUTING.md`](CONTRIBUTING.md).

Issue templates and a PR checklist are set up.
**Rule:** no proprietary blobs, rootfs, app binaries, or secrets in commits.

## Acknowledgments
Built on the Linux kernel DRM subsystem, the Imagination PowerVR DDK, the Radxa /
Allwinner BSP, FEX-Emu, box64, Mesa/Zink, DXVK / DXVK-Sarek, Hangover and wine — plus
the wider A733 community. Full list + links in [`ACKNOWLEDGMENTS.md`](ACKNOWLEDGMENTS.md).

## License

Original code/patches/scripts here are MIT (see `LICENSE`). They are intended to be
applied on top of vendor/upstream sources that carry their own licenses; obtain those
from their respective sources.
