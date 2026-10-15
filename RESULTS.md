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
| Radxa Cubie A7A | 6.6.98-5-aw2511 | 24.2@6603887 | **7392** | **2229** | **579** | **147** | trixie, GPU **1104 MHz** + DSU 1027 MHz (`overlays/`), 1280×720; a second run read 7644/2227/578/147 |

> The same board at the driver's default **600 MHz** clock read 4186/1215/315/80 — the
> GPU clock, not the driver, was the difference. See
> [`docs/PERFORMANCE-2026-09-22.md`](docs/PERFORMANCE-2026-09-22.md).

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
| Radxa Cubie A7A | 6.6.x-aw2511 | glmark2-es2 `--off-screen`, zink | **661** | June 2026 measurement, GPU at 600 MHz; system Mesa 25.0.7 zink; GLES2/GL2.1 ceiling; windowed/desktop GL does NOT work |
| Radxa Cubie A7A | 6.6.98-5-aw2511 | glmark2-es2 `--off-screen`, zink | 454 → **581** | 2026-09-22: 454 at the stock 600 MHz, 581 at **1104 MHz** (`overlays/gpu-clk.dts`) |
| Radxa Cubie A7A | 6.6.98-5-aw2511 | glmark2-es2 `--off-screen`, vendor GLES | 659 → **826** | 2026-09-22, `LD_LIBRARY_PATH=/usr/local/lib`, same 600 → 1104 MHz step |

> glmark2 is **CPU/driver-bound** on this board: moving the GPU 1008 → 1104 MHz changed
> nothing measurable there (vendor 816–829, zink 573–588 across runs); use
> `bench/glbench.c` for a GPU-throughput signal, which does track it (+77–84%).
>
> Note the 454 above is *below* June's 661 for zink **at the same 600 MHz** — that gap
> predates the clock work (447–469 across pre-overlay runs on 2026-09-22) and is a
> userspace difference, not the clock; the clock overlays do not touch zink's shader path.

## Memory / storage
| Board | RAM 8-thr read | RAM write | UFS/eMMC seq read | seq write | Notes |
|---|---|---|---|---|---|
| Radxa Cubie A7A | ~17 GB/s | ~10 GB/s | ~1.7 GB/s (UFS, QD8) | 265 MB/s | bullseye measurement; LPDDR5 4800 MT/s |

> Add your board as a new row. Different SoC revision, OS/kernel, DDK, cooling, or
> governor all move these — that's exactly the data we want to collect.

## GPU — open Mesa `pvr` + zink vs vendor GLES, glmark2-es2 off-screen (trixie)
Open = mainline `powervr` + Mesa main with `mesa/open-driver-series` 0001–0105, GL through zink;
vendor = DDK 24.2 native GLES. 800×600 off-screen, GPU 1104 MHz, pinned to cores 6-7.

| Board | Kernel | open 2026-10-10 | open 2026-10-11 | open 2026-10-14 | vendor | Notes |
|---|---|---|---|---|---|---|
| Radxa Cubie A7A | 6.6.98-5-aw2511 | 233 | 627 | **688** | 930 | 2026-10-14: two runs 690 / 687, per-scene geomean vs vendor 0.72, texture 84–95 %, gouraud 87 %, phong 72 %, effect2d 62–68 %, desktop 39–46 %. 2026-10-11, per-scene: build 86 %, texture 85–95 %, gouraud 87 %, phong 65 %, effect2d 50 %, desktop 30 % of vendor |

## GPU — open Mesa `pvr` vs vendor `pvrsrvkm`, native Vulkan (`bench/pvr-vulkan`, trixie)
3 alternating open/vendor rounds, medians, pinned to cores 6-7, `performance` governor.

| workload | open | vendor |
|---|---|---|
| vkrender 512 / 2048 render, ms/frame | 0.358 / **1.766** | 0.358 / 1.969 |
| vkrender 512 / 2048 render+copy, ms/frame | 0.637 / 5.358 | 0.608 / 5.321 |
| vkrender 512 / 2048 copy, ms/frame | 0.237 / 2.530 | 0.242 / 2.529 |
| vkheavy 2048, ms/frame | **171.7** | 179.8 |
| cstp / cstpf / cstpi, M inv/s | **447.9** / 148.6 / 151.6 | 372.5 / 146.2 / 147.1 |
| vkmany 512 / 2048, ms/frame | 0.817 / 5.729 | 0.801 / 5.615 |
