# Tile geometry confirmed correct — and the session close-out

## Tile geometry — excluded

Full `pvr_arch_rt_mtile_info_init()`. On the simple-format path (ours,
`simple_internal_parameter_format = true`):

```c
assert(PVR_GET_FEATURE_VALUE(dev_info, simple_parameter_format_version, 0) == 2);
info->mtile_x1 = DIV_ROUND_UP(info->num_tiles_x, 8) * 2;   /* 32 for 128 tiles */
info->mtile_x2 = info->mtile_x3 = 0;
info->x_tile_max = ALIGN_POT(info->num_tiles_x, 2) - 1;    /* 127 */
info->tiles_per_mtile_x = info->mtile_x1 * samples_in_x;   /* 32 */
```

**Coverage = `mtiles_x × tiles_per_mtile_x` = 4 × 32 = 128 = `num_tiles_x`.** Correct; `x_tile_max` matches
the surface; version 2 is asserted at the site. **No over- or under-processing.**

## Final verification of the delivered state

| check | result |
|---|---|
| `bda` | PASS (0 failures) |
| `vk13` | PASS |
| `pctest` | PASS (0 failures) |
| `vk16` | PASS |
| `vkrender` 512 | PASS — 262144/262144 correct |
| `vkrender` 2048 | PASS — 4194304/4194304 correct |
| **`glmark2-es2 --validate`** | **27 scenes validated OK** |
| the fix in tree | `pco_nir.c:85  .max_unroll_iterations = 64,` |
| HEAD | `c2bde57` |
| tree | **37 commits ahead of `main`, clean** |

## Session close-out

**Met:** the bottleneck was found and quantified, a real fix was written, verified and committed, and the
remaining gap was decomposed into measured terms.
**Not met:** the objective's goal of closing the gap.

| term | size | state |
|---|---|---|
| loop not unrolled (>16 iterations) | **1.85–2.85× recovered** | **fixed** (`c2bde57`), correctness green |
| immediate rematerialization | 1.40× | measured (`cstpi` 49.6 vs `cstpin` 73.1) — fixable in PCO |
| register-file moves in the unrolled body | ~2.1× | instruction counts track the measured ratios |
| render per-surface cost, no loop (`vkrender`) | **2.47×** | **open** — every structural candidate excluded |
| per-job sync interface | 84% of frame time in kernel | kernel UAPI needed; Mesa variants unsound |

**The unexplained remainder is bounded to per-tile work in the fragment job that depends on neither
coverage, attachment format, tile geometry, macrotile grid, region-header count, PBE, FBCDC, empty-tile
handling, nor the fragment shader itself.**

**Reaching further needs an instrument this board does not have** — PVRtune, the vendor's command stream, or
a UAPI timing facility. That is the honest boundary of what this method reaches, and the point at which a
new session with a richer toolchain should pick up.
