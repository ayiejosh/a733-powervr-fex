# Fresh cross-driver measurement: the unroll fix took 1.85–2.85× off every looped workload

Both drivers measured in the same session on the same probe suite. Vendor: `pvrsrvkm` + `libVK_IMG`.
Open: `powervr` + Mesa with the unroll fix (`c2bde57`) in place.

| probe | open (with fix) | vendor | ratio | before the fix |
|---|---|---|---|---|
| `vkrender` 2048 (no loop) | 13.706 ms / 306.0 Mpix/s | 5.538 ms / 757.3 Mpix/s | **2.47×** | ~13.8 — **unchanged, as expected** |
| `vkheavy` 2048 (32-iteration loop) | **301.1 ms** | 180.0 ms | **1.67×** | 858.5 ms → **2.85× faster** |
| `cstp` (integer, no loop) | 305.9 M inv/s | 375.0 | 1.23× | — |
| `cstpf` (float, 32-iteration loop) | **67.5 M inv/s** | 145.9 | **2.16×** | 29.6 → **2.28× faster** |
| `cstpi` (integer, 32-iteration loop) | **49.2 M inv/s** | 146.9 | **2.99×** | 26.6 → **1.85× faster** |

## What this settles

* **The unroll fix is real and substantial**: 1.85× (`cstpi`), 2.28× (`cstpf`) and **2.85× on the real shader
  `vkheavy`** — while `vkrender`, which has no loop, is **correctly unchanged** at ~306 Mpix/s.
* **It cut the loop deficit roughly in half**: `cstpi` from **5.53×** behind the vendor to **2.99×**;
  `cstpf` from **4.93×** to **2.16×**.
* **A residual 2–3× gap remains on exactly the same workloads.** So the unroll threshold was *a* cause of
  the loop deficit, **not the only one**.
* The vendor figures reproduced exactly across two separate switches (`vkrender` 5.538/5.6, `vkheavy`
  180.0/177.6, `cstp` 375.0/372.0, `cstpf` 145.9/146.2, `cstpi` 146.9/147.2), so **the vendor baseline is
  stable to ~1%** and these ratios are trustworthy.

## The current picture of the gap

| workload | gap to vendor |
|---|---|
| raw render, no loop (`vkrender`) | **2.47×** |
| real shader, with a loop (`vkheavy`) | **1.67×** |
| float loop compute (`cstpf`) | 2.16× |
| integer loop compute (`cstpi`) | 2.99× |
| integer compute, no loop (`cstp`) | 1.23× |

**The gap is no longer one thing**: a no-loop render is 2.47× down while looped compute is 2.16–2.99× and
integer no-loop compute is 1.23×. **That argues against any single remaining cause and for at least two
independent contributions** — consistent with the two separate bottlenecks already identified:

1. the **per-job sync interface** (~190 ioctls/frame, 84% of frame time in the kernel), and
2. whatever **remains of the loop body after unrolling**.

## Verification note

The conclusion rests on a *before/after pair measured with the same instrument under the same conditions*,
with the no-loop probe (`vkrender`) as an internal control confirming the fix did not change what it should
not have. **The control behaving as predicted is what makes the 2.85× on `vkheavy` credible.**
