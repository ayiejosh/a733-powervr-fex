# Community results

Reproduce the harnesses in [`bench/`](bench/) and add a row (PR, or open a
"Benchmark result" issue and a maintainer adds it). Goal: a cross-board baseline so
optimizations and regressions are visible. Please include **OS, kernel + DDK version**
— the `bullseye` (Debian 11 / 5.15) and `trixie` (Debian 13 / 6.6) lines are different
stacks and their numbers are not directly comparable.

## GPU — GLES ALU-loop, Mpix/s (`bench/glbench.c`)
| Board | Kernel | DDK | loop=4 | loop=16 | loop=64 | loop=256 | Notes |
|---|---|---|---|---|---|---|---|
| Radxa Cubie A7A | 5.15.147-21-a733 | 24.2@6603887 | 4198 | 1216 | 315 | 80 | bullseye baseline (1280×720) |

## CPU — sysbench events/s (`cpu-max-prime=20000`)
| Board | Kernel | 1-thread | all-cores | Notes |
|---|---|---|---|---|
| Radxa Cubie A7A | 5.15.147-21-a733 | 876 | 3654 | bullseye; 2×A76 + 6×A55 (load-sensitive) |

## CPU — emulated x86-64 single-thread, sum-ms CPU bench (trixie, big core)
Lower = faster. `bench/cpubench.c` cross-built static x86-64, pinned to a **big core**
(cores 6-7); native = the same source built arm64. (Sum of per-section ms; this metric
is *not* the sysbench events/s above.)

| Board | Kernel | native | box64 0.4.3 tuned | FEX tuned | Notes |
|---|---|---|---|---|---|
| Radxa Cubie A7A | 6.6.x-aw2511 | 5488 | 8564 (~64%) | 7746 (~71%) | box64 `CALLRET=1 SAFEFLAGS=0`; FEX `TSOEnabled=0 Multiblock=1` |

> Core placement dominates: the A733 is heterogeneous big.LITTLE — the same x86 work is
> ~2.3–6.5× faster on a BIG core (6-7) than a LITTLE core (0-5). Don't hard-pin; the
> capacity-aware scheduler puts hot work on the big cores. Report which core if you pin.

## GPU — Direct3D 11 via DXVK-Sarek → PowerVR Vulkan (trixie)
Native arm64ec DXVK-Sarek (FL 11_0). Headless RTT and windowed present, indicative.

| Board | Kernel | metric | value | Notes |
|---|---|---|---|---|
| Radxa Cubie A7A | 6.6.x-aw2511 | instancing | ~370k tris/s | |
| Radxa Cubie A7A | 6.6.x-aw2511 | windowed present (trivial scene) | ~227 fps | software-llvmpipe window present ~1.9 ms/frame |
| Radxa Cubie A7A | 6.6.x-aw2511 | draw-call ceiling (distinct PSO) | ~1000–1100 draws/s | PSO-swap-bound; trivial no-state draws are far cheaper (CPU/futex, not driver ioctl) |

> A *realistic* textured + depth-tested frame (hundreds–thousands of draws) is
> **GPU-fill-bound** (PowerVR BXM-4-64 raster/fragment ceiling), not CPU/submission
> bound — GPU-finish scales linearly with triangle count and dominates CPU-record by 4–6×.

## GPU — OpenGL via zink → PowerVR Vulkan, off-screen (trixie)
| Board | Kernel | bench | score | Notes |
|---|---|---|---|---|
| Radxa Cubie A7A | 6.6.x-aw2511 | glmark2-es2 `--off-screen` | **661** | system Mesa 25.0.7 zink; GLES2/GL2.1 ceiling; windowed/desktop GL does NOT work |

## Memory / storage
| Board | RAM 8-thr read | RAM write | UFS/eMMC seq read | seq write | Notes |
|---|---|---|---|---|---|
| Radxa Cubie A7A | ~17 GB/s | ~10 GB/s | ~1.7 GB/s (UFS, QD8) | 265 MB/s | bullseye measurement; LPDDR5 4800 MT/s |

> Add your board as a new row. Different SoC revision, OS/kernel, DDK, cooling, or
> governor all move these — that's exactly the data we want to collect.
