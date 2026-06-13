# Benchmarks — Allwinner A733 / Radxa Cubie A7A

Measured on a Cubie A7A, Debian 11 (bullseye), kernel `5.15.147-21-a733` (not Trixie/6.6), ambient ~27 °C.
Numbers are indicative (single board, methodology noted per section) — not a
controlled suite. Use for orders-of-magnitude, not precise comparison.

## CPU (native ARM64)
SoC: **2× Cortex-A76 @ 2.0 GHz + 6× Cortex-A55 @ 1.79 GHz** (A76 = cpu6,7).

| Test (sysbench, prime ≤ 20000) | Result |
|---|---|
| single-core (A76 @ 2.0 GHz) | **875 events/s** |
| all 8 cores | **3204 events/s** (≈3.66× scaling, heterogeneous) |

## FEX — x86→ARM translation overhead (native ARM = 1.0× baseline)
Per-instruction-class slowdown of x86-under-FEX vs the equivalent native ARM
(microbenchmarks, 2026-06-09). **Most real code lands in the 1.1–2.2× band.**

| Workload class | FEX overhead vs native |
|---|---|
| flag-heavy arithmetic | **1.26×** |
| atomics | **1.08×** |
| x87 floating point | **1.44×** |
| branchy / unpredictable | **2.17×** |
| **unaligned atomics** | **~2.5×** (re-measured 2026-06-13; split-lock crossing 16B ~2.7×) — see note ‡ |

**Practical note:** real-app slowness under FEX (e.g. Chrome) is dominated by
**JIT *compile* time on cold start**, not steady-state translation — an AOT/code
cache is the lever, not per-instruction overhead. No full-application fps figure
was measured; treat the table as instruction-class characterization.

**‡ CORRECTION (2026-06-13):** an earlier note claimed unaligned atomics were **187×**.
Re-measuring on this box with the installed FEX (`bench/uatomic.c`, a `lock xadd` loop)
shows only **~2.5×** vs the aligned equivalent (split-lock ~2.7×), and it's
**config-invariant** (toggling `TSOEnabled` / `KernelUnalignedAtomicBackpatching` /
`HalfBarrierTSOEnabled` / `StrictInProcessSplitLocks` changes nothing) — current FEX
emits unaligned-safe code at JIT time rather than SIGBUS-trapping per op. The 187×
figure was **not reproducible** (likely an older FEX or a no-backpatch measurement).
Reproduce: `x86_64-linux-gnu-gcc -O2 -static bench/uatomic.c -o u && FEXInterpreter ./u 30000000 0|2|14`.

## GPU (PowerVR BXM-4-64, GLES 3.2, DDK 24.2@6603887)
Offscreen FBO, ALU-loop fragment shader, 1280×720, Mpix/s. GPU vs the CPU doing the
same math (OpenMP 8-core, NEON, -O3) and vs the desktop's software (softpipe) path.

| Shader ALU loop | GPU (PowerVR) | CPU 8-core best-case | softpipe (llvmpipe-class) |
|---|---|---|---|
| 4   | **4198** | 28 | — |
| 16  | **1216** | 7 | — |
| 64  | **315** | 2 | — |
| 256 | **80** | 0.46 | — |
| (fill-rate ceiling) | ~4.2 Gpix/s | — | ~0.5 Mpix/s |

→ **GPU ≈ 150–175× the CPU's absolute best case**, and **~600× vs the actual
software (softpipe) fallback** the desktop otherwise uses. `GL_RENDERER =
PowerVR B-Series BXM-4-64`. (Offscreen FBO throughput, not full-app fps; the CPU
baseline is generous — no raster overhead — so the real-world gain is ≥150×.)

## Memory (LPDDR5, 2400 MHz / ~4800 MT/s)
sysbench memory, 1M blocks (optimistic vs STREAM):

| | read | write |
|---|---|---|
| 1 thread | 10.3 GB/s | 8.5 GB/s |
| 8 threads | 15.5 GB/s | 10.0 GB/s |

## Storage (UFS — *not* eMMC)
fio, `direct=1`, on the root device:

| | value |
|---|---|
| sequential read | 1.64 GB/s |
| sequential write | 255 MB/s |
| random 4K read | 115k IOPS |
| random 4K write | 44.8k IOPS |

## Thermal (context)
Passive (fan off): idle ~50 °C; sustained all-core load climbs past 78 °C and keeps
rising → active cooling required under sustained load (see `system/fan-curve.sh`).
