# A733 / PowerVR BXM-4-64 — capability matrix & walls (trixie / 6.6)

Hardware: Allwinner A733 (`sun60iw2`), GPU **Imagination PowerVR B-Series
BXM-4-64 MC1** (BVNC `36.56.104.183`, Vulkan **1.3.277**), Radxa BSP **Debian 13
(trixie)**, kernel `6.6.x-aw2511`, closed `pvrsrvkm` (out-of-tree DKMS) + closed
`libVK_IMG` Vulkan ICD.

> This is the **trixie / 6.6** matrix. The Debian 11 / 5.15 matrix is on the
> [`bullseye`](../../../tree/bullseye) branch — a different stack.

The short version: **the GPU works great per-workload — including GPU Direct3D
9/10/11 and off-screen GL — but it cannot drive a GPU-composited desktop (that
deadlocks the kernel).** Details below.

## Proven working

**x86 / x64 emulation**
- **Linux x86-64 ELF** -> routed to **FEX** by default (binfmt). FEX handles
  clone3/threads and static-glibc multithreading correctly. Run a bare
  `./prog.x86_64` and it uses FEX.
- **Linux x86-32 ELF** -> FEX (`FEX-x86` binfmt).
- **box64 v0.4.3** (built from source) at `/usr/bin/box64`, used **explicitly**
  (`box64 app`) — clone3 fixed vs the Debian 0.3.4, ~9% faster, with a heavily-tuned
  `/etc/box64.box64rc`. Strength = dynamically-linked games via lib-wrapping.

**Windows apps (Hangover 11.9 = wine 11.9 + FEX/box64 WoW64)** — see `windows/`
- **CLI/console apps** (`winrun`): verified 7-Zip 24.08 — full compress/test/extract
  byte-perfect, plus the multithreaded LZMA benchmark.
- **GUI apps** (`guirun`, **software-GL** window): verified Notepad, WordPad, 7-Zip
  File Manager — real titled windows, clean lifecycle. GUI render is CPU/llvmpipe
  (the PowerVR GL blob deadlocks wine graphics init).

**GPU Direct3D 9/10/11 (FL 11_0)** via native arm64ec DXVK-Sarek -> PowerVR Vulkan — see `gpu/dxvk/`
- WORKS: **instancing** (~370k tris/s), **compute/GPGPU**, **render-to-texture**,
  **textures incl. BC1-5** (decoded in-driver), **depth/Z**, **MRT**, **windowed
  present** (~227 fps; software-llvmpipe window). Verified end-to-end (`tri.exe`,
  textured+depth+BC1 `cube.exe`) without wedging the board.
- The headline new capability vs the bullseye branch (where DX/DXVK was a documented
  dead-end). See the D3D capability matrix below.

**GPU OpenGL via zink -> PowerVR Vulkan (OFF-SCREEN)** — see `gpu/README.md`
- `glrun` runs **EGL/off-screen** GL on the GPU (system Mesa 25.0.7 zink + the
  feature-strip Vulkan layer). **glmark2-es2 `--off-screen` = 661.** GLES2 / GL2.1
  class (hardware feature ceiling). **Windowed / desktop GL does NOT work** (see walls).

**GPU compute / Vulkan**
- The closed `libVK_IMG` Vulkan blob **works for off-screen render** (compute,
  graphics-to-FBO). Vulkan 1.3.277, INTEGRATED_GPU, ~5.9 GB device-local.

**Kernel**
- **DRM PRIME import** — the `pvrsrvkm` patch in `kernel/` (still applies on 6.6)
  implements `prime_fd_to_handle` / `gem_prime_import`, so zink/kmsro can share
  buffers with the GPU. The off-screen kmsro/renderonly bridge on `card0` is proven
  (a SCANOUT|RENDER buffer succeeds).

## Walls (with the reason)

**Hard — kernel/driver class, not fixable from userspace**

- **GPU-accelerated DESKTOP (X11 or Wayland) = HARD-BLOCKED at the KERNEL.** The
  off-screen bridge (kmsro/renderonly on `card0` + zink/PowerVR render) is *proven*.
  But the moment a **live compositor** drives the PowerVR GPU to scan out the HDMI
  display, the **kernel deadlocks** — `pvrsrvkm` `mutex_spin_on_owner` in IRQ -> hard
  hang -> power-cycle (confirmed in `journalctl -b -1` after the attempt). Same failure
  class: KDE GL compositing, `DXVK_HUD`, and `scrot` while a GPU app runs. **Off-screen
  GPU render is fine.** NOT fixable from userspace — needs a fixed `pvrsrvkm`/kernel.
  The desktop stays on **software-rendered X11** (the `LIBGL_ALWAYS_SOFTWARE`
  workaround is the board's defense against exactly this hang). See `kernel/`.
- **D3D12** (vkd3d-proton) — **infeasible**: needs Vulkan features the blob lacks.
- **Geometry shaders / tessellation / MSAA — not native.** The blob *advertises*
  `geometryShader=1` but GS *pipelines* fail (the report is a lie); `tessellation` is
  unsupported; no native MSAA. GS is emulated via compute (see below) but at **~80x
  slow** = compatibility-grade only.
- **Open Mesa PowerVR (`pvr`) Vulkan on this kernel — HARD-BLOCKED.** The open driver
  targets the **mainline `powervr` DRM UAPI** (`DEV_QUERY`/`CREATE_BO`/`SUBMIT_JOBS`);
  our kernel only implements the **closed `pvrsrvkm` bridge UAPI** (`PVR_SRVKM_CMD` +
  sync ioctls). The ICDs don't match; mainline `powervr` is absent from this 6.6 BSP
  (no `CONFIG_DRM_POWERVR`, DT node is `img,gpu` sunxi-BSP, no mainline-format firmware
  for BVNC `36.56.104.183`). So the closed blob is the only GPU stack here.

**Soft — environmental / emulator class**

- **box64 static-glibc multithreaded binaries -> abort.** box64 0.4.3 corrupts the
  glibc mutex `__owner` field on static-linked pthread programs (`pthread_mutex_lock`
  assertion / SIGSEGV / deadlock), **nondeterministically, under every config tried**
  (22 configurations, 0 passes — it is not tunable away). **Mitigation:** FEX is the
  binfmt default for bare `./binary` execs and handles static MT correctly, so this
  only bites if box64 is invoked *explicitly* on a static threaded binary. Dynamically
  linked x86-64 MT binaries run fine under box64.
- **Windows / D3D GUI is software-GL.** The PowerVR GL blob deadlocks wine's graphics
  init, so `guirun` and DXVK's windowed *present* use an llvmpipe (CPU) window. The D3D
  *render* is on the real GPU; only the final window blit is software (~1.9 ms/frame).

## D3D capability matrix (DXVK-Sarek -> PowerVR Vulkan)

| Feature | Status | Note |
|---|---|---|
| D3D11 feature level 11_0 | works | `FL=0xb000`; D3D9/10 paths via DXVK too |
| Instancing | works | ~370k tris/s |
| Compute / GPGPU | works | |
| Render-to-texture / depth (Z) / MRT | works | |
| Textures incl. **BC1-5** | works | decoded in-driver (a patch added to this DXVK-Sarek build) |
| Windowed present | works | software-llvmpipe window (~1.9 ms/frame), ~227 fps trivial scene |
| Geometry shaders | emulated | compute-based, ~80x slow, gated OFF (`d3d11.emulateGeometryShaders`) |
| Tessellation / MSAA (native) | no | blob lacks the features |
| D3D12 (vkd3d-proton) | no | needs Vulkan features the blob lacks |

> **NEVER set `DXVK_HUD`** — it wedges the GPU (the live-compositor deadlock class).

## Why the draw-call wall is *not* GPU/emulation bound

Profiling the shipping DXVK-Sarek stack (strace delta + DXVK info log; `perf`
unavailable on the custom kernel):

- The only state op that materially costs is the **pipeline (PSO) swap**: ~+3 us/draw,
  ~40% throughput drop (~140k -> ~99k trivial draws/s; the distinct-PSO ceiling for
  real content lands at **~1000-1100 draws/s**). Constant-buffer / SRV / vertex-buffer
  rebinds and Draw-vs-DrawInstanced are all near-noise.
- The PSO cost lands on **DXVK's CPU side** (state re-record + CS-thread handoff): the
  added syscalls are **futex** (DXVK thread sync), while **ioctl** (the kernel/driver
  submit count) stays **flat** — so it is **not** the wine->driver thunk, **not** the
  PowerVR blob, and **not** shader compilation (state-cache hit confirmed).
- For a **realistic** textured/depth frame, the dominant cost is **GPU fill** (PowerVR
  BXM-4-64 raster/fragment ceiling), scaling linearly with triangle count and
  dominating CPU-record by 4-6x. No remaining DXVK software lever speeds it.

## Geometry-shader compute-emulation (branch `gs-latest`)

A compute-based GS emulation (libpoly-style: GS->compute SPIR-V codegen + a 3-pass
runtime driver — VS capture -> compute-GS dispatch -> counter-driven indirect draw) is
**proven to render**: the probe `gs.exe` produces the GS-tinted output (`GS_OK`,
RC=0, reproducible). Native GS is a genuine wall — the BXM blob itself reports
`geometryShader: 0` and a GS draw dies in `vkCreateGraphicsPipelines` (rc=3) with our
feature fakes on *or* off — so this emulation is the only way a GS workload runs here.

It costs **~676 us/draw against a ~11.2 us/draw no-GS baseline (~60x)**, so it is
compatibility-grade only, gated behind `dxvk.conf d3d11.emulateGeometryShaders`
(default OFF).

**2026-09-24: ported onto the shipping base.** The work was written 2026-06-21 against
June `main`; the shipping branch (`bcn-update-20260921`) had grown 4830 commits past that
base, so the test dlls had none of the BC1-5 decode patch or the WSI rework and a
BC-textured app could not run on them at all (`cube.exe` page-faulted in
`CreateTexture2D`). Branch **`gs-latest`** (`80756906`) is the merge onto the shipping
base: one conflict (`d3d11_options.h`, both option fields kept), builds clean with **no
source change**, `gs.exe` still `GS_OK`, and the non-GS battery — BC1-5, textures,
compute, RTT, depth, MRT, present, `tri` — is clean. The three amortisation folds are
worth **-32.6% per draw** on the new base too (1005.9 -> 677.6 us), measured against
`gs-latest-nofold` (`8652b433`) in the same session. Still **not default-on**: the path
assumes a fixed vertex stride, a 3-slot output record and 3-in/3-out triangle lists,
ignores indexed draws and the application VS, intercepts only `Draw`/`DrawIndexed`, and
its post-dispatch barrier is implicit. Full detail in `gpu/dxvk/README.md`.

The general Pass-1 (VS-as-compute for arbitrary app vertex shaders, with format-aware
input gather) is **scoped but not implemented** — the current input path is a
probe-specific shortcut.

## The only lever that lifts the hard walls

A **fixed `pvrsrvkm`** (no live-compositor mutex deadlock) and/or **mainline
`drm/imagination` + Mesa `pvr`** for A733 — which would give a clean open Vulkan, a
GPU desktop, and potentially the missing features. Mesa's `pvr` docs *list* this exact
part (BXM-4-64, BVNC `36.56.104.183`), but on **this** kernel the UAPI/DT/firmware
don't line up (see the open-Mesa wall above), and A733 mainline is at the bare-DTS
upstreaming stage. Months+ away, but it's the only path that raises the ceiling.

## x86 emulation tuning — see `fex/`, `box64/`

- **FEX is the default** x86-64/x86-32 Linux ELF interpreter (binfmt). Trixie tuning:
  `TSOEnabled=0` + `Multiblock=1` (~-7% on the CPU bench; the load-bearing knob is
  TSO-off). **TSO-off carries a multithread-correctness risk** — revert if a
  multithreaded guest misbehaves.
- **box64 0.4.3** (explicit invoke): `CALLRET=1` (~-9%) + `SAFEFLAGS=0` (~-4%) ~ -15%
  on the CPU bench. `SAFEFLAGS=0` is a flag-correctness risk; `CALLRET=1` alone is the
  safe ~-9%.
- Single-thread emulation reaches ~**71%** (FEX-tuned) / ~**64%** (box64-tuned) of
  native on a big core. The biggest system lever is **core placement** (big vs LITTLE,
  ~2.3-6.5x) — handled by the scheduler; don't hard-pin.

## 2026-07-02 additions

### Closed vs mainline GPU firmware: same packaging family, decoded byte-level
`/lib/firmware/rgx.fw.36.56.104.183` (closed DDK 24.2) and mainline
`powervr/rogue_*_v1.fw` are BOTH 32-bit MIPS ELFs with the SAME trailing-4K
`pvr_fw_info` block and identical 6×24-byte layout tables — the closed file is header
**v2** (flags = closed build options `0x80020810`, fw ABI = DDK 24.2 build 6603887),
mainline requires **v3** + the `OPEN_SOURCE` flag + open fw ABI v1. So conversion is NOT
possible (kernel↔fw shared-struct ABI differs), but the gap is precise: **IMG building
its open-ABI firmware for BVNC 36.56.104.183** is a build-config request — they already
ship the sibling BXM revision `36.53.104.796` (TH1520/LicheePi 4A) in linux-firmware.
That is the single missing artifact between the A733 and the mainline `powervr` DRM +
Mesa open driver (which already carries `bxm-4-64.h` with this exact BVNC).

### DXVK-Sarek `dxvk.tilerMode`: currently a NO-OP on this stack
Sarek's backport only sets `preferCachedMemory`; the actual tiler render-pass
optimization is still TODO upstream, and `Auto` already matches the Imagination driver
ID. Don't chase it for perf on PowerVR yet. (The upstream rebase DID bring a real UMA
fix: heap budget is no longer wrongly enforced on unified-memory GPUs.)

### FEX rebuild (d848cbb + patches) beats the bullseye numbers
Same bench suite, trixie vs bullseye baseline: unaligned atomics **+11–18%**
(136.6/53.8/51.1 Mops vs 120.8/48.3/43.4), thread create+join **30% faster**
(139,849 ns vs 199,995 ns), CPU/GPU baselines unchanged. See bench/baseline.txt.

## 2026-09-24 additions (full-suite verification)

### Every applied gain re-measured in one session — all still hold
`bench/run.sh` vs `baseline.txt`: cpu.1thread **847.83 vs 847.80**; FEX atomics
153.2/60.8/56.8 vs 152.1/61.4/56.9 Mops; x87 ratio **18.7x vs 18.9x**; glbench
**7589/2225/579/147** vs 7392/2229/579/147 Mpix (GPU at 1104 MHz, DSU 1027 MHz, verified
through `clkctl`). D3D11: 21/27 apps clean, baseline draw **4.690 us/draw** (repeat spread
0.15%), realistic frames GPU-fill-bound, PSO swap still the only costly state op (+54%).
Desktop GL over zink: **2.2–3.5x** faster than llvmpipe windowed, 0 swap errors. Open
driver: `regress.sh` **29 passed, 0 failed** plus GL/render repeats PASS.

### `bench/run.sh` read NA for the GPU rows inside a Plasma session — FIXED
`MESA_LOADER_DRIVER_OVERRIDE=zink` is session-wide for the desktop GL change, so Mesa's EGL
tried zink on the GBM platform and the vendor GLES path died with `eglInitialize 0x3001`,
leaving all four `gpu.glbench` rows NA — which reads like a GPU regression but is not one.
`bench/run.sh` now strips the session GL variables (`MESA_LOADER_DRIVER_OVERRIDE`,
`GALLIUM_DRIVER`, `LIBGL_DRIVERS_PATH`, `LIBGL_KOPPER_DRI2`, `LIBGL_ALWAYS_SOFTWARE`,
`VK_ICD_FILENAMES`, `VK_DRIVER_FILES`, `VK_INSTANCE_LAYERS`, `VK_LAYER_PATH`, `PVR_FAKE_*`)
for the glbench section and warns if a row still comes back empty. Verified after the fix:
**7616/2224/579/147** Mpix vs the 7392/2229/579/147 baseline.

### One entry point for all of it: `bench/full.sh`
Four selectable phases — `canonical` (CPU/FEX/GPU GLES), `d3d` (the 30-program D3D11
matrix + the timed suite, now in-tree under `bench/d3d11/`), `gs` (geometry-shader
emulation A/B) and `open` (the Vulkan/GL suite on the open driver through the module-swap
wrapper). Per-phase logs plus a `SUMMARY.txt`; a failed phase does not hide the others and
the exit status is non-zero if any failed. `docs/FULL-BENCHMARK.md` records what is
covered, what is not, and when two numbers may be compared (same session for evidence,
`baseline.txt` only with the clock overlays applied and the session GL environment
stripped, never by md5 — the arm64ec link is not bit-reproducible).

### Known-failing D3D apps, confirmed pre-existing (layer on == layer off)
`msaa.exe`/`msaa2.exe` (MSAA_FAIL, no MSAA in the blob), `tess.exe` (rc=3, no tessellation),
`d7test.exe`/`d3d7test.exe` (no D3DHALDevice), and `mrt.exe` (MRT_FAIL — the
`SV_VertexID`/no-input-layout variant; `mrt2.exe` with a real vertex buffer is MRT_OK, which
is what the "MRT works" row below rests on). None is caused by `PVR_FAKE_*`/the strip layer.

### Geometry shaders: the blob itself reports `geometryShader: 0`
With `PVR_FAKE_GS` unset DXVK sees `geometryShader: 0`; with the session fake on it sees 1.
Either way a GS draw on the shipping dll dies in `vkCreateGraphicsPipelines` (rc=3), so
native GS is a genuine wall and the compute emulation is the only way through. Two
consequences: (a) the emulation does not depend on the session fake — it renders GS_OK with
the fake on *and* off; (b) the trade-off note on `PVR_FAKE_GS=1` is about *Vulkan* apps that
trust the flag, not about the DXVK path.
