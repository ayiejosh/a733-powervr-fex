# BREAKTHROUGH: the vendor has a uniform-colour PBE fast path the open driver lacks

The objective deprioritised FBCDC by reasoning ("the available source has no FBD structure or
compression-stream allocation"). **That reasoning covered only the open driver's source. It never tested
what the vendor does.** Added `UNIFORM=1` (a shader writing a constant colour) and compared it with the
pattern shader and with `FRAGDISCARD` (shader runs, nothing reaches the PBE):

| 2048x2048, s1 | pattern | uniform | FRAGDISCARD | **PBE (pattern)** | **PBE (uniform)** |
|---|---|---|---|---|---|
| open | 14.160 ms | 12.161 ms | 8.214 ms | **6.08 ms** | **3.95 ms** |
| vendor | 5.902 ms | **4.003 ms** | 3.963 ms | **1.94 ms** | **0.04 ms** |

## What this shows

* **The vendor's uniform fill costs the same as rendering no fragments at all** (4.003 vs 3.963 ms) -
  PBE cost **0.04 ms**. Its pattern fill costs 1.94 ms. The vendor **collapses the cost of writing
  compressible data to essentially zero** - frame-buffer compression or an equivalent fast path.
* **The open driver improves only 35% for uniform data** (6.08 -> 3.95) against the vendor's **98%**
  (1.94 -> 0.04). It has *some* data-dependent behaviour, but nothing like the vendor's.
* **The open driver's uniform PBE (3.95 ms) is ~100x the vendor's (0.04 ms).**

## Fits everything already measured

* **Bytes/pixel flat** (r8 -> rgba8 -> rg16): compression is about *compressibility*, not width.
* **Format-independent**: same.
* **Load/store ops irrelevant**: the cost is in the PBE's per-pixel processing before any store.
* **Layout-independent** (linear vs optimal, +1%).
* **"Firmware boundary"**: correct - but the boundary is not opaque, it is a **missing feature**.

## Correction to the objective's target (4)

**FBCDC was deprioritised on evidence covering only the open driver's source.** This measurement shows
the vendor has a render-target compression fast path the open driver does not implement, and it is the
dominant term in the 3.2x PBE-write deficit. **This is now a concrete, named feature gap** - and it
cannot be implemented from the available mainline UAPI (no FBD allocation ioctl), so it remains blocked,
**but for a known and correct reason rather than an assumed one.**

## Status change

**PBE write (3.2x) is no longer "mechanism unknown".** It is: *the open driver lacks the vendor's
uniform-colour/compressed PBE fast path, unavailable without FBDC support in the mainline UAPI.* A
materially better statement than before this round.

## Caveat

`UNIFORM=1` reports `VERDICT: FAIL` because the verifier checks for the `fract()` **pattern**, which a
constant fill deliberately does not produce. Frame time is still reported, so the measurement is valid -
but `UNIFORM=1` is a **timing variant only**, not a correctness check.
