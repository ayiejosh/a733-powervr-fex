# Open PowerVR stack vs vendor driver — session handover, 2026-10-08

Everything below is **measured against a vendor control**. Nothing is inferred from the open stack alone.
Where a conclusion was later refuted by its own control, both the claim and the refutation are kept.

## 1. The setup that made the work possible

The vendor userspace **exists** and was the single most important discovery of the session:

* `/usr/lib/libVK_IMG.so.24.2.6603887` — vendor Vulkan 1.3.277
* `/usr/lib/libGLESv2_PVR_MESA.so.24.2.6603887` — vendor GLES
* `/usr/share/vulkan/icd.d/img_icd.json`, `/usr/local/bin/glrun` (zink -> vendor Vulkan)
* kernel driver `pvrsrvkm` (vendor) vs `powervr` (mainline)

An earlier note in this repo claimed there was no vendor userspace and that the 787 FPS baseline was
unreachable. **Both were wrong** — the search missed `_PVR_MESA` and `libVK_IMG`. **Withdrawn.**

## 2. The controlled A/B — same everything, only the Vulkan driver differs

Same weston, Xwayland, client (glmark2), zink, scene, sizes:

| size | zink on Mesa pvr | zink on libVK_IMG | ratio |
|---|---|---|---|
| 640x480 | 45 FPS | **1016 FPS** | 22.6x |
| 1920x1080 | 12 FPS | **297 FPS** | 24.8x |

This is the objective's "787 vs ~31", and it is the Vulkan driver.

## 3. The gap, fully decomposed

| term | open | vendor | ratio | where it lives |
|---|---|---|---|---|
| per-pass syncobj overhead | 0.968 ms/pass (0.456 kernel) | 0.003 ms/pass | **74x** | kernel UAPI (no null job) |
| render: PBE write @1x | 5.78 ms | 1.81 ms | **3.2x** | Mesa PBE, mechanism unknown |
| render: Tiler @1x | 6.97 ms | 4.11 ms | 1.83x | Mesa tiler |
| render: Tiler @4x MSAA | 56.86 ms | 12.57 ms | **4.5x** (growth 8.16x vs 3.06x) | Mesa, mechanism unknown |
| render: shader execution | 1.11 ms | 0.087 ms | 12.8x | small absolutely |
| copy | 4.006 ms | 2.858 ms | 1.40x | closest |
| compute | 393.7 M inv/s | 441.9 M inv/s | 1.12x | not a problem |
| present | 22 ms/frame | 0.980 ms/frame | 22.6x | **100% WSI waits** |

## 4. The corrected model — this is the key conceptual result

**The long-standing "raw render 2.5-4x down / fill-rate deficit" was WRONG.**

`vkrender AREA=quarter` shrinks the drawn area 4x without changing the surface, and the cost barely
moves (14.87 -> 13.90 ms). Fitting `time = a + b*covered`:

| term | open | vendor |
|---|---|---|
| per drawn pixel | 0.31 ms/Mpix | 0.47 ms/Mpix (**open is faster**) |
| per surface | 13.57 ms | 3.69 ms (3.68x worse) |

**It was never a fill-rate problem — it is a per-tile cost.** Every earlier "per-Mpix" figure was
really per-*surface*, because the drawn area always equalled the surface. `AREA` separated them.

## 5. Three-way render split (`DISCARD=1` / `FRAGDISCARD=1`)

| component (2048, s1) | open | vendor | ratio |
|---|---|---|---|
| Tiler (rasterizerDiscardEnable) | 6.989 | 3.818 | 1.83x |
| Shader execution (discard shader) | 1.114 | 0.087 | 12.8x |
| PBE write (difference) | 6.150 | 1.805 | 3.41x |

**This explains why every shader-side change was a measured null**: the shader is a ~1 ms term.

## 6. The two findings with root causes

### 6a. Per-pass syncobj overhead (74x) — root cause found and proved

`pvr_drm_winsys_null_job_submit` (`pvr_drm_job_null.c:41`) is a **userspace fence-forwarding routine**
built from DRM syncobj operations (create + N+1 transfers + destroy). It exists **because the mainline
UAPI has no null job type** — `DRM_PVR_JOB_TYPE_NULL` is a stale comment with no enum value and no
kernel handler. Verified by attempting the fix: it does not build.

* 15 syncobj ioctls per frame measured; 6.4 null jobs/frame on a real client.
* **Payoff measured at 2.5-5% of a real client frame** — real, second-tier.
* **Timeline-backing is NOT the fix** — measured: worse ioctl count (create/destroy traded for resets,
  waits +50%). `VK_SYNC_FEATURE_CPU_RESET` is only offered by `vk_sync_timeline`.
* Fix belongs in the **kernel module** (in scope): add a null/no-op job type accepting `sync_ops`.

### 6b. Present path (22.6x) — localised, and it is correct behaviour

`ACQ_TRACE`/`WSIREL_TRACE` on the vendor stack produce **no output at all**: the vendor client performs
no explicit-sync waits. Mesa does: release 12.5 ms + acquire 9.5 ms of a 22.2 ms frame.

**But Mesa's waits are correct** — the client renders in 0.2 ms while weston repaints on vsync
(~16.7 ms), so it outruns the compositor and waits. The vendor renders 1021 FPS into a 60 Hz display,
so most frames are never shown. **Closing this term means matching non-synchronising behaviour, which
trades away the buffer-reuse guarantee.** It is not a defect in the same sense as the others.

## 7. Refuted hypotheses — keep these, they save rework

| hypothesis | refuted by |
|---|---|
| fill-rate deficit | `AREA=quarter`: cost is per-surface, and per-drawn-pixel the open driver is faster |
| PCO codegen / shader instructions | 24-instruction prologue removal: null; 1-instruction vs 640-op: same deficit |
| DOUTU sample_rate FULL vs SELECTIVE | measured null both trivial and heavy shaders |
| tile load/store | `LOADOP`/`STOREOP=dontcare` change nothing |
| tile partition size | 6144 in both, exactly |
| tiles in flight / ISP pipes | instrumented: 6, as designed |
| tiling geometry | 16x16, `skip_init_hdrs=1` confirmed on |
| GPU clock | 1104000000 under both drivers |
| bytes/pixel, format | flat r8 -> rgba8 -> rg16 |
| memory layout / exportability | linear+exportable at optimal's rate |
| on-chip storage spill | 64 bits/pixel vs the 128-bit recommendation |
| MSAA cost is "expected architecture" | **vendor control: 3.06x vs the open driver's 8.16x growth** |
| mtile sample scaling | required for correctness; consistent scaling changes nothing |
| MSAA sample layout (2,2) vs (1,2) | both correct, zero performance change |

## 8. What is NOT resolved

1. **PBE write 3.2x** — the best-defined Mesa-side target; mechanism unknown.
2. **Tiler 4x-MSAA excess (8.16x vs 3.06x growth)** — three tiling hypotheses refuted; mechanism unknown.
3. **Kernel UAPI null job** — root-caused but needs a kernel change; ~5% payoff.

All three need either the vendor command stream (closed, `libVK_IMG`) or **PVRtune**, which is not
installed and would name the resource (Tiler vs Renderer) in minutes. **The ISP/PBE per-tile state is
the remaining place to look**, and `DISCARD`/`FRAGDISCARD` show it is Tiler-side for MSAA and PBE-side
at 1x.

## 9. Instruments left behind

`vkrender` (`TILING`, `EXPORTABLE`, `FORMAT`, `SAMPLES`, `LOADOP`, `STOREOP`, `AREA`, `MODE`, `BATCH`,
`DISCARD`, `FRAGDISCARD`), `vktex` (sampling), `vkheavy` (ALU), `cstp` (compute), `pvranimate` (KMS
present), and in-driver traces `PVR_TILE_TRACE`, `PVR_SUBMIT_MIX`, `PVR_JOB_TRACE`, `ACQ_TRACE`,
`WSIREL_TRACE`, `SWAP_TIMING`.

## 10. State at handover

* meson `build` dir, buildtype release. **NDEBUG: all `assert()` compiled out** — use
  `MESA_VK_ABORT_ON_DEVICE_LOSS=true` or a `build-assert` dir for invariant checks.
* Mesa tree clean, 36 commits ahead of `main` on `open-pvr-work-2026-10-06`; **never pushed**.
* Bench repo clean, 161 commits ahead, **never pushed**.
* Board: driver `powervr`, weston + Xwayland up, kwin absent, `gpu-fw-guard` active.
* Safety: never unbind `pvrsrvkm` or rebind GPU drivers while kwin/X is alive. Loading a modified
  `powervr.ko` risks the session; measure the payoff first (as done for the null-job change).
