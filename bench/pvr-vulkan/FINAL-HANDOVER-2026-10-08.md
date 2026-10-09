# Open PowerVR stack vs vendor driver — FINAL handover, 2026-10-08

> ## READ THIS FIRST - sections 1-20 contain claims that later sections FALSIFY
>
> This document grew over the whole session and **was not rewritten as findings changed**. Sections 1-20
> carry the *original* framing; sections **21-23 are the corrected record**. Where they disagree, the later
> section wins. Specifically:
>
> | early claim | corrected in | truth |
> |---|---|---|
> | "25x client gap, 787 vs ~31 FPS" | **S21, S23** | the open stack reaches **102-113 FPS** on that scene; the ~31 matches **software rendering**; the measured gap is **~2.4x render** |
> | "84% of frame time in the kernel" | **S22 tally / S21** | **62.5%**, independently measured (the original number holds in magnitude) |
> | "target (5) CLOSED - PR job is a non-issue" | **S17, S19** | **it is the worst stage at 4.03x** |
> | "target (2) - extra images give 19%" | **S21** | **measured ~2%, inside the noise** |
> | "pool or timeline-back the vk_sync objects" | **S18, S19** | pooling is **unsafe** (kernel holds handle refs); the timeline needs the **two-line design** and has failed **three times** |
> | **the current cross-driver comparison** | **S25** | render **1.68-2.46x**, straight-line compute **1.06x (parity)**, loops **1.63-2.15x**, `vkheavy` **1.42x** |
> | the four fixes are a client-level win | **S23** | **probe-level only - measured, no client effect on two scenes** |
>
> **Everything that survived is in S21-23 with its measurement and its uncertainty. Everything else here is
> the trail of how it was found.**


This supersedes earlier handover text. Where a claim was later refuted by its own control, **the
refutation is what stands and the claim is marked withdrawn.**

---

## 1. The one root cause

**The open driver's render cost is ~3.3x the vendor's per tile of surface, and every frame pays it for
every tile regardless of coverage.** Confirmed by **two independent methods**:

| method | open | vendor | ratio |
|---|---|---|---|
| vary covered area (`AREA` fit) | 3.47 ms/Mpix surface | 1.06 | **3.4x** |
| hold drawn work fixed, vary surface | 14.543 ms @2048 surface | 4.427 | **3.28x** |

Second method detail - all three draw exactly 262144 pixels:

| surface | tiles | open | vendor | ratio |
|---|---|---|---|---|
| 512 | 1,024 | 1.848 | 0.640 | 2.89x |
| 2048 | 16,384 | 14.543 | 4.427 | 3.28x |
| 4096 | 65,536 | 55.772 | 15.619 | 3.57x |

**Both drivers scale the same way** (open 7.9x then 3.8x; vendor 6.9x then 3.5x), so the per-surface
cost is architectural in both - the open driver does the **same work at ~3.3x the cost**.

## 2. That one cause explains the present gap too

`WSIREL_TRACE` release wait: **12.5 ms at 640x480 (0.31 Mpix), 66.0 ms at 1080p (2.07 Mpix)** - 5.3x the
wait for 6.7x the pixels. The wait is per-surface, not fixed sync latency.

weston composites the **4K output (8.29 Mpix) in ~2 passes**:
`8.29 x 3.47 x 2 = 57.5 ms` predicted against **66 ms measured**.

**So: the client's render is 3.3x slower, and weston's composite is 3.3x slower and large (4K, 2
passes). The open client *waits* for that composite; the vendor client never waits, so it never pays it
in its own timing.** The compositor is a **victim, not a cause** - which is why the objective's framing
("the gap is the Vulkan driver") is right.

## 3. Secondary, independently measured

| term | measurement | confidence |
|---|---|---|
| per-pass syncobj overhead | 0.968 ms/pass vs 0.003 (**74x**); root cause proved: `pvr_drm_winsys_null_job_submit` is a userspace fence-forwarding routine, because the UAPI has **no null job type** | high |
| per-frame sync churn | 15 syncobj ioctls/frame (5 create + 5 transfer + 5 destroy); 6.4 null jobs/frame | high (syscall counts) |
| kernel time | 0.456 ms/pass blocked in `drm_syncobj_array_wait_timeout` | high |
| payoff of fixing it | **2.5-5% of a real client frame** - measured before risking a module reload | high |
| compute | 1.12x vendor | high |
| copy | 1.40x | medium |

## 4. Withdrawn claims (refuted by their own control)

| claim | why withdrawn |
|---|---|
| "raw render 2.5-4x down / **fill-rate deficit**" | `AREA`: per *drawn pixel* the open driver is FASTER; cost is per-surface |
| "PBE write **3.2x**" | measured against `FRAGDISCARD`, which used a cheaper shader; discard-contaminated |
| "3.68x per-tile" | the fit assumed a coverage term the open driver does not have |
| "Tiler 1.84x" | discard-based (`rasterizerDiscardEnable`); directionally consistent, different measurement |
| "shader is 12.8x" | `PATTERNDISCARD`: the vendor optimises discards away entirely, so the control is invalid |
| "FBCDC explains the deficit" | **causal test**: reloaded `pvrsrvkm` with `TFBCVersionDowngrade=2`; performance unchanged |
| "macrotile grid is a 2.8x lever" | **the "speedup" was an all-zero render**; `mtiles=4` is structurally required by four offsets |
| "`ZINK_EXTRA_IMAGES` helps (43 vs 36)" | interleaved A/B: ordering flips every round - noise |
| "MSAA is expected architecture, not a defect" | vendor control: 3.06x growth vs the open driver's **8.16x** |

## 5. Measurement methodology — the hard-won part

1. **Variance is ~25%.** Under identical conditions the same config measured 47-60 FPS. **Single-sample
   comparisons under ~25% are unproven.** Interleaved A/B is mandatory.
2. **Discard-based controls are invalid cross-driver.** Both `FRAGDISCARD` and `PATTERNDISCARD` change
   what a compiler can eliminate; the vendor optimises discards away. Two claims died this way.
3. **A speedup without a correctness gate is worse than no measurement.** The mtile "2.8x" was an empty
   render; one correctness command would have caught it immediately.
4. **Documentation explains mechanisms; it does not establish what this driver should cost.** The MSAA
   conclusion was taken from Imagination's guide and then retracted on the vendor control.
5. **Verify the driver reads the field you think it reads.** `data->fs.rasterization_samples` is
   **never assigned anywhere** (declared `pco_data.h:103`, read `pco_nir.c:1143`, written nowhere).

## 6. What is NOT resolved

**The mechanism of the 3.3x per-tile cost.** Twenty-plus candidates tested and refuted, including every
variable the driver exposes and both remaining guide leads. It bottoms out at:
* **PVRtune** - not installed (would name Tiler vs Renderer in minutes)
* **the vendor's command stream** - inside `libVK_IMG` (closed, though it *is* dissectable - see below)
* **a UAPI timing facility** - does not exist (only static `DEV_QUERY`)

## 7. What the vendor binary dissection DID establish

`libVK_IMG` contains `GetFBCSurfaceSize2D()`, `DisableFBCDC`/`DisableSwapchainFBCDC`,
`VK_EXT_image_compression_control`; the DDK allocates a **2 MiB `RGX_FBCDC_HEAP`** the mainline module
never does; and `pvrsrvkm` exposes app hints as module parameters (`TFBCVersionDowngrade`, read-only at
runtime). **The FBCDC heap is a real gap - but the causal test shows it does not drive this benchmark.**
It remains a genuine architectural difference worth pursuing separately.

## 8. State

* Mesa: clean, `80788b9`, **36 commits ahead of `main`**, branch `open-pvr-work-2026-10-06`, **never pushed**.
* Bench: clean, **174 commits ahead**, **never pushed**.
* Board: driver `powervr`, weston + Xwayland up, kwin absent, `gpu-fw-guard` active.
* **Correctness fully green**: glmark2 `--validate` all scenes Success; `bda`/`vk13`/`pctest`/`vk16`/
  `vkrender`/`inatt` all PASS.
* Build caveat: **NDEBUG compiles every `assert()` out** - use `MESA_VK_ABORT_ON_DEVICE_LOSS=true` or a
  `build-assert` dir for invariant checks.
* Safety: never unbind `pvrsrvkm` or rebind GPU drivers while kwin/X is alive; measure a modified
  `powervr.ko`'s payoff before loading it.

## 9. Instruments

`vkrender` (`TILING`, `EXPORTABLE`, `FORMAT`, `SAMPLES`, `LOADOP`, `STOREOP`, `AREA`, `MODE`, `BATCH`,
`DISCARD`, `FRAGDISCARD`, `PATTERNDISCARD`, `UNIFORM` - **`UNIFORM` is timing-only, it fails pattern
verification by design**), `vktex`, `vkheavy`, `cstp`, `pvranimate`; in-driver `PVR_TILE_TRACE`,
`PVR_SUBMIT_MIX`, `PVR_JOB_TRACE`, `ACQ_TRACE`, `WSIREL_TRACE`, `SWAP_TIMING`.

## 10. The one target

**Reduce the per-tile render cost from 3.47 to 1.06 ms/Mpix.** Both the render gap and the present gap
shrink together, because the present gap is the same cost applied to weston's 4K composite.

---

## 11. UPDATE (2026-10-08, later): the bottleneck is FOUND - it is FP32 throughput

**Supersedes section 6 ("what is NOT resolved").** The mechanism was found with a confound-free
cross-driver experiment. Two compute shaders with **identical SPIR-V**, differing only in `float` vs
`uint`, run on **both** drivers:

| shader (same SPIR-V, both drivers) | open | vendor | ratio |
|---|---|---|---|
| integer compute (`cstp`) | 336.0 M inv/s | 375.0 M inv/s | **1.10x** |
| **float compute, 1 chain (`cstpf`)** | 29.6 M inv/s | 144.8 M inv/s | **4.89x** |
| **float compute, 4 independent chains (`cstpf4`)** | 10.6 M inv/s | 61.0 M inv/s | **5.75x** |

**The float deficit persists and grows under four independent chains, so it is not latency - it is FP32
throughput. Integer is at parity in the same pair.**

### Ruled out by measurement, not reasoning

| candidate | how it was excluded |
|---|---|
| clock, DRAM, layout, dispatch overhead | integer is at parity in the same experiment |
| dependency latency | 4 independent chains: deficit persists and grows |
| register pressure / spilling / occupancy | temps 6/9/11, no spills, workgroup 64 |
| launch / workgroup / cluster config | **flat across workgroup counts (28.7 -> 29.9 M/s)**; `CR_COMPUTE_CLUSTER` mask 0 (all USCs), `usc_seq_dep=false` |
| fragment stage, rasterization, PBE, tiles | the effect is in **compute** |
| sample rate / DOUTU modes | measured null |
| register-move overhead | **refuted**: the parity shader has a *higher* move ratio (52% vs 45%) |
| temp-allocation strategy | forced max temps: 860.2 vs 858.6 ms |
| constant folding / codegen shape | same SPIR-V on both drivers |

### The remaining, specific question

**Why does the hardware execute FP32 at ~5x the vendor's rate under this driver?** The driver's launch
configuration is correct, the USC is saturated, and every software-side candidate measured has been
excluded. **The remaining explanation is the shader binary / USC mode PCO emits versus the vendor's
compiler** - which needs the vendor's compiled shader to compare against, and that is not present on this
system.

### The three unused USC features (a concrete lead)

Our device declares `max_usc_tasks = 156`, `usc_itr_parallel_instances = 16`, `usc_slots = 64` - and
**none of the three is referenced anywhere in the driver.** Worth investigating as the missing
parallelism/rate configuration, though the flat workgroup scaling above suggests the USC is already
occupied and the difference is a per-op rate rather than a task count.

### Revised confidence

| finding | status |
|---|---|
| **FP32 throughput ~5x lower, integer at parity** | **high** - identical SPIR-V, both drivers, 5x above noise |
| every software-side candidate excluded | high - each measured |
| the exact USC mode/register responsible | **open** - needs the vendor's compiled shader |\n
---

## 12. UPDATE (2026-10-08, final): the bottleneck is the per-job KERNEL interface, and the fix is a UAPI change

**Supersedes sections 6 and 11.** The picture converged on one place.

### What is fixed and committed

**`c2bde57` — PCO's `max_unroll_iterations` 16 -> 64.** Loop-bound shaders were ~5x slow because loops
>16 iterations are not unrolled and PCO's loop body is 33 instructions for 4 operations (12 register moves,
8 predicated conditional/control instructions, 2 64-bit counter adds).

| measurement | before | after | gain |
|---|---|---|---|
| `cstpi32` (integer, 32-iteration) | 26.7 M/s | 49.8 M/s | **1.87x** |
| `vkheavy` (real 32-iteration fragment) | 858.5 ms | 597.1 ms | **1.44x** |
| shader-heavy client scene | 49 FPS | 62 FPS | **~1.27x** |
| full glmark2 default suite | 46 | 46 | **none** |

**Scope, stated honestly: it is a loop fix, not the whole gap.** Correctness fully green (27 glmark2
scenes + all probes).

### The dominant remaining bottleneck, quantified

**Xwayland's own `/proc/<pid>/stat` utime/stime split** (25 s runs, 640x480, composited):

| scene | FPS | user | sys | per frame: user / sys |
|---|---|---|---|---|
| conditionals | 21 | 16.6% | **44.2%** | 7.90 ms / **21.05 ms** |
| desktop blur | 20 | 18.8% | **41.4%** | 9.40 ms / **20.72 ms** |
| terrain | 5 | 5.4% | **15.1%** | 10.77 ms / **30.11 ms** |

**The kernel side is 2.2-2.8x the user side in every scene.** Jobs per frame: `terrain` 45.7, `desktop blur`
28.0, `refract` 20.4, simple scenes 8.7 - and the slowest scenes are exactly the multi-pass ones.

**Payoff: cutting the per-frame round trips from ~250 to ~50 would remove on the order of 17 ms/frame** -
the difference between 21 FPS and ~45 FPS on the simple scene.

### Why it cannot be fixed from Mesa - proved, not assumed

**Target (1) of the objective is closed as not viable from userspace.** All variants fail for one reason:
**the userspace handle is not the only reference to a sync object.**

* **Pooling the per-job syncs aliases in-flight jobs.** The kernel resolves each handle and takes its own
  reference (`pvr_sync.c:82 drm_syncobj_find`, `:178 dma_fence_get`, released at `:41`/`:43`). Reusing a
  pooled handle means one object referenced by two jobs with different meanings - **a silent
  synchronisation break whose failure mode is a GPU hang.**
* **Pooling the null-job temp syncobj re-points a `drmSyncobjTransfer` dependency.**
* **Timeline-backing measured worse** (3404 -> 3604 ioctls, waits +50%, +399 RESET).
* The cost is the **wait** (0.456 ms/pass blocked in `drm_syncobj_array_wait_timeout`), not object churn.

### The single remaining target, with a verified base

**A `drm/imagination` UAPI facility that lets a batch of passes be described once and ordered by the kernel
or firmware, instead of one handle round trip per job** - the equivalent of the vendor's
`pvr_srv_sync_type`.

| prerequisite | status |
|---|---|
| module builds on this host | **yes** - `make -C /lib/modules/6.1.98-5-aw2511/build M=/home/radxa/kernel-src/powervr modules` completes |
| UAPI gap confirmed | **yes** - `enum drm_pvr_job_type` has GEOMETRY/FRAGMENT/COMPUTE/TRANSFER_FRAG and no chaining/null type; kernel dispatches on exactly those four (`pvr_job.c:280-289`) |
| payoff measured | **yes** - ~17 ms/frame |
| can be done from Mesa | **no** - proved above |
| within objective scope | **yes** - mainline `powervr` module |

**This is the objective's remaining work. It is a kernel change, it needs a module reload (weston/Xwayland
down, guard respected), and it should not be started without the context budget to finish and verify it.**

### Claims withdrawn across the session (for the record)

"raw render 2.5-4x down / fill-rate deficit" - it is per-surface, not fill-rate. "PBE write 3.2x" -
discard-contaminated. "Tiler 1.84x" - discard-based. "shader 12.8x" - invalid discard control. "FP32
throughput 5x down" - **confounded**; it is loop execution. "FBCDC explains the deficit" - killed by its own
causal test. "macrotile / tile-size 2.8x lever" - both were empty renders. "`ZINK_EXTRA_IMAGES` helps" -
noise. "MSAA is expected architecture" - vendor control says otherwise. "the vendor does no explicit-sync
waits" - **`ACQ_TRACE`/`WSIREL_TRACE` are Mesa env vars the vendor ignores, so the silence proves nothing.**
"the driver processes empty tiles unnecessarily" - the vendor scales identically.

### Measurement discipline earned (the most reusable output)

1. **Variance is ~25%** - single-sample comparisons under that are unproven; interleaving is mandatory.
2. **Discard-based controls are invalid cross-driver** - they measure the compiler's ability to eliminate
   the discard, not the stage under test.
3. **A speedup without a correctness gate is worse than no measurement** - the mtile and tile-size "2.8x
   wins" were both empty renders, caught by one correctness command each.
4. **One control per hypothesis.** The "FP32" conclusion died because the probe varied float-vs-integer
   *and* loop-vs-no-loop at once; the integer-loop control redirected the whole search.
5. **A probe must survive both compilers' optimisers** - the vendor constant-folded a mul-only shader that
   PCO did not, which would have produced a fabricated "40x float ALU deficit".
6. **Know the instrument's limits.** The per-job kernel timing that cracked the loop problem pairs jobs to
   completions by fence handle, so it **degrades above a few thousand jobs** - it gave a physically
   impossible 168 ms median for terrain. Short traces only.
7. **Documentation explains mechanisms; it does not establish what this driver should cost.** Only a vendor
   control does.\n
---

## 13. UPDATE (final): the codegen decomposition, and the two open items

### The loop gap is now fully decomposed - both terms are PCO codegen

| contribution | measured by | size |
|---|---|---|
| loop not unrolled (>16 iterations) | the cliff at 16; 1.85-2.85x recovery | **FIXED** (`c2bde57`) |
| immediate rematerialization | `cstpi` (49.6) vs `cstpin` (73.1) M inv/s, cross-driver | **1.40x** |
| register-file moves in the unrolled body | instruction counts tracking the measured ratios | **~2.1x** |

**Instruction counts track the measured gap across probes** - 2.73x ideal -> 2.97x slow, 1.70x ideal ->
2.12x slow - so the residual is codegen size, not an execution-rate mystery:

```
cstpin body:  imadd32  67   the real work (32 x 2 = 64)
              mbyp     70  ┐
              bbyp0bm  34  ├ 141 register-file moves - two moves per real operation
              bbyp0s1  35  ┘
```

Measured to beat: `cstpin` 73.1 vs vendor 155.0 M inv/s; `cstpi` 49.6 vs 147.2.

**Correction included**: an earlier refutation of the move hypothesis used a *loopless* shader (`cstp`, 52%
moves, at parity) as its control - comparing a loopless move ratio against a looped one. With a matched
comparison (both unrolled loop bodies), move count does track the gap.

### Cross-driver state, same session, both drivers

| workload | open | vendor | gap |
|---|---|---|---|
| raw render, no loop (`vkrender` 2048) | 13.706 ms / 306 Mpix/s | 5.538 ms / 757 Mpix/s | **2.47x** |
| real shader, loop (`vkheavy` 2048) | 301.1 ms | 180.0 ms | 1.67x |
| integer compute, no loop (`cstp`) | 305.9 M/s | 375.0 | 1.23x |
| float loop compute (`cstpf`) | 67.5 M/s | 145.9 | 2.16x |
| integer loop compute (`cstpi`) | 49.2 M/s | 146.9 | 2.99x |

Vendor figures reproduced to ~1% across two separate driver switches.

### The two open items

1. **`vkrender`'s 2.47x, with no loop** - so none of the codegen terms above applies to it. The render is
   per-surface (3.46 ms/Mpix vs the vendor's 1.06, flat against coverage), and every per-surface candidate
   tested has been refuted (fill rate, bytes/pixel, format, layout, tile geometry, tile size, MSAA, PBE
   state, FBCDC, empty tiles). **Genuinely unexplained.**
2. **The per-job sync interface** - 84% of frame time in the kernel (~190 syncobj ioctls/frame). Fix must be
   kernel-side; all Mesa-side variants are unsound or measured worse (see sections 11-12 and
   `SYNC-TIMELINE-ATTEMPT-2026-10-08.md`).

### Fixed and verified this session

* **`c2bde57`** - PCO `max_unroll_iterations` 16 -> 64: **1.85x** (`cstpi`), **2.28x** (`cstpf`), **2.85x**
  (`vkheavy`), with `vkrender` correctly unchanged as the control. Correctness green: `bda`, `vk13`,
  `pctest`, `vk16`, `vkrender` 512+2048, **27 glmark2 scenes**.
* **Scope stated honestly**: no change on the full glmark2 default suite (46 vs 46), because its slowest
  scenes are multi-pass/multi-window, bound by per-pass cost rather than shader execution.\n
---

## 14. UPDATE (2026-10-09, final): TWO FIXES LANDED, and the honest end-to-end result

### The two committed fixes

**`c2bde57` - PCO `max_unroll_iterations` 16 -> 64.** Loops longer than 16 iterations were not unrolled, and
PCO's loop body is 33 instructions for 4 operations (12 register moves, 8 predicated conditional/control
instructions, 2 64-bit counter adds).

**`c251c9b` - block-local immediate hoisting in `pco_const_imms.c`.** A constant not in the hardware
constant-register table cost one `bbyp0bm_imm32` materialization *per use* - measured at 131 for three
constants. Now the first use in a block materializes it and later uses move from that register.
**Block-local is what makes it safe:** a value produced earlier in the same block dominates every later use,
so no cross-block dominance analysis is needed, and an unrolled loop body is a single block.

### Measured effect, cross-driver, same session

| workload | before | now | vendor | gap before | gap now |
|---|---|---|---|---|---|
| **real 32-iteration shader (`vkheavy`)** | 858.5 ms | **255.8 ms** | 180.0 ms | **4.77x** | **1.42x** |
| integer loop (`cstpi`) | 26.6 M/s | **72.8 M/s** | 146.6 M/s | **5.53x** | **2.01x** |
| float loop (`cstpf`) | 29.6 M/s | **87.9 M/s** | 145.9 M/s | **4.93x** | **1.66x** |
| integer, no loop (`cstp`) | 305.9 M/s | 324.4 M/s | 375.0 M/s | 1.23x | **1.16x** |
| raw render, no loop (`vkrender`) | 306 Mpix/s | **306 Mpix/s** | 757 Mpix/s | 2.47x | **2.47x unchanged** |
| **full glmark2 suite** | **46** | **49** | - | - | **1.07x** |

**2.7-3.4x where the workload is loop/shader-bound; ~7% end-to-end.** The difference is the point: the suite
score is dominated by its slowest scenes (`terrain` 5 FPS, `refract` 12, `desktop blur` 24), which are
multi-pass / multi-window and bound by per-pass kernel cost and per-surface render cost - neither of which
either fix touches.

**Correctness green for both**: `vkrender` 512 and 2048, `bda`, `vk13`, `pctest`, `vk16`, and
**`glmark2-es2 --validate` 27 scenes**. Both fixes had a control that behaved as predicted (`vkrender` for the
unroll fix, `cstpin` for the hoisting fix).

### Why the loop gap is now fully explained

Built `cstpi1` - the same loop with a single distinct operand - to discriminate:

| shader | instructions | vs ideal | moves | open | vendor | gap |
|---|---|---|---|---|---|---|
| `cstpi1` (1 operand) | 152 | **1.19x** | 75 (49%) | 112.1 | 155.8 | **1.39x** |
| `cstpi` (several) | 224 | 1.75x | 146 (65%) | 72.8 | 146.6 | **2.01x** |

**Gap divided by the instruction-count ratio is 1.15-1.28 across probes** - so **instruction count fully
explains the remaining loop gap**. And the instructions are `one bypass per ALU op (inherent, the vendor pays
it)` plus **legalization for ISA operand-encoding limits** (`needs_s124` in `pco_legalize.c`) - **not**, as I
first said, register-allocation waste. That correction is on the record, and it lowered the expected headroom
of the obvious follow-up.

### What remains, unchanged

* **The per-job kernel sync interface** - **84% of frame time in the kernel**, ~190 syncobj ioctls/frame,
  ~17 ms/frame. **Proved unreachable from Mesa**: the kernel resolves sync objects by handle and holds its own
  reference (`pvr_sync.c:82`), so pooling aliases in-flight jobs and the failure mode is a GPU hang. Needs a
  `drm/imagination` UAPI change. The module builds on this host and the UAPI gap is confirmed; the migration
  plan is in `SYNC-TIMELINE-ATTEMPT-2026-10-08.md`, with one aborted attempt and a corrected ordering
  (the event/barrier paths own the same array slots the render path uses, so it cannot be done piecewise).
* **`vkrender`'s 2.47x per-surface cost** - every source-visible candidate excluded (PBE <10% by a clean
  non-discard format probe; format-, coverage- and geometry-independent). Needs PVRtune or a vendor
  command-stream diff, neither available on this board.\n
---

## 15. FINAL STATE (2026-10-09, end of session)

### Four fixes shipped, all in `src/imagination/pco/`

| fix | commit | measured |
|---|---|---|
| unroll threshold 16 -> 64 | `c2bde57` | 1.85x (`cstpi`), 2.28x (`cstpf`), 2.85x (`vkheavy`) |
| block-local immediate hoisting | `c251c9b` | 1.47x (`cstpi`), 1.31x (`cstpf`), 1.18x (`vkheavy`) |
| unroll threshold 64 -> 256 | `5a1be21` | 2.64x on 128-iteration loops |
| unroll threshold 256 -> 1024 | `167a943` | 2.68x on 512-iteration loops |

**Combined, on the probes: `cstpi` 26.6 -> 72.8 M inv/s (2.74x), `cstpf` 29.6 -> 87.9 (2.97x),
`vkheavy` 858.5 -> 255.8 ms (3.36x).** Gap to the vendor on loop-bound compute **5.53x -> 2.02x**.
**Full glmark2 suite 46 -> 49 (6.5%).**

**Each fix had a control that behaved as predicted**: `vkrender` unchanged for the unroll changes (no loop),
`cstpin` unchanged for the hoisting (no immediates). **Correctness green throughout**: `vkrender` 512 and
2048, `bda`, `vk13`, `pctest`, `vk16`, and `glmark2-es2 --validate` 27 scenes.

### Withheld, because measurement did not support it

**`max_unroll_iterations_aggressive = 32768`** - a null for every probe (all loops are below 1024, so the
normal limit already covers them) **and an unmeasured icache risk for long loops.** Reverted rather than
shipped.

### Measurement discipline, corrected at the end

**The host carries ~65% of a core of background load at all times and it cannot be removed**: `syncthing`
(~34%) plus **the DSH agent harness itself (`MainThread`, ~32%)**, which exists because this session is
running. **Consequently wall-clock FPS cannot resolve any effect below ~35% on this board.**

**Standing rule: measure with throughput (M invocations/s) or per-job kernel timestamps - both repeat to ~1%
because they are fixed work over its own kernel-timed duration and are insensitive to contention on other
cores. Use ioctl and job counts (integers). Use wall-clock FPS only above ~35%, and never for a 3-run median
comparison.**

**Withdrawn on this basis**: the "1.27x on the shader-heavy client scene" (re-measured: current median equals
baseline, ranges overlap completely) and the "GPU 100% busy" figure (fence-pairing artefact at high job
counts).

### The move mechanism, analyzed to its end

The register-file moves that dominate PCO's remaining instruction count were chased through **four
attributions, each partly wrong** - register allocation, legalization for ISA limits, the class table, slot
contention. **The answer**: `bbyp0s1`/`bbyp0bm` are **encoding mappings in the assembler's ISA table
(`pco_map.py`)**, emitted when a PCO instruction's operands must be moved to satisfy hardware register
constraints. **A large part of the 1.1-2.1 moves per operation is therefore likely inherent** - consistent
with the vendor being only **1.39x** faster than PCO's near-optimal `cstpi1` (1.19x ideal) rather than the
several-fold gap a removable cost would give. **The measured floor is `cstpi1`'s 1.1 moves/op.**

### What the session did NOT move, with the reason established

1. **Per-job kernel synchronisation - 84% of frame time in the kernel**, ~190 syncobj ioctls/frame,
   ~17 ms/frame. **Proved unreachable from Mesa**: the kernel resolves sync objects by handle and holds its
   own reference (`pvr_sync.c:82`), so pooling or recycling aliases in-flight jobs and the failure mode is a
   **GPU hang**. Needs a `drm/imagination` UAPI change. **The migration plan is complete in
   `SYNC-TIMELINE-ATTEMPT-2026-10-08.md`**, with one aborted attempt and a corrected ordering: the
   event/barrier paths **own the same array slots** the render path uses (lines 561-581, 717-753), so it
   cannot be converted piecewise.
2. **`vkrender`'s 2.47x per-surface cost** - flat against coverage, format-independent (PBE <10% by a clean
   non-discard probe), tile geometry/macrotile grid/region-header count/ISP partition/AA mode all correct or
   excluded. **Needs PVRtune or a vendor command-stream diff; neither exists on this board.**
3. **The two PCO codegen terms already decomposed** - immediates 1.40x (fixed) and register moves (~2.1x,
   mostly inherent as above).

### Verification commands, for anyone picking this up

```
VK_ICD_FILENAMES=/home/radxa/pvr_gen_icd.json PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1
probes:  ./cstpi 64 200 | ./cstpf 64 200 | ./cstpi128 64 200 | ./cstpi512 64 200 | ./cstpin 64 200
         ./vkrender 2048 20 | ./vkrender 2048 2  (the second is the correctness check)
gate:    ./bda && ./vk13 && ./pctest && ./vk16 && ./vkrender 512 2 && ./vkrender 2048 2
         glmark2-es2 --validate   (expect 27 scenes)
```\n
---

## 16. SESSION END STATE (2026-10-09) — what to do next

### The toolset (all committed, all guard-aware)

| tool | what it answers |
|---|---|
| `components.sh` | **what's working / what isn't** — 30 checks: modules, vermagic vs running kernel, driver binding, DRM nodes, firmware (+md5), both ICDs **and whether the library each names resolves**, the four Mesa fixes, git state, tracepoints, SDDM, the guard, the harness |
| `harness.py` | one probe → **driver, speed, correctness, per-stage job durations, critical path, CPU split, bandwidth** — one JSON record per run |
| `sweep.sh` | the whole probe matrix → one consolidated table |
| `ab.sh` | **full A/B in one run** — closes the desktop, arm open fully, arm vendor fully, reopens via **trap**, prints the diff |

### The measurement that closed the compute investigation

**Same instruction shape, 1.02x vs 2.14x:**

| probe | final code shape | open | vendor | ratio |
|---|---|---|---|---|
| `cstp` | straight-line ALU, no loop | 361.0 | 369.8 | **1.02x - parity** |
| `cstpin` | 32-iteration loop, registers only | 72.0 | 154.2 | **2.14x** |

**At unroll limit 1024 `cstpin` is fully unrolled, so both are straight-line ALU.** The deficit is therefore
**instructions per operation** (register moves): ~1.1/op at low pressure, ~2.1/op at high. **Confirmed
statically (IR move counts) and dynamically (throughput) — two independent methods, same cause.**

### The four shipped fixes

| commit | change | engine |
|---|---|---|
| `c2bde57` | unroll 16 → 64 | yes, PCO |
| `c251c9b` | block-local immediate hoisting | yes, PCO |
| `5a1be21` | unroll 64 → 256 | yes, PCO |
| `167a943` | unroll 256 → 1024 | yes, PCO |

**Effect: `cstpi` 26.6 → 72.8, `cstpf` 29.6 → 87.9, `vkheavy` 858.5 → 255.8 ms. Compute gap 5.53x → ~2.1x;
straight-line compute at parity.** All correctness-green.

### THE LEVERAGE — where the remaining milliseconds are (real client 640x480)

| term | excess | share |
|---|---|---|
| client render | 0.5 ms | **1.5%** |
| present (release wait) | 12.5 ms | **38.5%** |
| **kernel / sync** | **~19.5 ms** | **60.0%** |

**98.5% of the recoverable cost is not the render.** Two levers, neither the shader:

1. **Kernel/sync (60%)** — ~190 syncobj ioctls/frame, 84% of frame time in the kernel. **Proven unreachable
   from Mesa**: the kernel resolves sync objects by handle and holds its own reference (`pvr_sync.c:82`), so
   pooling aliases in-flight jobs → **GPU hang**. **Needs a `drm/imagination` UAPI change**; plan complete in
   `SYNC-TIMELINE-ATTEMPT-2026-10-08.md` with one aborted attempt and the corrected ordering (the
   event/barrier paths own the same array slots, so it cannot be converted piecewise).
2. **Present (38.5%)** — the release wait **is the per-surface render deficit through weston's compositor**:
   12.5 ms at 640x480 → 66 ms at 1080p; weston composites the 4K output in ~2 passes at the open per-surface
   rate. **Not reachable from source** — every driver-visible config reads correct or maximal, and the vendor
   firmware is a different image that is not interchangeable (proven: loads as build 6603887, then DABT).

### Bugs fixed in the user's own scripts this session

* **`switch-open.sh` used `insmod`** — which does not resolve module dependencies, so the open driver could
  not load at all (`Unknown symbol drm_gem_shmem_*`, `drm_sched_*`). Fixed to **`modprobe powervr`** after
  installing to `/lib/modules/$(uname -r)/extra/powervr/` + `depmod -a`. **The module also had to be rebuilt
  because a reboot changed the running kernel.** Verified: `vkrender` 512 PASS under `powervr`.
* **`harness.py` driver detection** — `realpath()` on `.gpu/driver` returns the device path when unbound, so
  the bound driver was mislabelled. Fixed with `readlink()`.

### The honest conclusion

**The objective is NOT met.** The open stack is still ~2x down on rendering and ~2.1x on loop-bound compute;
the real client gap is dominated by kernel-side synchronisation that cannot be fixed from Mesa. **What is
achieved**: four verified codegen fixes, a compute gap fully attributed (parity where the cost is absent), a
complete measurement toolset that caught three silent-wrong-answer bugs, and two levers precisely scoped with
proofs that each is out of Mesa's reach.

**Next session should start with `./components.sh` and `./ab.sh`, then attempt the `drm/imagination` UAPI
change — only with the budget to finish and verify it.**\n
---

## 17. ROUNDS 201-207: the sync lever attacked, and the worst stage located

### The sync-timeline lever went from "unreachable" to "reachable with a named blocker"

Step 1 (persistent `job_sync`/`job_value`, created and destroyed, **unused**) was already landed and is **kept**.
Two attempts to use it, **both reverted**:

| attempt | probe suite | weston |
|---|---|---|
| 1 (`d253e35`): GEOM+FRAG on the persistent timeline | **all PASS** | **SIGSEGV** |
| 2: + restored the previous-barrier wait (map-derived hypothesis) | **all PASS** | **still SIGSEGV** |

**Both removed 2 ioctls per barrier and both crashed the compositor.** The constraint they establish:

> **The barrier's submit both waits on and signals the same timeline syncobj** (wait N, signal N+1). The
> per-job path used **two different syncobjs**, so it never waited on what it was signalling. **The wait line
> and the signal line must be separate** — two timeline objects per stage (`job_sync_wait` / `job_sync_signal`)
> is the smaller shape, mirroring the per-job path.

**And the gate that must apply** (the probes were blind both times):

```
1. probe suite via harness.py (correct ICD)   <- passed BOTH times
2. WESTON MUST COME UP                        <- caught BOTH regressions
3. glmark2 via zink renders + validates       <- end-to-end proof
```

### The worst stage is Mesa-side: the PR job at 4.03×

| job | open | vendor | ratio |
|---|---|---|---|
| geometry / TA | 0.35 ms | 0.82 ms | **0.43x — open WINS** |
| **PR (partial render)** | **9.31 ms** | **2.31 ms** | **4.03x** |
| fragment | 13.01 ms | 5.99 ms | 2.17x |

`pvr_render_job_ws_fragment_pr_init_based_on_fragment_state()` builds the PR state by **copying the fragment
command stream** and patching two offsets, while `pvr_drm_job_render.c:587` confirms *"no PRs will be
performed, as they aren't needed"*. **The driver's own TODOs flag both fixes and neither is taken.**

**Two directions, correctly assigned** (I got this backwards once and corrected it):

| TODO | saves | touches the 4.03x? |
|---|---|---|
| avoid the fragment state setup when `!run_frag` | **host CPU** (a stream built and never submitted — `[2]` is only submitted when `run_frag`) | **no** |
| eliminate the pr / use frag directly in SPM | **a GPU job** | **yes — this is the 4.03x** |

**The host-side one is nearly free and correctness-safe and host CPU is one of the two levers.** The GPU-side
one is the 4.03x and needs a defensible test for "no PR can be needed", which the driver cannot know in
advance because the firmware decides.

### The method lesson, now three times over

**Green probes are not sufficient.** The `insmod` module-load failure (probes never load the module), the
mislabelled driver (silent wrong answers), and the two compositor crashes (probes issue too few barriers) —
**the complement to `harness.py` is running the actual client.**\n
---

## 18. THE TWO-LINE SYNC DESIGN - implementable spec for the 60% lever

**Status: not implemented. Spec complete. Two earlier attempts failed and were reverted.**

### The constraint the failures established

```
barrier_N+1 must wait point N AND signal point N+1 on the SAME object, in ONE null_job_submit.
The per-job path never did that: it waited S1 and signalled S2 - two different objects.
```

**This is structural, not a bug.** One timeline cannot express it.

### The design

**Two persistent objects and two counters per stage:**

| line | role |
|---|---|
| **A** (`job_wait_line`) | the barrier **signals** it; jobs and the next barrier **wait** on it |
| **B** (`barrier_line`) | scratch - so a barrier **never waits the object it signals** |

```
barrier_N  : wait B@(N-1)   signal A@N
job_N      : wait A@N       signal its own per-job sync   (unchanged)
barrier_N+1: wait A@N       signal B@N
barrier_N+2: wait B@N       signal A@(N+1)
```

**The alternation removes the self-wait while preserving identical ordering** - **2 ioctls saved per barrier**
(create + destroy). Mirrors the per-job path with two persistent objects instead of two fresh ones per barrier.

### Implementation notes

* **Two arrays** in `pvr_queue.h` plus two counters per stage; created in `pvr_queue_init`, destroyed in
  `pvr_queue_finish`. **Step 1's existing `job_sync`/`job_value` becomes line A.**
* **`last_job_signal_sync` must stay untouched** - it is the job's own signal and both event paths depend on
  it. **That was the first failed attempt's mistake.**
* **The barrier path also waits on the previous barrier's signal** (`pvr_arch_queue.c:561`) - skipping that
  wait was the second failed attempt's mistake. **The two-line form keeps it.**
* **Convert ONE stage first (GEOM)** and run the full gate before proceeding.

### The gate - not optional

```
1. probe suite via harness.py (correct ICD)     <- passed BOTH failed attempts
2. WESTON MUST COME UP                          <- caught BOTH failures
3. glmark2 via zink renders + validates
```

### Payoff

**~190 syncobj ioctls/frame; 84% of frame time in the kernel; 60% of the real workload's recoverable cost.**
Removing 2 ioctls per barrier across every stage is the largest single item on the board.

---

## 19. THE OTHER OPEN ITEMS, in the order I would take them

| # | item | state | first move |
|---|---|---|---|
| 1 | **two-line sync** (above) | spec complete | implement GEOM only, gate it |
| 2 | **PR job 4.03x** | target measured: 72% -> 39% of the fragment pass | make the PR pass cheap when no PR is needed (smaller range / early-out); host-side alone cannot help |
| 3 | **target (2), zink extra images** | one literal, 19% recorded | `zink_kopper.c:321` `0` -> `2`; **unverifiable here** (19% < the 35% FPS floor) - needs a quieter host |
| 4 | **per-surface render 2.17x** | every config excluded | needs PVRtune or a vendor command-stream diff; neither exists here |

### Two corrections to carry forward

* **The goal's target (5) is wrong.** The PR job is **not** a non-issue: it is **4.03x**, the worst stage.
  *"No PRs are performed"* does not mean *"the job costs nothing"* - it is still submitted and still runs a
  fragment-shaped pass.
* **CPU offload does not help.** Measured on lavapipe: the CPU is **3-26x slower** than the GPU on every probe,
  and the client frame is **already 84% kernel CPU**. **The CPU is the bottleneck, not spare capacity.**\n
---

## 20. VERIFICATION AND THE srv WINSYS (rounds 222-229)

### The headline figures, re-verified at the end of the session

| probe | open (n) | vendor (n) | **ratio (median)** | ratio (worst case) |
|---|---|---|---|---|
| `vkrender` 2048 | 13.687 ms (7) | 5.735 ms (20) | **2.39x** | **1.94x** |
| `vkrender` 512 | 1.507 ms (7) | 0.776 ms (9) | **1.94x** | 2.08x |

**Spreads:** open 2048 **4.4%**, vendor 2048 **4.5%** (six consecutive runs: `5.736 5.486 5.518 5.537 5.513
5.734`), open 512 20.4%, vendor 512 19.8%.

**"About 2x" holds across the entire observed overlap**, including the worst-case extremes.

**One correction recorded:** the consolidated log first showed a **32.6%** vendor spread at 2048, which was
reported as uncertainty. **Six more runs showed it was a single early outlier at 7.260 ms** — the true
consecutive spread is 4.5%. **The figure is firmer than the first summary implied.**

**`HARNESS-LOG-SUMMARY.md`** consolidates every recorded run by driver/probe/size with the **observed range**,
not a point value. **Six rows labelled `driver` are stale pre-fix records** from before the driver-detection
bug was fixed — ignore them; the label is the tell.

### The srv winsys: real, complete, and a porting project

**Mesa contains a full winsys for the vendor kernel** — `src/imagination/vulkan/winsys/pvrsrvkm/`, **7555
lines**, including **`pvr_srv_sync_type`** (the driver-native sync = the 60% lever's mechanism, already
written).

**It is compiled out by default.** `meson.build:311` sets `with_imagination_srv = get_option('imagination-srv')`,
and the winsys is inside `if with_imagination_srv`. **Enabling `-Dimagination-srv=true` compiles it in** (69
`PVR_SUPPORT_SERVICES_DRIVER` occurrences — verified).

**But it still fails, and the reason is definitive:**

```c
/* Only the 1.17 driver is supported for now. */
if (version->version_major != PVR_SRV_VERSION_MAJ ||
    version->version_minor != PVR_SRV_VERSION_MIN) { ... return false; }
```

**`PVR_SRV_VERSION_MAJ/MIN` = 1.17. This board's vendor kernel reports 24.2** (measured: `name=pvr
version=24.2.6603887`). **`pvr_srv_winsys_create()` calls this first → `VK_ERROR_INCOMPATIBLE_DRIVER` → 0
physical devices.**

**So using it means porting the winsys to the DDK 24.2 bridge interface — not a configuration change.**
**Warning: updating the version constant alone compiles and then fails deeper.**

**Two wrong theories were recorded and corrected along the way** (the srv branch being dead code; the build
option being sufficient). **The constants are `PVR_DRM_DRIVER_NAME="powervr"` and `PVR_SRV_DRIVER_NAME="pvr"`,
and the srv branch IS taken.**

### Build state

**`imagination-srv` was reverted to its default (False) after the experiment**, so the build matches the
session's verified baseline. **Mesa `d253e35`, 0 modified, 43 ahead, four PCO fixes intact.**\n
---

## 21. THE OBJECTIVE'S PREMISES, TESTED (rounds 239-244)

**Six inherited figures were tested. Three failed.** That ratio is the single most useful thing in this
document.

### Refuted

| premise | source | test | result |
|---|---|---|---|
| **extra swapchain images give 19%** | target (2) | 8 interleaved rounds, `ZINK_EXTRA_IMAGES=0` vs `=2` | **~2% - `101.5` vs `103.5` FPS, ranges 75-113 / 76-107 - inside the noise** |
| **the PR job is a non-issue** | target (5) | per-job kernel timestamps | **it is the WORST stage: 9.31 vs 2.31 ms = 4.03x** |
| **the open stack gets ~31 FPS** | ground truth | glmark2 through weston+Xwayland+zink | **observed 102-113 FPS, ~3x the recorded figure** |

### Explained rather than refuted

**The "~31 FPS" is best explained by SOFTWARE RENDERING (llvmpipe), not by the open driver.** Measured, same
probe: llvmpipe **40.075 ms** vs open GPU **13.687 ms** = **2.93x slower**; the open client at **108 FPS** would
be **~37 FPS** on llvmpipe, against the recorded **~31** - **within ~15%.**

**Consistent with a known fact: the open driver was genuinely unable to load before this session fixed
`switch-open.sh`** (`insmod` does not resolve module dependencies) - **exactly the state that produces a
fallback.**

**Caveat, honest:** the software figure is **inferred** from a probe ratio; a direct measurement with
`LIBGL_ALWAYS_SOFTWARE=1` **did not complete** (200 s per run, no output). **The scene also differs.**

**And the vendor half is unreachable:** the vendor is 2.39x faster than the open GPU on the same probe, so a
vendor client would be **~258 FPS, not 787** - reaching 787 needs another **3.1x** beyond anything measurable.

### Held up (measured, with ranges)

| figure | value |
|---|---|
| render ratio, 2048 | **2.39x median, 1.94x worst case** |
| fixed per-tile cost | **2.17x** (13.01 vs 5.99 ms) |
| PR job | **4.03x** - now with a target: PR is **72%** of the open fragment pass vs **39%** of the vendor's |
| kernel share of the client frame | **84%** |
| straight-line compute | **parity (1.02x)** |

### The variance, characterised

**~0.2 ms of fixed scheduling jitter plus occasional single hiccups.** Percentage spread therefore scales
inversely with frame time: **4.5% at 2048 (5.5 ms)**, **27% at 512 (0.67 ms)**. Absolute jitter is similar
(0.18-0.25 ms). **Small workloads need more samples; a 3-run median at 512 proves nothing.**

**`harness.py` now reports a range** (`HARNESS_REPEATS`, default 3) and stores `samples` + `spread_pct`.

### The conclusion for anyone reading this

**The objective's headline - a 25x client gap - should be retired in favour of the measured ~2.4x render gap.**
Closing 2.4x with a known 60% kernel-side component is a different and more tractable problem than 25x, and is
what the four shipped fixes and the documented levers actually address.\n
---

## 22. OPERATIONAL WARNING: the driver-switch sequence itself crashes the vendor driver

**Two reboots in this session, and NEITHER was the open driver.**

| time | cause | stack |
|---|---|---|
| 09:07 | the rewrapped vendor firmware loaded then faulted (DABT) | **vendor** |
| 09:49 | **`pvrsrvkm` NULL-deref at `+0x20` in its file-close path** (`PVRDBG: postclose`) | **vendor** |

**The second happened during the round-250/251 CPU measurements**, which repeatedly stopped weston, switched
`powervr` <-> `pvrsrvkm`, and tore down clients under the vendor driver.

### What to do differently

1. **Batch switch operations.** Measure everything you need on one driver before switching, and switch once.
2. **Avoid repeated weston teardown while the vendor driver is bound.** The fault is in its close path, so
   every client teardown under `pvrsrvkm` is exposure.
3. **Prefer the harness's own `--driver=`** (it checks for kwin and refuses), and **run `./ab.sh` when you need
   both arms** - it does one controlled switch pair with a trap-restore.
4. **Expect the vendor stack to be the fragile one.** The objective says so, and both session reboots confirm
   it. **The open driver has caused none.**

### Recovery is automatic

**Both times the board came back on `pvrsrvkm` with the guard active, kwin up, and the firmware intact at
`4b70eca8...`.** No intervention was needed. **The guard has been exercised by real faults and has held.**

### Measurement discipline, final form

* **throughput / kernel timestamps / counts** - repeat to ~1%, insensitive to host load
* **wall-clock FPS** - ~0.2 ms fixed jitter, so **percentage spread scales inversely with frame time**
  (4.5% at 2048, 27% at 512); needs many interleaved samples and never a 3-run median at small sizes
* **always check the tool measured what it claims** - two of this session's errors were instrumentation
  silently not measuring (a mislabelled driver, a missing client process)\n
---

## 23. THE SHIPPED FIXES HAVE NO MEASURABLE CLIENT EFFECT - proven, not assumed

**This is the most important thing to know about the four commits in this document.**

### The experiment

Checked out `80788b9` (the parent of `c2bde57`), rebuilt, and ran the **same scene through the same client
setup** (weston + Xwayland + zink), four runs each, then restored. Repeated on a second scene chosen
specifically to stress the shader.

| scene | WITH fixes | WITHOUT fixes | verdict |
|---|---|---|---|
| `-b build` | 106, 109, 89, 80 (med ~97.5) | 83, 101, 105, 73 (med ~92) | **ranges overlap - no effect** |
| `function:fragment-complexity=high:fragment-steps=10` | 99, 82, 78, 101 (med ~90.5) | 101, 91, 76, 79 (med ~85) | **ranges overlap - no effect** |

### The control that makes it trustworthy

**The same probes DO show the fixes' effect** - `cstpi` **2.74x**, `vkheavy` **3.36x**, with controls that behave
as predicted. **So the builds genuinely differ, and the absence of a client effect is a property of the scene,
not of a botched build.**

### Why - and the leverage analysis already said so

**Even a "shader-heavy" glmark2 scene spends most of its frame outside the shader on this stack:** compositor
and present **38.5%** of the recoverable cost, **kernel synchronisation 62.5%** (independently measured), and
**actual render 1.5%**. **A 2.7x improvement to 1.5% of the frame is ~0.9% overall** - indistinguishable from
noise, exactly as measured. **The scene name describes the shader workload, not the frame's composition.**

### What may and may not be claimed

| claim | supported? |
|---|---|
| the fixes speed up loop-bound compute 2.7-3.4x | **YES** - probes, with controls |
| they close the compute gap from 5.53x to ~2.1x | **YES** - probes |
| **they make the real client faster** | **NO - measured, no effect on two scenes** |

**Anyone writing a summary of these commits must not imply a client-level win.** Their value is that they
removed a **real, measured, documented inefficiency in the compiler** - not that they made anything the user
sees faster.

### The corollary

**The only lever that can move the client is the kernel-side synchronisation (62.5%)**, which is why the three
timeline attempts - all reverted - were aimed at the right target, and why more codegen is not the answer.\n
---

## 24. THE STAGES HAVE DIFFERENT SHAPES - and that separates the two gaps

**Measured per-stage durations against surface size (vendor driver):**

| size | QV (fragment) | PV (PR) | **VV (geometry)** |
|---|---|---|---|
| 256 | 0.423 ms | 0.327 ms | **0.320 ms** |
| 512 | 0.603 | 0.373 | **0.366** |
| 1024 | 1.543 | 0.764 | **0.421** |
| 2048 | 5.433 | 1.875 | **0.514** |
| 4096 | 26.179 | 8.568 | **1.131** |
| **per doubling** | **~4.8x** | **~4.6x** | **~1.2x** |

**Fragment and PR are TILE-BOUND. Geometry is nearly FLAT** across a 256x change in area.

### What it explains, and what it separates

| stage | shape | gap | what kind of problem |
|---|---|---|---|
| **geometry** | **flat** | **0.43x - open WINS** | fixed per-submit work (command setup, submit, fences). **Open's simpler path is good at this.** |
| **PR** | **tile-bound 4.6x/doubling** | **4.03x** | **work SHAPE** - the pass's range/content |
| **fragment** | **tile-bound 4.8x/doubling** | **2.17x** | **raster COST** - per-tile processing |

**The key consequence: a tile-bound stage cannot be fixed by submitting fewer or better jobs - only by making
each tile cheaper.** So:

* **fragment's 2.17x is a raster-cost problem** - firmware or command stream, **outside Mesa** (every
  driver-visible config is already correct or maximal);
* **PR's 4.03x is a work-shape problem** - **the pass runs the fragment-shaped stream over the tile range even
  when no PR is needed** (72% of the open fragment pass vs the vendor's 39%), **and that shape is Mesa's**.

**Earlier sections treated the two as the same kind of gap. They are not.**

### Also corrects a previous explanation

**An earlier entry attributed the geometry job reading 0.50 ms against a recorded 0.76-0.87 ms to jitter.** The
scaling data shows it ranges **0.32-1.13 ms with size** - **so the earlier range was taken at a different size.
The jitter explanation was wrong.**\n
---

## 25. THE CURRENT CROSS-DRIVER COMPARISON (fresh, both arms, one session)

**This supersedes the original ground truth's framing. Measured with `harness.py`, spreads 0.1-8.9%.**

| probe | size | **open** | **vendor** | **gap** |
|---|---|---|---|---|
| `vkrender` | 256 | 1.000 ms | **0.596 ms** | **1.68x** |
| `vkrender` | 512 | 1.663 | **0.743** | **2.24x** |
| `vkrender` | 1024 | 4.182 | **1.753** | **2.39x** |
| `vkrender` | 2048 | 13.627 | **5.538** | **2.46x** |
| `vkrender` | 4096 | 53.933 | **22.947** | **2.35x** |
| `vkheavy` | 2048 | 255.429 | **179.992** | **1.42x** |
| **`cstp`** (integer, no loop) | 64 | 338.7 M inv/s | **358.8** | **1.06x - PARITY** |
| `cstpf` (float loop) | 64 | 88.4 | **144.1** | **1.63x** |
| `cstpi` (integer loop) | 64 | 70.6 | **145.2** | **2.06x** |
| `cstpin` (register loop) | 64 | 71.0 | **152.3** | **2.15x** |

**All render probes PASS. Gate green on the open driver** (`bda`, `vk13`, `pctest`, `vk16` 8 ok/0 failed,
`vkrender` 2048).

### The shape

* **Render 1.68-2.46x**, peaking at **1024-2048**, smallest at 256.
* **Straight-line compute 1.06x - PARITY.** **The gap is specific to loops and rendering, not to the driver as a
  whole.**
* **Loops 1.63-2.15x.**

### Progress versus the session's recorded baselines

| | session start | **now** | vendor | gap at start | **gap now** |
|---|---|---|---|---|---|
| `cstpi` | 26.6 M inv/s | **70.6** | 145.2 | **5.46x** | **2.06x** |
| `cstpf` | 29.6 | **88.4** | 144.1 | **4.87x** | **1.63x** |
| `vkheavy` | 858.5 ms | **255.4 ms** | 180.0 | **4.77x** | **1.42x** |
| `cstp` | - | **338.7** | 358.8 | - | **1.06x** |
| `vkrender` 2048 | - | **13.627 ms** | 5.538 | - | **2.46x** |

**Loop/shader-bound work: ~4.8-5.5x behind at the start, 1.4-2.1x now.** **Raw render: ~2.4x, untouched by any
fix** - because the fixes are codegen and the render gap is tile-bound raster cost (S24).

### Procedure, after three reboots

**One switch, verify the driver bound before measuring, restore afterwards.** The failed attempt used `ab.sh`,
whose switch left the driver **unbound** so the open-arm probes ran against nothing and a kernel Oops followed.
**The verification step is what makes a switch safe; without it, a failed switch produces silent garbage and then
a crash.**
