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
