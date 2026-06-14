# Benchmark workflow — every perf-affecting change must show its delta

**Rule:** any commit that could change performance (a FEX patch, a GPU/driver change,
a config tweak, a kernel-module change) must be **verified with `bench/run.sh` before
and after**, and the **before→after diff goes in the commit message**. Update
`bench/baseline.txt` only when a change is verified to move a number on purpose.

## Do this per change
```sh
# 1. make your change, rebuild/redeploy, then:
bench/compare.sh            # runs the suite + prints a human-readable diff vs the
                            # committed baseline (git main), with a SIGNAL/ABSOLUTES
                            # split, Δ%, ▲ better / ▼ worse / ≈ noise, and a verdict.
# 2. paste that output into the commit body.
# 3. if it's a real, intended improvement, refresh the baseline:
bench/run.sh > bench/baseline.txt
git commit -am "<what changed> + benchmark comparison (see body)"
```
`bench/compare.sh` resolves the baseline as: `origin/main` → local `main` → `HEAD` →
`bench/baseline.txt` (whichever it finds first); override with `BASELINE=path`.

## What to trust in the diff
- **Compare the `ratio.*` lines and the `gpu.*` lines.** These are the reliable
  change-signals: the ratios are A/B comparisons run back-to-back under identical
  load (stock-vs-patched FEX, X87 RP on/off), and GPU throughput is CPU-load-independent.
- **The raw absolutes** (`cpu.1thread`, `fex.tcreate.nsop`, `fex.atomic.*.Mops`,
  `*.STOCK.*`) are **load-sensitive ±~20%** on this always-on box (it runs services
  in the background). A 20% wobble there is noise, **not** a regression. Only treat an
  absolute as changed if it moves *and* the corresponding ratio moves with it.

## Current baseline (`bench/baseline.txt`, 2026-06-13)
Headline ratios: unaligned-atomic patch **~220×**, thread-pooling **~6×**,
X87ReducedPrecision **~19×**; GPU 4136/1210/315/80 Mpix/s. Run `bench/run.sh --hw`
to also re-check CPU-all-core / RAM / UFS (those rarely change with software).

> Pin to the A76 cores (the runner does, via `taskset -c 6,7`) and ideally run when
> the box is otherwise idle for the cleanest absolutes.
