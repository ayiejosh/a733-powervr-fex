# x86 on A733 via FEX — the default x86 interpreter (trixie / 6.6)

[FEX-Emu](https://github.com/FEX-Emu/FEX) runs x86/x86-64 binaries on this ARM64 board.
On the **trixie / 6.6** stack FEX is the **default** x86-64 *and* x86-32 Linux ELF
interpreter via `binfmt_misc` — a bare `./prog.x86_64` runs under FEX automatically.
box64 is kept for explicit invocation (`box64 ./app`); see `../box64/`.

> The FEX build here is on **Debian 13 (trixie) / kernel 6.6** (the bullseye branch's
> notes are for the older 5.15 stack). Bring your own x86 rootfs and apps — none are
> bundled.

## FEX is the binfmt default

box64 0.4.3 aborts on static-glibc multithreaded binaries (mutex `__owner` corruption);
FEX handles static **and** dynamic multithreading correctly. So bare x86-64/x86-32 execs
are routed to FEX by disabling box64's binfmt rule with an empty
`/etc/binfmt.d/box64.conf` override (see `../box64/README.md`). Result: `FEX-x86` and
`FEX-x86_64` are the only x86 ELF handlers.

> A corrupt FEX binfmt magic can break **all** binary execution on the board (the magic
> must end `…02003e00`; a truncated one catches every aarch64 ELF). The
> `binfmt-guard` service in `../system/` self-heals that — keep it enabled.

## Tuning (validated on this board, CPU bench)

Trixie FEX config (`~/.fex-emu/Config.json`):

```json
{"Config":{"RootFS":"<your-rootfs>","TSOEnabled":"0","Multiblock":"1"}}
```

- `TSOEnabled=0` — the load-bearing knob (~-6.8% on the CPU bench; the win is in
  memory-ordering-bound loops — hash ~-12%, qsort ~-8.5%). **Correctness risk for
  MULTITHREADED guests** (it stops FEX inserting the x86-TSO barriers ARM doesn't have):
  validate per multithreaded workload, or keep the default and accept the loss.
- `Multiblock=1` — larger JIT block formation, safe, ~+0.3% on top (kept).
- Together: ~**-7.1%** (best). Back up `Config.json` before applying; a persistent
  FEXServer means new processes pick up the config on launch.

Knobs that were requested but are **not present** in this FEX build (confirmed via
`strings`): AOT/object cache (only experimental WIP flags), `ParanoidTSO` (maps onto
`TSOEnabled`), `StaticRegisterAllocation` (always-on internally). `X87ReducedPrecision`
exists and gives a large win for legacy 64-bit-on-x87 code but is irrelevant to SSE-based
64-bit apps — see `Config.json.example` and `../docs/BENCHMARKS.md`.

## FEX patches + config (validated wins)
See [`patches/`](patches/) — local FEX patches developed on this board:
- **Perf:** unaligned-atomic backpatch (**~88x**) + thread-context pooling (**~6.8x**).
- **Build-compat / Chrome / VAAPI / stability:** namespace-sandbox support, a VAAPI thunk
  for VE2 H.264, an SMC null-guard.
- **Config:** [`Config.json.example`](Config.json.example) — `X87ReducedPrecision`,
  `HideHybrid`, TSO.

⚠️ AI-assisted; **not for upstream** (FEX no-AI policy). Apply to your own MIT FEX build;
run the SMC-stress correctness gate. Read `patches/README.md`.

## The novel bit: a Vulkan GPU thunk

By default a Vulkan app inside FEX would hit a *software* x86 Vulkan (or nothing). The
thunk forwards Vulkan from the x86 guest to the **native ARM `libVK_IMG`**, so x86 apps
render on the real PowerVR GPU. Compute dispatch + WSI forwarding are verified working.

- `vkthunk_render.c` — standalone render test/reference (GBM render node -> import -> GPU
  fill -> readback).
- `ThunkConfig.json`, `ve2-thunks.json` — FEX thunk wiring.

There is also a full x86 OpenGL ES 3.2 thunk in
[`thunks/libEGL-gles/`](thunks/libEGL-gles/) (x86-64 and i386 -> native PowerVR).

## Setup / launcher scripts

| Script | What it does |
|--------|--------------|
| `complete-fex-env.sh` | Brings an x86 rootfs to app-readiness (i386 multiarch, codecs, X-session libs, audio socket). |
| `fex-install` | Install an x86 `.deb` into the rootfs + create a host launcher. |
| `fexrun` | Run an x86 command under FEX with the right env. |
| `glx-run` | Run an x86 GL app via a nested Xephyr (software/llvmpipe GLX). |
| `chrome-fex-gpu.sh` | Launch x86 Chrome under FEX (tuned flags). |
| `steam-fex` | Launch Steam under FEX (**WIP**). |

## What works / what doesn't (be realistic)

- ✅ **Bare x86-64/x86-32 execs** run under FEX automatically (binfmt); static + dynamic
  multithreading correct (where box64 aborts on static-MT).
- ✅ **x86 Vulkan -> native PowerVR GPU** via the thunk (compute + WSI verified).
- ⚠️ **Steam** — client renders, but the CEF `steamwebhelper` loops on a
  bwrap/pressure-vessel failure under FEX. Not solved.
- ⚠️ Cold start is slow (FEX JIT compile-bound); no AOT cache in this build.

For **Windows** apps and **D3D**, FEX is used *under* Hangover / DXVK-Sarek WoW64 — see
`../windows/` and `../gpu/dxvk/`. See `../docs/FINDINGS.md` for the GPU capability matrix.
