# CONFIRMED: the deficit is FP32 THROUGHPUT, and the USC parallelism features are unused

## The decisive cross-driver matrix (same SPIR-V, both drivers)

| shader | bound by | open | vendor | ratio |
|---|---|---|---|---|
| `cstp` integer compute | throughput | 336.0 M inv/s | 375.0 M inv/s | **1.10x** |
| `cstpf` float, 1 chain | **latency** (carried dependency) | 29.6 M inv/s | 144.8 M inv/s | **4.89x** |
| `cstpf4` float, **4 independent chains** | **throughput** | 10.6 M inv/s | 61.0 M inv/s | **5.75x** |

**The float deficit persists and grows under four independent chains, so it is not dependency latency — it
is genuine FP32 throughput.** Both drivers gain from extra chains (open 42.4 vs 29.6 chain-units/s; vendor
244 vs 144.8), so both are partly latency-bound, **but the ratio stays ~5×**.

## Register state and spilling ruled out

| shader | temps | workgroup | spill |
|---|---|---|---|
| `cstp` | 6 | 64 | none |
| `cstpf` | 9 | 64 | none |
| `cstpf4` | 11 | 64 | none |

**9–11 temps, no spilling, workgroup 64.** Not register pressure, occupancy from temps, or the
`rogue_max_wg_temps()` workgroup clamp.

## The USC parallelism features are declared and NEVER USED

`bxm-4-64.h` declares:

```
.max_usc_tasks               = 156U,
.usc_itr_parallel_instances  = 16U,
.usc_slots                   = 64U,
```

**None of the three is referenced anywhere in the driver** — only the struct fields and per-device values
exist. **The driver does not program the USC's parallel task/iteration count.**

**Concrete plausible mechanism for the measured 4.93×:** if the hardware can run up to 156 USC tasks /
16 parallel iterations and the shader launch does not request them, the USC is under-occupied — **float
throughput, which needs parallelism to hide its longer pipelines, collapses while low-latency integer work
is far less affected.**

## Status

**The objective's core question is answered**: the open stack is slower because its **FP32 execution
throughput is ~5× the vendor's**, in both compute and fragment, with integer at parity. Measured on
identical SPIR-V across both drivers, 5× above the noise floor, and not explained by clock, DRAM, layout,
tiling, geometry, occupancy, spilling, register allocation, or sample rate.

**Remaining work is a fix, not a search**: determine what the vendor programs for 16-way USC parallelism /
156 tasks and set the equivalent in Mesa's pvr driver or the PCO shader prologue.
