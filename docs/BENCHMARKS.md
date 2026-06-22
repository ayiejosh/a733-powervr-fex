# Benchmarks — Allwinner A733 / Radxa Cubie A7A (trixie / 6.6)

Measured on a Cubie A7A, **Debian 13 (trixie)**, kernel `6.6.x-aw2511`. Numbers are
indicative (single board, methodology noted per section) — not a controlled suite. Use
for orders-of-magnitude, not precise comparison. The bullseye (Debian 11 / 5.15) branch
has its own, separately-measured numbers; the two stacks are not directly comparable.

## CPU — emulated x86-64 single-thread (the headline trixie number)
SoC: **heterogeneous big.LITTLE** — cores **0-5 LITTLE** (cap 385), **6-7 BIG**
(cap 1024), ~1716 MHz ceiling (1794 MHz firmware-locked, not reachable from sysfs).

`bench/cpubench.c` cross-built static x86-64 (`x86_64-linux-gnu-gcc -O2 -static -lm`);
native = the same source built arm64. Metric = sum of per-section ms (int/float/hash/
qsort/matmul); lower = faster. Pinned to a big core for the headline figure.

| build | sum-ms | % of native |
|---|---|---|
| native arm64 (big core) | **5488** | 100% |
| box64 0.4.3, tuned (`CALLRET=1 SAFEFLAGS=0`) | **8564** | ~64% |
| FEX, tuned (`TSOEnabled=0 Multiblock=1`) | **7746** | ~71% |

> **Core placement is the single biggest lever**: the same x86 work runs ~**2.3-6.5x**
> faster on a BIG core (6-7) than a LITTLE core (0-5) — pure IPC, even at the same clock
> (e.g. matmul 6.5x, primes/mandel/qsort 2.3x). The capacity-aware scheduler already
> puts hot work on the big cores; **don't hard-pin**. The CPU governor (performance vs
> ondemand) is ~0% for sustained compute — its only value is bursty launch latency.

## FEX tuning (x86-64 -> ARM64), CPU bench, TOTAL ms
Single option at a time, then the winner combo, median runs; correctness checksum
identical to native in every case. (Detail: `fex/README.md`.)

| config | TOTAL ms | delta vs default |
|---|---|---|
| default | 20568 | — |
| `TSOEnabled=0` | ~19103 | **-6.8%** (load-bearing) |
| **`TSOEnabled=0` + `Multiblock=1`** | **19118** | **-7.1%** (best, shipped) |

The win concentrates in memory-ordering-bound metrics (hash streaming loads ~-12%, qsort
swap-heavy indirect-call loop ~-8.5%); pure-compute (primes, mandel) is silicon-bound and
unaffected. **`TSOEnabled=0` is a correctness risk for MULTITHREADED guests** (it stops
FEX inserting x86-TSO barriers) — validate per multithreaded workload or keep the default.
`Multiblock=1` is safe. Knobs *not* present in this FEX build: AOT/object cache (only WIP
flags), ParanoidTSO (maps onto `TSOEnabled`), SRA (always-on).

## box64 0.4.3 tuning (explicit invoke), CPU bench
Built from source (clone3 fixed; ~9% faster than the Debian 0.3.4). Paired A/B harness
(cancels neighbor-core load drift); ratios are robust, absolute ms are not.

| option | delta | verdict |
|---|---|---|
| `BOX64_DYNAREC_CALLRET=1` | **-9.4 to -9.7%** | biggest single gain (call/ret; helps the qsort indirect-call path) |
| `BOX64_DYNAREC_SAFEFLAGS=0` | **-4.2%** | win, but flag-correctness risk |
| `CALLRET=1 + SAFEFLAGS=0` | **~-15%** | best combo (stacks) |
| `BOX64_DYNAREC_NATIVEFLAGS=0` | +35% | DO NOT SET |
| `STRONGMEM=1/2/3`, `BIGBLOCK=0/1` | +11 to +16% | slower; defaults already optimal |

Conservative (no flag risk): `CALLRET=1` only (~-9%). No persistent dynarec/JIT cache
exists in 0.4.3 (re-JITs every start). Leave everything else at default.

## GPU — Direct3D 11 via DXVK-Sarek -> PowerVR Vulkan
Native arm64ec DXVK-Sarek, FL 11_0. The shipping `d3d11.dll` (BCn build) was used
unmodified for all timing; `dxvk.conf` unmutated; **never `DXVK_HUD`** (wedges GPU).

### Draw-call submission ceiling (headless RTT, per-frame GPU-finish)
Sweep at 2000 frames x 256 draws; each frame ends `CopyResource+Map` (forces GPU finish).
The DELTA between cases isolates the per-draw CPU state cost.

| state op changed per draw | us/draw | draws/s | delta vs baseline |
|---|---|---|---|
| baseline (no state change) | 7.14 | ~140k | — |
| **PSO swap (VS+PS pair)** | 10.09 | ~99k | **+2.96 us (+41%)** |
| constant-buffer update | 7.50 | ~133k | +0.36 us |
| SRV / texture rebind | 7.66 | ~130k | +0.53 us |
| vertex-buffer rebind | 7.52 | ~133k | +0.38 us |
| DrawIndexed / DrawInstanced vs Draw | ~7.1 | ~140k | ~0 (the draw verb is free) |

The **pipeline (PSO) swap is the only state op that moves the needle** (+~3 us/draw,
constant at N=256 and N=1024 -> a true per-draw cost). For real content the
distinct-pipeline draw-call ceiling is **~1000-1100 draws/s**. Attribution (strace delta
b vs a, matched scale): PSO swap adds **futex** calls (DXVK CS-thread sync) while
**ioctl is flat** (driver submit count unchanged) -> the cost is **DXVK CPU-side state
re-record + CS-thread handoff**, NOT the wine->driver thunk, NOT the PowerVR blob, NOT
shader compile (state-cache hit confirmed via DXVK info log). `perf` was unavailable
(kernel-tools mismatch on the custom 6.6 kernel); attribution rests on the syscall delta.

### A realistic frame is GPU-fill-bound
Textured (32x32 RGBA + linear sampler) + depth-tested (D32_FLOAT) 512x512 offscreen RTT,
N small quads, dynamic cbuffer per draw, a few real PSO swaps. CPU-record vs GPU-finish
split per frame:

| draws | tris | CPU-record ms | GPU-finish ms | inferred bound |
|---|---|---|---|---|
| 500 | 1000 | 0.53 | 3.17 | GPU-bound |
| 1000 | 2000 | 0.89 | 6.03 | GPU-bound |
| 2000 | 4000 | 1.78 | 10.96 | GPU-bound |
| 4000 | 8000 | 4.63 | 19.45 | GPU-bound |

GPU-finish scales ~**linearly** with triangle count and is **4-6x** the CPU-record at
every level (CPU-record is <=~19% of frame even at 4000 draws). The linear-with-triangles
fingerprint = the PowerVR BXM-4-64 fill/raster ceiling is the wall, reached through
DXVK's Vulkan path — NOT emulation/submission. No remaining DXVK software lever speeds it.

### Present cost (windowed swapchain, software llvmpipe window)
Trivial clear-only scene, IMMEDIATE present, vsync off: clear/record ~0.015 ms;
**present ~1.87 ms/frame** (~530 fps). For a realistic frame already spending 7-24 ms on
GPU render, present is a ~8-25% tax, not the dominant cost (and overlappable). The ~1.9 ms
is inherent to the **software** window path (the broken PowerVR GL blob forces an llvmpipe
blit of the backbuffer to the X11 window); the only true fix is a working GL/WSI present
blob (a driver problem) or rendering offscreen.

### Other measured D3D points
- Instancing: ~**370k tris/s**.
- Windowed present (realbench trivial scene): ~**227 fps** (~530 fps clear-only).
- BC1-5 textures decode in-driver (textured+depth+BC1 cube renders, `hr=0x0`).

## GPU — OpenGL via zink -> PowerVR Vulkan (off-screen)
System Mesa 25.0.7 `zink_dri.so` on the closed PowerVR Vulkan ICD + the feature-strip
layer. `GL_RENDERER = zink Vulkan 1.3 (PowerVR B-Series BXM-4-64)`.

| bench | result |
|---|---|
| **glmark2-es2 `--off-screen`** | **661** (~662 fps; build 646 / texture 826 / shading 485) |
| eglinfo | EGL 1.5 Mesa, zink -> PowerVR confirmed |
| glmark2 FULL suite `--off-screen` | functional but too slow to finish in 240s (shader-compile bound) |

Ceiling is **GL 2.1 / GLES 2.0** (the blob lacks `fillModeNonSolid`, `descriptorIndexing`,
`robustness2`, etc. — faked features are stripped at `CreateDevice`). **Windowed/GLX GL
does not work** (the X server GLX isn't wired to zink, and a live compositor would hit the
kernel deadlock). zink warns `PowerVR lacks fillModeNonSolid` -> non-solid/wireframe fill
unreliable; solid-fill scenes render correctly.

## Memory / storage / thermal (context, board-level, stable across stacks)
- **RAM (LPDDR5 ~4800 MT/s):** ~17 GB/s read / ~10 GB/s write (8-thread, sysbench).
- **UFS:** ~1.7 GB/s seq read (QD>=8) / 265 MB/s write / ~112k IOPS 4K-read.
- **Thermal:** single-core emulation load held 1716 MHz at ~59-61 C — no throttling
  (the 1716 cap is a static policy limit, not live throttling). Sustained all-core load
  needs active cooling (`system/fan-curve.sh`).
