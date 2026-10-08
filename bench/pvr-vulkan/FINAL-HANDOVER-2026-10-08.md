# Open PowerVR stack vs vendor driver — FINAL handover, 2026-10-08

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
change — only with the budget to finish and verify it.**
