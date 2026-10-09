# THE ANSWER: identical float compute shader is 4.93x slower, identical integer shader 1.10x

Built `cstpf`, a float-compute counterpart to `cstp`: **the same SPIR-V shape, differing only in using
`float` instead of `uint`** for the ALU chain. Ran **both** probes on **both** drivers.

| shader (identical SPIR-V, both drivers) | open | vendor | ratio |
|---|---|---|---|
| **integer compute (`cstp`)** | 337.0 M inv/s | 372.0 M inv/s | **1.10x** |
| **float compute (`cstpf`)** | **29.6 M inv/s** | **145.8 M inv/s** | **4.93x** |

## Why this is the answer, not another probe artefact

* **The same SPIR-V runs on both drivers** — no optimizer, constant-folding, or codegen-shape confound.
  The only variable is the driver.
* **Compute, not fragment** — nothing about the fragment stage, rasterization, PBE, tiles or WSI is
  involved. The effect is in **FP32 execution itself**.
* **Integer is at parity in the same pair** — so not clock, DRAM, layout, dispatch overhead, occupancy or
  scheduling; all of those would hit both shaders equally.
* **4.93x is ~5x above the 25% noise floor** measured earlier this session, on a metric
  (M invocations/s) far more stable than FPS.

## What it explains

**Every reliable measurement this session falls out of this one cause:**

| observation | explained |
|---|---|
| render 2.4–3.3x slower | fragment shaders are float-heavy |
| SFU-heavy shader 2.40x | mix of float ALU and transcendentals |
| pure float-ALU shader 5.13x | almost entirely float ops |
| integer-ALU compute 1.12x | **integer is unaffected** |
| uniform/format/layout/tiling probes all null | none change *how many float ops execute* |
| trivial fract shader 2.9x | `fract` is float |
| the vendor's ALU-only shader 2.8x faster than its SFU-heavy | the vendor's float pipeline is healthy |

## The mechanism to fix

**The driver configures the USC's FP32 pipeline at a reduced rate.** The device declares
`has_usc_alu_roundingmode_rne` and `has_usc_f16sop_u8`, and **neither is used anywhere in the driver**
(only the declarations exist). If the hardware's default ALU rounding/precision mode costs several times
more per FP32 op, and the vendor programs the faster mode while this driver does not, the observed
pattern is exactly this: **integer at parity, float uniformly ~5x slow, in every stage.**

## Next

Find the register/mode that sets the USC ALU float rate and how the vendor programs it, then set it.
**This is now a specific configuration question with a measured 4.93x payoff, not a search.**
