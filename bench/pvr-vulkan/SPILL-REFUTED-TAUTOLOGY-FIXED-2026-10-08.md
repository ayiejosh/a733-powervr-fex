# A real bug found and fixed, and the spill hypothesis refuted

## A genuine bug: the only `true ||` tautology in the driver

```c
/* pvr_arch_pipeline.c:2673 */
if (true || data->common.spilled_temps) {
   data->common.spill_info = (pco_range){ .start = data->common.shareds, .count = 3 };
   data->common.shareds += 3;
}
```

**Every shader unconditionally reserves 3 shared dwords for spill info**, even shaders that never spill.
It is the only such tautology in `src/imagination/` (checked), and the `spilled_temps` guard was evidently
meant to gate it. **Fixed** (`if (data->common.spilled_temps)`), rebuilt, verified.

**Measured effect: none.** Per-job durations 0.42 / 9.44 / 12.19 ms vs 0.46 / 9.49 / 12.19 baseline; frame
14.194 vs ~13.8 ms; **2048 correctness PASS (4194304/4194304)**. Three shared dwords is too small to move
anything. **Kept as a correctness/cleanliness fix** — it stops a shader that never spills from consuming
shared memory the code's intent says it should not — and recorded as having no measured performance gain.

## The spill hypothesis is refuted

PCO reports its own allocation:

| shader | `temps` |
|---|---|
| vkrender fragment | **8** |
| vkheavy fragment (640 ops) | **18** |

**18 temp registers for a 640-op shader** — PCO reuses registers well (the loop structurises), there is no
spilling, and no occupancy loss from temps. So the "fragment ALU 2.4x slower" measurement is **not** caused
by register pressure or spilling.

Noted but not explanatory: `pco_ra.c:1178` adjusts `max_temps` for workgroup size on the **compute** path
only; the fragment path uses the raw device maximum — that *constrains compute*, so it cannot explain
fragment being slower.

## Status of the fragment-ALU finding

The measurement stands (**fragment 640-op 2.40x slower, compute 1.12x**), and two candidate mechanisms are
now excluded (spilling; the spill_info tautology). Remaining: the **USC task configuration for the
fragment stage** — `pvr_pds_setup_doutu()` receives `usc_temps`, `sample_rate`, `phase_rate_change`, and
the fragment path's values versus compute's are the next thing to read and compare.
