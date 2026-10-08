# The move-overhead hypothesis is REFUTED by its own control

The previous entry concluded register-move traffic (42% of the loop body, ~2.7× the ideal instruction
count) explained the 2.40× float deficit. **Tested against a control: the integer compute shader `cstp`,
which is at parity with the vendor (1.12×).**

| shader | instructions | moves | move % | measured deficit |
|---|---|---|---|---|
| **`cstp` (integer compute, PARITY)** | **29** | **15** | **52%** | **1.12×** |
| `vkheavy` (float) | 110 | 50 | 45% | 2.40× |

**The shader at parity has a HIGHER move ratio than the slow one** — so move traffic does not explain the
deficit, and the previous conclusion is withdrawn.

Caveat on the control: `cstp` is short and loop-free, so its ratio is prologue-dominated and the two
ratios are not measuring the same thing. **But the refutation stands: "more moves per instruction means
proportionally slower" is false on this driver.**

## What this re-points at

**Float-heavy shaders are 2.40–5.13× slower while integer-ALU compute is at parity**, and it is not:

| excluded | how |
|---|---|
| instruction count per se | parity shader has a comparable ratio |
| register moves | **refuted here** |
| temp allocation strategy | refuted (forced max temps: 860.2 vs 858.6 ms) |
| spilling / occupancy from temps | refuted (8 and 18 temps) |
| sample-rate mode | refuted earlier |
| clock, DRAM, tile geometry, tiler, layout | would hit compute, which is at parity |

**What remains is float instruction throughput itself** — the rate at which the USC executes FP32 ops under
this driver's configuration. The device exposes `usc_f16sop_u8` and `usc_alu_roundingmode_rne`; **if the
driver configures the USC for a reduced-rate FP32 mode while emitting full FP32 instructions, the result
is exactly this pattern: integer at parity, float uniformly slow, scaling with the number of float ops** —
which matches 2.40× on the SFU-heavy shader and 5.13× on the pure float-ALU shader.
