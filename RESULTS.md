# Community results

Reproduce the harnesses in [`bench/`](bench/) and add a row (PR, or open a
"Benchmark result" issue and a maintainer adds it). Goal: a cross-board baseline so
optimizations and regressions are visible. Please include kernel + DDK version.

## GPU — GLES ALU-loop, Mpix/s (`bench/glbench.c`)
| Board | Kernel | DDK | loop=4 | loop=16 | loop=64 | loop=256 | Notes |
|---|---|---|---|---|---|---|---|
| Radxa Cubie A7A | 5.15.147-21-a733 | 24.2@6603887 | 4198 | 1216 | 315 | 80 | baseline (1280×720) |

## CPU — sysbench events/s (`cpu-max-prime=20000`)
| Board | Kernel | 1-thread | all-cores | Notes |
|---|---|---|---|---|
| Radxa Cubie A7A | 5.15.147-21-a733 | 876 | 3654 | 2×A76@2.0 + 6×A55@1.79 (load-sensitive) |

## FEX x86→ARM overhead (native ARM = 1.0×)
| Board | FEX ver | flags | atomics | x87 | branchy | unaligned-atomics | Notes |
|---|---|---|---|---|---|---|---|
| Radxa Cubie A7A | stock/patched | ~1.4× | ~1.0× | ~1.4× | 1.4–2.2× | **~190× stock → ~2.5× patched** | re-validated 2026-06-13; micro-numbers bench-dependent |

## Memory / storage
| Board | RAM 8-thr read | RAM write | UFS/eMMC seq read | seq write | Notes |
|---|---|---|---|---|---|
| Radxa Cubie A7A | ~17 GB/s | ~10 GB/s | ~1.7 GB/s (UFS, QD8) | 265 MB/s | LPDDR5 4800 MT/s |

> Add your board as a new row. Different SoC revision, kernel, DDK, cooling, or
> governor all move these — that's exactly the data we want to collect.
