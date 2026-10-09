# CORRECTION: the "1.27× on the shader-heavy client scene" was noise, not an effect

## What I claimed

When the unroll fix landed I reported:

| | runs | median |
|---|---|---|
| before any fix | 52 / 49 / 44 | 49 |
| after the unroll fix | 62 / 63 / 51 | 62 |

and called it **~1.27× on a real shader-heavy scene**.

## What re-measuring shows

Identical scene, identical settings, with **all four fixes** in place:

| | runs | median |
|---|---|---|
| before any fix | 52 / 49 / 44 | **49** |
| "after unroll" (claimed) | 62 / 63 / 51 | 62 |
| **all four fixes, now** | **58 / 48 / 49** | **49** |

**The current median equals the baseline, and the ranges overlap completely** (44–52 vs 48–63). Background
load during the re-run was heavy (`syncthing` 33.7%, bash 69.5%).

## The conclusion

**The earlier 49 → 62 pair was noise, not an effect.** This scene's run-to-run spread is **44–63 — about
40%** — so a 3-run median difference of 62 vs 49 **with overlapping ranges proves nothing**, and I should not
have reported it as a client-level win. **The claim is withdrawn.**

**This is the same trap the session already documented (25% variance, interleaving mandatory) and I walked
into it anyway** because the two sample sets looked clean.

## What the fixes' evidence actually rests on

**The microbenchmark measurements, which this does not touch.** They use throughput (M invocations/s) and
kernel job timestamps, repeat to **~1%**, and each had a control that behaved as predicted:

- `cstpi` 26.6 → 72.8 M/s, `cstpf` 29.6 → 87.9, `vkheavy` 858.5 → 255.8 ms, **with `cstpin` unchanged where
  the fix cannot apply**
- Each unroll raise came with a probe showing the cliff it removed (128-iteration 7.4 → 19.5; 512-iteration
  1.9 → 5.1)
- Full glmark2 suite 46 → 49 — aggregate over ~30 scenes so far less noisy, **but only 6.5%, so weak evidence
  on its own**

**So: the codegen fixes are well established; their translation into client frame rate is not established at
all.** The honest statement is **"2.7–3.4× on loop-bound microbenchmarks, no demonstrated client FPS
effect"** — not the ~1.27× I claimed.
