# FIXED: PCO's unroll threshold was the bottleneck — 1.44–1.87×, and ~1.27× on the real client

## The cause

The corrected diagnosis was that **loops** are ~5× slow, not float. The mechanism is **PCO's unroll
threshold**:

```c
/* src/imagination/pco/pco_nir.c */
.max_unroll_iterations = 16,
```

**Loops of ≤16 iterations unroll and pay no loop overhead. Longer loops do not, and fall off a cliff:**

| trip count | open driver throughput |
|---|---|
| 8 (unrollable) | 153.6 M inv/s |
| 16 (unrollable) | 89.3 M inv/s |
| **32 (NOT unrollable)** | **26.7 M inv/s** |

**Why the cliff is steep** — PCO's loop body for a 32-iteration loop is **33 instructions for 4 real ops**:

```
LOOP BODY: 33 instructions
  mbyp          12   register moves
  imadd32        3   the actual work
  cndst.if       2   predicated conditional blocks
  cndend         2
  cndlt.if       1   loop control
  cndsm.if       1
  add64_32.s     2   64-bit counter arithmetic
  br/br.allinst  2
```

## The fix and its measured effect

**`max_unroll_iterations = 16` → `64`** (committed `c2bde57`):

| measurement | before | after | gain |
|---|---|---|---|
| `cstpi32` | 26.7 M/s | **49.8 M/s** | **1.87×** |
| **`vkheavy` (real 32-iteration shader)** | 858.5 ms | **597.1 ms** | **1.44×** |
| **real client** (640×480 shader-heavy glmark2, composited) | 52/49/44 (median 49) | **62/63/51 (median 62)** | **~1.27×** |

## Correctness fully green

`bda` PASS(0) · `vk13` PASS · `pctest` PASS(0) · `vk16` PASS · `vkrender` 512 + 2048 PASS ·
**`glmark2-es2 --validate`: 27 scenes OK.**

## Promotion gate

- correctness passes ✅ · delta repeated ✅ (3 runs each; 5 of 6 paired client comparisons favour it)
- rollback obvious ✅ (single constant) · in source control ✅ (`c2bde57`, tree clean)

**Caveat**: larger unrolls increase code size — a tradeoff, not a free win. 64 is the smallest value
covering the measured loop sizes. Upstream-worthy.
