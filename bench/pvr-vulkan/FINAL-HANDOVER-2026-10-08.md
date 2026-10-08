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
   control does.
