# BREAKTHROUGH: fragment ALU is 2.4x slower while COMPUTE ALU is 1.12x

## The measurement

Same probes, size 2048, one frame, per-job kernel durations:

| shader | open driver (2 jobs) | vendor (PV + QV) | ratio |
|---|---|---|---|
| trivial (`vkrender`) | 9.49 + 12.19 = 21.7 ms | 2.02 + 5.38 = 7.4 ms | **2.9x** |
| **heavy, 640 ops (`vkheavy`)** | **427.8 + 430.9 = 858.7 ms** | **177.6 + 179.5 = 357.1 ms** | **2.40x** |

**Both drivers run TWO fragment jobs** (open 427.8/430.9, vendor 177.6/179.5), so the doubling is present
in both and is **not** the difference between them.

## The contradiction that localises this

* **Fragment shader, 640 ALU ops: open is 2.40x slower.**
* **Compute shader (`cstp`): open is 1.12x slower.**

**The same arithmetic throughput is near-parity in a compute shader and 2.4x down in a fragment shader.**
That cannot be clock, memory, or a uniform per-tile cost — it is specific to the **fragment** stage's
shader execution.

## Mechanism: the DOUTU's `usc_temps`

```c
void pvr_pds_setup_doutu(struct pvr_pds_usc_task_control *usc_task_control,
                         uint64_t execution_address,
                         uint32_t usc_temps,        /* temps in 4-dword blocks */
                         uint32_t sample_rate, bool phase_rate_change)
```

**`usc_temps` determines USC occupancy** — more temps per shader instance means fewer concurrent instances,
and ALU throughput falls roughly in proportion.

**Hypothesis: PCO allocates materially more temporaries for the same fragment shader than the vendor's
compiler, cutting fragment occupancy.** It is:

* consistent with the measurement (ALU-bound fragment 2.4x down, ALU-bound compute 1.12x);
* consistent with everything already excluded (clock, DRAM, tiling, geometry would hit compute too);
* **in Mesa's scope** — `src/imagination/pco` register allocation and the `usc_temps` passed to
  `pvr_pds_setup_doutu`;
* **directly measurable**: dump the fragment shader's allocated temps via PCO, read the value the driver
  stores into the DOUTU task control, and compare against what the shader needs.

## Why this is the most actionable finding of the session

* **Excludes** clock, DRAM, per-tile overhead, geometry, tiler, memory layout — all would affect compute
  equally, and compute is at 1.12x.
* **Narrows** the problem to fragment-stage shader execution — a specific code area
  (`pvr_arch_job_render.c` / `pvr_usc.c` / `pvr_pds.c`) rather than a platform mystery.
* **Measurable in one run** with the per-job instrument at 1% repeatability.
