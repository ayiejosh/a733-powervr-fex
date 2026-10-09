# CORRECTED ANSWER: the deficit is LOOP EXECUTION, not FP32

## The confound, and the control that exposed it

The previous entries concluded "FP32 throughput is 5x lower". **That was confounded.** `cstp` (integer, at
parity) has **no loop**; `cstpf` (float, 5x slower) **has a 32-iteration loop**. The comparison varied
**two** things at once: float-vs-integer AND loop-vs-no-loop.

**Built the missing control: `cstpi`, an INTEGER shader with the same 32-iteration loop structure.**

## The decisive matrix (same SPIR-V on both drivers)

| shader | open | vendor | ratio |
|---|---|---|---|
| `cstp` integer, **NO loop** | 357.8 M inv/s | 378.9 M inv/s | **1.06x** |
| **`cstpi` integer, WITH loop** | **26.6 M inv/s** | **147.2 M inv/s** | **5.53x** |
| **`cstpf` float, WITH loop** | **29.5 M inv/s** | **146.2 M inv/s** | **4.96x** |

## The corrected answer

* **The integer-loop shader is 5.53x slower — the same as the float-loop shader's 4.96x.** The deficit has
  **nothing to do with float**.
* **On the vendor, integer-loop (147.2) and float-loop (146.2) are the same speed.** Float and integer are
  equally fast there; this driver is equally slow at both.
* **Loopless compute is at parity (1.06x).**
* **Therefore: the open driver executes shader LOOPS ~5x slower than the vendor.**

## It explains every measurement in the session

| observation | explained by loop execution |
|---|---|
| `cstp` integer, no loop: 1.06x | no loop |
| `cstpi` integer, loop: 5.53x | loop |
| `cstpf`/`cstpf4` float, loop: 4.96x / 5.75x | loop (4-chain has a longer loop body) |
| `vkheavy` 32-iteration loop: 2.40x | loop, diluted by non-loop overhead |
| fill/format/layout/tiling probes all null | none add a loop |
| `vkmul` "40x" | vendor folded the loop away; PCO did not |
| integer-vs-float spread | **not real — an artefact of my probe shapes** |

## What this changes

**The problem is PCO's loop codegen** — how it compiles a loop body, not the arithmetic inside it. That is
a **narrower and far more tractable target** than "the USC's FP32 rate", and it is entirely inside Mesa
(`src/imagination/pco/`).

**It supersedes the "FP32 throughput" conclusion, the "fragment ALU" framing, and the
`usc_itr_parallel_instances` lead** — all of which rested on the confounded comparison.

## Method note

**This is the fourth probe-artefact caught this session, and the most consequential**: the earlier
conclusions were wrong not because the measurements were noisy but because **my probes varied two
variables at once**. The fix is one control per hypothesis — here, an integer shader with the same loop.
