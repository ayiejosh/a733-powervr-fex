# Round-2 corrections and new defects (from the VPU, NPU, audit and LSFG workstreams)

## 1. 🔴 NEW DEFECT: the VE decode engine leaks a runtime-PM reference

**Verified directly after the VPU workstream finished:**

```
1c0e000.ve    runtime_status=active   usage=(empty)   control=auto
1c10000.ve2   runtime_status=suspended
pd_ve_dec     on
```

**Nothing is using it:** `sunxi_ve` refcount 0, no open `/dev/cedar*` handles, no demo processes running. **The
decode engine stays powered and clocked with no consumer.**

**The VPU agent traced it to the H.265 decode attempt aborting in userspace** (a glibc `sysmalloc` assertion during
plugin registration, before decoding a single frame) — **so a userspace abort leaks a kernel runtime-PM reference.**

**Why this matters:** it is the **third** resource-management failure in this driver family:
1. prior session — repeated failing `VideoEncodeOneFrame` leaked **SMMU mappings**; `rmmod` did not clear them,
   only a reboot did;
2. this session — 2× decode + 2× encode churn caused a **NULL deref at `0x18` and a reboot**;
3. now — a userspace abort leaves the **decode genpd held on**.

**Consequence:** the VE block is burning power whenever a decode attempt fails, and a wedged engine may be the
precondition for the later crashes. **Not cleared** — `rmmod`/`modprobe` is itself the risky operation here, and
the cost of trying is a reboot. Logged as an open defect with the evidence.

## 2. The NPU needs ZERO restore — only new models need the compiler

**Correction to my own inventory and to the task framing.** The prior session's 7.1 GB working tree is gone
(`npu-rootfs`, `acuity-run.sh`, `npu-compute`, `xgcc`), **but the 1.1 GB `ai-sdk` clone survived and is
version-matched to the in-kernel driver.** Every existing `.nb` model runs **today**. Only the **x86 ACUITY
compiler** (7.1 GB) is missing, and it is needed **only to compile NEW models.**

**Exact next command, no root, no download, ~10 s:** `sh MAPS/npu-smoke.sh` — preprocesses a JPEG, runs
ResNet-50, prints top-5, and **asserts top-1 == collie**. 67 GB free on `/`, 88 GB on `/mnt/sdcard`; the ACUITY
restore needs ~10 GB transient and the source is reachable (HTTP 200).

## 3. A corrected framing on "disabled features": the 1120 MHz NPU OPP is NOT enableable

**It exists in the DT (`npu-opp-table/opp-1120`) but the board resolves to VF index 0, whose supported list is
492/852/1008 MHz only.** The +11 % is **latent silicon tied to a different chip VF bin, not a configuration
toggle.** **Do not plan on it.** (Current: 1008 MHz, governor `performance`, `cur_state 0`.)

**This is the first candidate to be ruled out rather than enabled, and it sharpens the rule: presence in the DT is
not evidence of enableability.** Compare `g2d` — present as a *device* with clocks registered and the driver in
the config but unset, which IS enableable.

## 4. NPU thermal/power detail worth keeping

* `devfreq-3600000.npu` is registered as **`cooling_device8`** (max_state 2) under the IPA `power_allocator` on
  `npu_thermal_zone`; the only trip point is **critical @110 °C**.
* Idle: NPU 63.7 °C vs cpub 65.0 / gpu 64.6 / ddr 61.1.
* 100 back-to-back inferences showed **no timing step** → no throttling expected; IPA would fall to 852 MHz
  (−18 %) long before 110 °C.
* `pd_npu off-0` with all consumers suspended is **correct idle gating, not a disabled feature.**

## 5. A silent-wrong-answer trap worth propagating

**While writing `npu-smoke.sh`, the agent's own assert caught a real bug: Pillow's `load()` indexes `[x,y]`
(column,row), so feeding `px[h,w]` sends a TRANSPOSED image — and the NPU still returns a confident, plausible,
WRONG answer** (Shetland 12.63 vs the correct collie 11.42). **Input must be NCHW planar via `tobytes()`.**
**Same class as every other error this project has recorded: instrumentation that produces believable wrong data.**

## 6. VPU: the source's own build command is WRONG

**The build line in `sunxi_ve_drv_video.c`'s header omits `-lvencoder -lMemAdapter -lVE`.** It **links
successfully** but yields no vendor `NEEDED` entries, so `dlopen` fails with `undefined symbol: AllocInputBuffer`
and `vaInitialize` returns −1. **Correct recipe is in the map; verified by building both ways and diffing
`readelf -d`.** A trap that looks like success.

## 7. VPU open defect: deterministic P-frame corruption (DMA-sync hypothesis FALSIFIED)

**The VA-API encoded stream is structurally perfect** (1 SPS / 1 PPS / 1 IDR / 29 P, 30 frames, `ffprobe` parses
it) **yet ffmpeg conceals an entire 3600-MB P frame.** The dma-buf sync-direction hypothesis
(`DMA_BUF_SYNC_READ` where CPU→device needs `SYNC_WRITE`, lines 475/479) was **tested and FALSIFIED** — the patched
build emits **byte-identical output**, so it is not a cache race. **Patch recorded, NOT deployed; source restored to
the as-found bytes (`a58e3340` / `fabdbab2`).** **A real encoder bug remains unexplained.**

## 8. H.265 / VP9 remain UNMEASURED

`vdecoderdemo` **aborts in userspace** (glibc `sysmalloc` assertion) during plugin registration, before decoding a
frame. **That abort is the blocker for any per-codec measurement** — and it is also what leaked the genpd ref in
item 1.

## 9. GPU numbers are ~1.8× LOW, not wrong

**GPU_BENCHMARK.md (2026-06-05) 4198/1216/315/80 → today 7355/2211/574/147 Mpix/s** (same binary, same shader), CPU
baseline unchanged (31/7.3/1.6/0.37). **The "~150-175× GPU advantage" is really 237-397×.** GPU clock 600 → **1104
MHz** (generator ceiling; still **no DVFS** — the driver imports no `dev_pm_opp`, and >1104 clamps).

## 10. Unresolved: no zero-copy path from the NPU to the GPU/display

**An app-visible NPU path has no dmabuf route to the GPU or the display.** Same shape as the VE (whose VA-API
export exists) and as g2d (no driver). **The glue between accelerators is the recurring gap on this board.**
