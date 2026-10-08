# DOUTU sample_rate FULL vs SELECTIVE - refuted, and the null is informative

## What was found

The driver forces per-sample shading on every multisampled pipeline:

```c
/* pvr_arch_cmd_buffer.c:7349 */
doutu_src.sample_rate = dynamic_state->ms.rasterization_samples > VK_SAMPLE_COUNT_1_BIT
                           ? ROGUE_PDSINST_DOUTU_SAMPLE_RATE_FULL
                           : ROGUE_PDSINST_DOUTU_SAMPLE_RATE_INSTANCE;
```

The PDS enum has three modes - `INSTANCE` (per pixel), `SELECTIVE` (per sample only where needed) and
`FULL` (per sample always) - and the driver never uses `SELECTIVE`. That looked like the 4.87x MSAA
cost.

## Measured: no change

| | baseline (FULL) | with SELECTIVE |
|---|---|---|
| s1 | 303.7 | 299.7 |
| s2 | 164.7 | 162.7 |
| s4 | 62.4 | 62.1 |
| heavy s1 | 9.6 | 9.6 |
| heavy s4 | 3.8 | 3.8 |

Reverted; tree clean; s4 back to 61.9.

## The null is the useful part

The MSAA penalty depends on the shader in the **opposite** way per-sample shading would produce:

| shader | 1 sample | 4x MSAA | MSAA cost |
|---|---|---|---|
| trivial fill (1 instr) | 303.7 | 62.4 | **4.87x** |
| heavy (640 ops) | 9.6 | 3.8 | **2.5x** |

**If the shader ran per sample, the heavy shader would suffer most - it suffers least.** So `FULL` is
not producing per-sample shader execution here, and the MSAA cost is **fixed-function PBE per-sample
work**, not shader invocations. Consistent with bytes-independence, area-scaling, and being worst for
the cheapest shader.

## Consequence

The shader-invocation explanation is now refuted by **two independent measurements** (removing 24
prologue instructions; and this sample-rate mode), so the per-sample cost is downstream of shader
execution entirely - in the PBE's fixed-function per-sample path.
