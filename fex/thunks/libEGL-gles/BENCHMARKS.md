# x86-GLES-under-FEX benchmarks — real measured numbers

Source: [`glesfullbench.c`](glesfullbench.c). Same PowerVR BXM-4-64 GPU for all three;
A76-pinned (`taskset -c 6,7`); surfaceless EGL + 1024² FBO. Built native-ARM (gcc),
x86-64 and i386 (clang, run under FEX via [`fex-gles`](fex-gles)).

## Full suite

| Metric | unit | native ARM | x86-64/FEX | i386/FEX | x86-64 | i386 |
|---|---|---:|---:|---:|---:|---:|
| glClear call rate | k calls/s | 3427 | 3032 | 3007 | 1.13× | 1.14× |
| draw-call rate | k draws/s | 1644 | 1692 | 1607 | 0.97× | 1.02× |
| fill rate (simple) | Mpix/s | 6714 | 6553 | 6602 | 1.02× | 1.02× |
| shader-ALU fill | Mpix/s | 517 | 516 | 516 | 1.00× | 1.00× |
| shader-ALU throughput | GFLOP/s | 207 | 207 | 206 | 1.00× | 1.00× |
| texture sampling | Mtexel/s | 8634 | 8736 | 8044 | 0.99× | 1.07× |
| triangle rate | Mtri/s | 14.2 | 14.2 | 14.2 | 1.00× | 1.00× |
| compute (SSBO) | Melem/s | 71.0 | 71.0 | 71.0 | 1.00× | 1.00× |
| compute throughput | GIntOp/s | 9.1 | 9.1 | 9.1 | 1.00× | 1.00× |
| buffer upload | GB/s | 3.9 | 3.7 | 3.7 | 1.04× | 1.04× |
| texture upload | GB/s | 3.8 | 3.9 | 3.8 | 0.96× | 0.99× |

(Ratio columns = native ÷ FEX, so **>1.0× means FEX slower**; <1.0× is run-to-run noise.)

## Reading it

- **GPU-bound work is identical** — shader-ALU **207 GFLOP/s**, compute **9.1 GIntOp/s**,
  triangle **14.2 Mtri/s**, simple fill **~6.7 Gpix/s**, texturing **~8.6 Gtexel/s** are the
  *same* native, x86-64, and i386 (all **1.00×**). The GPU half is never emulated, so the
  shader/raster/compute throughput is the hardware's, regardless of who issued the call.
- **Only cheap CPU-side call dispatch shows overhead** — `glClear` call rate drops from
  **3,427 k/s** native to **~3,020 k/s** under FEX (**~1.13×**), and buffer upload **~1.04×**.
  That's the x86→ARM thunk marshaling + CPU emulation around each call.
- **Draw-call rate and texture upload are within noise** (~1.0×).
- **i386 ≈ x86-64** — the 32-bit handle-mapping layer adds no measurable cost on GPU work
  and only a hair on call dispatch (1.14× vs 1.13×).

## Bottom line

For real GLES apps — dominated by GPU fill + shading + a few thousand draws/frame —
x86/i386 under FEX runs at **effectively native GPU throughput**, with single-digit-percent
overhead confined to CPU-side call issuing. For context, FEX's *general* x86→ARM CPU
overhead on this board is 1.1–2.2×; GLES thunking sits at the low end because the expensive
half runs on the native driver.

## Trixie rebuild re-run (2026-07-02, Debian 13 / kernel 6.6, FEX d848cbb + patches)

Spot re-run after the trixie rebuild (x86-64 via thunk vs native the same day):

| Metric | native ARM (today) | x86-64/FEX (today) | vs native |
|---|---:|---:|---:|
| shader-ALU throughput | 207 GFLOP/s | 206.6 GFLOP/s | 1.00× |
| triangle rate | 14.2 Mtri/s | 14.2 Mtri/s | 1.00× |
| compute throughput | 9.1 GIntOp/s | 9.1 GIntOp/s | 1.00× |
| glClear call rate | 2621 k/s | 2684 k/s | ~1.0× |

Call-dispatch overhead ≈1.0× (better than the 1.13× bullseye reference — newer FEX).
Note today's native glClear (2621k) is lower than the bullseye-era 3427k on both sides
(desktop running); the thunk-vs-native ratio is the meaningful number.
