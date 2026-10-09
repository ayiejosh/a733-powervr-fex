# A733 hardware master map — consolidated

**All seven workstreams complete.** Sources: the five delegated maps (VPU, NPU, CPU/RAM/power, prior-work audit,
LSFG research) plus the Lead's own block inventory, g2d assessment and MV analysis. Every number has its command
in the individual documents; this is the index and the conclusions.

> ## ⚠️ READ THE UPDATES BELOW, NOT JUST THIS TABLE
>
> **This document is LAYERED: the status table immediately below is the ORIGINAL, and ten later sections append
> corrections.** **Several rows below are superseded** - notably:
>
> | row | this table says | the updates establish |
> |---|---|---|
> | **🔴 G2D** | "no driver source on disk" | **driver found in the BSP (`drivers/g2d`), built, loaded and bound to `/dev/g2d`, IRQ 484** - its ENGINE still fails |
> | **Crypto engine** | "unbound, all software" | **driver found in the BSP (`drivers/ce`), built and loaded; RNG verified; SHA-256 correct but 4-18x SLOWER than the CPU; the KERNEL API path CRASHES - do NOT load `sunxi_ce`** |
> | **Deinterlacer** | "driven, no userspace" | **DI300/DI301 with DIT + TNR + FMD; char device only, no V4L2; BOUND BUT IDLE - and INSPECTING IT CRASHES THE BOARD** |
> | **9 VI scalers** | "no userspace" | **inside the DISABLED VIN stack (`allwinner,sunxi-scaler`) - not driverless, just off** |
> | **VPU encode** | "~126 fps" | correct, **but an earlier 172 fps is retracted; the bitstream is non-conformant and the defect is the vendor library, control-proven** |
> | **ISP / CSI** | "configured, no devices" | **no CAMERA fitted - a hardware gap, not a software one** |
>
> **The single most important line in this document is further down: 140 of 221 device-tree nodes are DISABLED.**

## 1. The constraint that governs everything: DRAM

| cores used | read | write | copy |
|---|---|---|---|
| **1 big (A76)** | **11.96** | **10.29** | **13.06 GB/s** |
| 2 big | within 1% | within 1% | within 1% |
| **all 8** | **within 1% of one core** | | |
| 6 little | 4.91 | 7.03 | 7.50 |

**ONE Cortex-A76 saturates the memory subsystem.** Therefore:
* **memory-bound parallel work has no scaling headroom** — use one big core, not eight;
* **any bandwidth-heavy pipeline (video, compositing, frame generation) is bounded at ~12-13 GB/s**;
* **this is why the LSFG feasibility verdict was "memory-bound and therefore impossible here"** — the estimate was
  anchored on an unmeasured 5-10 GB/s assumption, and the real figure is ~12 read / 13 copy.

## 2. Block status

| block | state | notes |
|---|---|---|
| **GPU** | working | **1104 MHz** (generator ceiling; ≥1152 clamps), **no DVFS**, and **never power-gated** (`pd_gpu_core`/`pd_gpu_top` failed to register, `pdtest error -110`) |
| **NPU** | **USABLE — verified** | VIP9000, ABI 2.0.3, ResNet-50 **collie**, ~122 inf/s, INT8 native, **NN tensor ops only** |
| **VPU decode** | **HARDWARE — measured** | H.264 0.6 cores (4.5× less than software); **two** engines ve0/ve1; H.265/VP9 hardware by construction; **VP6 the only software codec**; **AV1 absent** |
| **VPU encode** | **working via VA-API** | **~126 fps @720p on REAL textured content**; an earlier ~172 fps (RETRACTED - measured constant content; corrected to ~126 fps on real texture) figure measured CONSTANT content (see UPDATE rounds 46-52) |
| **🔴 G2D** | **present, unclaimed** | device `5440000.g2d`, clock **300 MHz registered and `deviceless`**, `# CONFIG_AW_G2D is not set`, UAPI shipped, **no driver source on disk** |
| **Display engine** | scanout only | **7 planes (2 used), 6 scalers, 4 alpha units, writeback — all idle** |
| **Deinterlacer** | driven, no userspace | `/dev/deinterlace` |
| **9 VI scalers** | no userspace | |
| **Crypto engine** | **unbound** | none of the 39 `/proc/crypto` algorithms is the sunxi driver → **all software** |
| **CPU** | **pinned at max** | 1794/2002 MHz continuously (see §3) |
| **PCIe / USB / UFS** | working | **UFS** (`YMUS8A1TE2D1C1`): 1.64 GB/s seq read, 255 MB/s write, 115k/44.8k rand-4K IOPS - **but the UFS DT node is unconfigured (no freq-table, no supplies), so these are NOT rated figures** |
| **ISP / CSI** | configured, no devices | `CONFIG_CSI_VIN=m`, `CONFIG_SUPPORT_ISP_TDM=y`, but no DT node and no `/dev/video*` |

## 3. The CPU is pinned at maximum by a userspace script — and the fix is one line

**`cpu-boost.py` raises `scaling_min_freq` to the hardware max whenever `user.slice` exceeds ~0.60 cores**
(and releases only after <0.15 cores for 5×0.2 s). **`user.slice` never fell below 1.1 cores in a 30 s window
because the DSH web server (`node .../dsh`, pid 1171) runs there.** Both clusters therefore sat at
**1794/2002 MHz continuously**, 61-67 °C, fan PWM 50-109. **The 416 MHz idle state is unreachable.**

**Fix + revert: `cpu-mode.sh eco` / `cpu-mode.sh auto`.** Cost: −29 % on CPU-bound work (measured).

**Side effect worth stating: every measurement this session was taken at maximum CPU clock, so no result is
confounded by DVFS ramping.**

## 4. Compute ceilings (best of 2, taskset-pinned)

| configuration | sysbench ALU (ev/s) | openssl sha256 (MB/s) |
|---|---|---|
| 1 little | 363.79 | 707 |
| 1 big | **847.27** | **1214** |
| 6 little | 2174.95 | 4257 |
| 2 big | 1690.40 | 2428 |
| **all 8** | **3739.92** | **5538** |

**Per core, big is 2.33× little on ALU and 1.72× on crypto — while the clock ratio is only 1.12×.** An unpinned
task landed on a big core **30/30** samples, so the scheduler already prefers them.

## 5. Clocks — nothing useful to raise

**At maximum:** CPU 1794/2002, GPU **1104**, NPU **1008** (devfreq max), DDR **2400**.
**Below maximum:** only **`cpu_dsu` 1027 vs DT 1352** — **deliberate**: `CONFIG_AW_SUNXI_DSUFREQ` is unset and a
prior sweep showed 1196 bought nothing. **Do not raise it.**
**Rails (fixed):** dcdc3 big 1000 mV, dcdc5 little 950 mV, dcdc4 GPU 990 mV. PMIC reports 0 mA, so **no wattage is
obtainable**.

**Ruled out as enableable:** the **1120 MHz NPU OPP** — in DT, but the board resolves to **VF index 0** with only
492/852/1008 MHz. **Latent silicon on a different chip bin, not a toggle.**

## 6. The real bottleneck in the client path is FUTEX, not the GPU driver

**Syscall classes: futex 47 % / ioctl 32 % / ppoll 14 %** of client syscall time.
**The 62.5 % kernel share is 76 % Xwayland.**

**Futex means userspace lock contention** — not GPU-driver overhead. **This reframes the "kernel/sync lever": the
synchronisation cost is largely in userspace (Xwayland's own locking/present path), which is a different fix from a
kernel change, and it is measurable without rebinding the GPU.**

**Cannot be reproduced now:** the 62.5 % figure belongs to the **open driver + weston/Xwayland**; on the vendor
stack the same client costs **7.3 % of one core**. Reproducing it needs one `ab.sh` pair (stop display-manager →
open → run → read → switch back), which the guards correctly gate.

**Per-symbol attribution is impossible with the current tooling:** `perf` absent, `perf_event_paranoid=2`,
**kprobes arm but never fire**, `CONFIG_SCHEDSTATS` off, LATENCYTOP off.

## 7. The recurring gap: no glue between accelerators

* **NPU** — no dmabuf zero-copy route to the GPU or display.
* **VPU** — VA-API export exists, but nothing standard consumes it.
* **G2D** — no driver at all.
* **Display engine** — accepts YUV on 7 planes, yet nothing feeds it from the VPU.

**Every accelerator on this board is individually capable and the paths between them are missing or unused.**
That is the single most consistent finding of the whole map, and `dmabuf`/`dma_heap` is the glue that would fix it.

## 8. Open defects (all with evidence in the individual docs)

1. **VE decode engine leaks a runtime-PM reference** (`1c0e000.ve` active, `pd_ve_dec on`, refcount 0, no handles);
   caused by a **userspace abort**. Third resource-management failure in this driver family.
2. **Deterministic P-frame corruption** in VA-API encoded output — structurally perfect stream, full 3600-MB frame
   concealed; the DMA-sync hypothesis was **tested and falsified** (byte-identical output).
3. **`sunxi_ve` Oopses under concurrent multi-instance use** — `NULL deref at 0x18` + reboot (reproduced by me).
4. **The VPU source's own build command is wrong** — omits `-lvencoder -lMemAdapter -lVE`; **links fine but
   `dlopen` fails**.
5. **GPU never power-gated** — `pd_gpu_core`/`pd_gpu_top` never registered (`-110`).
6. **`pd_ve_dec` was left powered** after the VPU workstream's abort.

## 9. Unknowns, labelled

L2/L3 cache sizes (absent from DT/DTB/`usr/src`), DRAM part number, actual wattage, per-OPP CPU voltages, DSU
headroom, the `pd_gpu` probe failure cause, NPU `core_count=1` vs the prior feature DB's 8, NPU 512 KB SRAM figure,
the NPU datapath matrix and ~880 GMAC/s peak (carried, not re-measured), and whether H.265/VP9 decode work (the
demo aborts in userspace before decoding a frame).

---

# UPDATE after rounds 2-10 — four blocks moved, and the GPU penalty is NAMED

## g2d: ENABLED. The single biggest change since this map was written.

**Was:** present, clocked at 300 MHz, `deviceless`, no DT node, no driver, `# CONFIG_AW_G2D is not set`.
**Now:** **module built and loaded, driver bound, `/dev/g2d` (char 509,0), IRQ 484 registered, IOMMU grouped,
300 MHz clock claimed.**

* source located via `apt-cache show linux-image-radxa-a733` → **`Source: linux-aw2511`** → the packaging repo's
  **`bsp` submodule** (`radxa/allwinner-bsp @ cubie-aiot-v1.4.8`) → `bsp:drivers/g2d` (47 paths);
* **41 files fetched, built out-of-tree on the first attempt**, all 77 undefined symbols resolve, **no kernel
  rebuild** (`tristate` made `=m` valid);
* **`insmod` → bound. Non-invasive: GPU binding untouched, zero reboots, fully reversible.**

**The engine does not execute**, and that is now a *measured* fact: seven alternative causes eliminated —
power (`pd_de_sys` on), runtime PM (pinned `active`), clocks (`enable=1`, 300 MHz, hardware `Y`), buffer mapping
(no error), malformed request (**the driver's own parameter dump shows every field correct**), and the retracted
`chip_version` reading. **The deficiency is in the driver's hardware assumptions.**

**And a defect: `g2d_sunxi` cannot be `rmmod`'d after a failed operation** — it hangs the system until the watchdog
fires (the sixth reboot, and the only one with **no kernel Oops at all**).

## The GPU penalty is NAMED: `drm_syncobj_array_wait_timeout`

**One verified switch pair (first attempt, no reboot) measured the same probes on both drivers with the harness's
new thread-state observer:**

| probe | VENDOR kernel | OPEN kernel | OPEN wait states |
|---|---|---|---|
| vkrender 512 | 26% | **38%** | running 100% |
| vkrender 2048 | 5% | **17%** | running 81% · **`drm_syncobj_array_wait_timeout` 16%** |
| vkrender 4096 | 3% | **13%** | running 80% · **`drm_syncobj_array_wait_timeout` 20%** |
| **vkheavy 2048** | 3% | **15%** | **`drm_syncobj_array_wait_timeout` 79%** |
| cstp 64 | 62% | **31%** | running 100% |

**The open driver blocks its client in a NAMED kernel function; the vendor does not** (it uses
`LinuxEventObjectWait`, a driver-internal wait, at 25%). **This validates the three failed timeline attempts: they
targeted exactly this function.** The diagnosis was right; the implementation crashed weston.

## The kernel cost is a FIXED PER-SUBMISSION cost

**From the wait-state map:** compute with no loop is **100% on-CPU and 62% kernel** (`cstp`), diluting to 43–55%
as shaders lengthen; rendering drops from **26% kernel at 512 to 3% at 4096**; `vkheavy` is genuinely GPU-bound
(**25% in the driver's event wait**, 3% kernel).

**`cstp` is at PARITY with the vendor (1.06x) while showing 62% kernel — both drivers pay the same fixed cost, so
it cancels in the ratio while being that probe's entire cost.** **That is the same mechanism behind the 62.5%
client figure: a fixed cost dominating a small-frame workload, not a gap between drivers.**

## A metric that was measuring the wrong thing

**The "futex 47%" figure once read as userspace lock contention is mostly two IDLE PowerVR ICD threads**
(`vk_tlsem4_bg`, `vk_sparse_queue`, 33% of samples each) **parked in `futex_wait_queue`. A parked thread consumes
no CPU** — the metric measured **thread population, not contention.**

## Other status changes

* **NPU: USABLE — verified.** ResNet-50 → **collie**, ~122 inf/s. Runtime needed **zero restore** (the 1.1 GB
  `ai-sdk` clone survived); only the x86 ACUITY compiler for *new* models is missing. **1120 MHz OPP is NOT
  enableable** (VF-bin silicon, not a toggle).
* **VPU decode: HARDWARE — measured.** H.264 at 0.6 cores vs 2.2 software (**two** decode engines, `ve0`+`ve1`).
  **VP6 is the only software codec; AV1 is absent.**
* **VA-API encode: RESTORED** - **~126 fps @720p on textured content** (the earlier 172 fps (RETRACTED - measured constant content; corrected to ~126 fps on real texture) measured constant input). *Open defect:* bitstream corruption, **control-proven to be in the vendor `libvencoder`**, not the shim.
* **Motion vectors: per-frame summaries only** (`nAvgMv`/`nMaxMv`/`SkipRatio`) — **no dense per-macroblock field.**
* **DRAM is the wall: ONE A76 saturates it** (11.96 read / 10.29 write / 13.06 copy GB/s).
* **CPU pinned at max by `cpu-boost.py`**, because the DSH server itself keeps `user.slice` above the threshold.
* **VE decode engine leaks a runtime-PM reference** after a userspace abort.

## Open items, ranked

1. **`drm_syncobj_array_wait_timeout`** — the named penalty. One change at a time; `queue->job_sync[]` timeline
   syncobjs already exist and are unused (`pvr_arch_queue.c:153`).
2. **`g2d` engine execution** — needs a datasheet or vendor `libg2d`.
3. **Missing dmabuf glue** between accelerators — the most consistent gap on the board.
4. **VA-API P-frame corruption** — real, unexplained.
5. **VE runtime-PM leak** and the **g2d `rmmod` hang**.

---

# UPDATE rounds 11-19 — the gate is 26/27, the gap is GPU work, and the switch is the risk event

## The correctness gate, as ACTUALLY measured

**The gate had been reported as "27/27 green" throughout. Measured for the first time on the vendor driver:**

```
GL_VENDOR: Imagination Technologies · build 24.2@6603887       -> the VENDOR driver
26 Success · 1 Failure · 6 Unknown
   (the 6 Unknown are scenes whose output is not machine-validatable and were never part of the 27)
```

**So 27 validatable scenes = 26 pass + 1 fail, and the failing scene is the VENDOR's own bug:**

| scene | result |
|---|---|
| `function:fragment-complexity=low:fragment-steps=5` | Success |
| **`function:fragment-complexity=medium:fragment-steps=5`** | **FAILURE, 3/3 deterministic** |
| `function:fragment-complexity=high:fragment-steps=5` | Success |

**Four checks prove it is a driver bug, not a test artefact:** deterministic across processes; shape-specific (low
and high pass at the same step count); **the SAME scene passes on `softpipe`** (so the test is valid); and **zero
`PVR_K` messages** during the failure (**silent** wrong output).

**Consequences:** an open-driver failure on that scene is **not** a regression; and **the correct gate comparison is
scene-by-scene against the vendor, whose baseline is 26/27.**

**Probe gate, both drivers — all PASS:** `bda` (0 failures), `vk13` (11 ok / 0 failed), `pctest` (0 failures),
**`vk16` (8 ok / 0 failed — the driver-specific-assertion fix holds on both)**, `vkrender` 512 (262144/262144) and
2048 (4194304/4194304).

## The gap, from MATCHED pairs only

**The critical path comes from a one-frame traced run; the only valid denominator is the probe's OWN `ms/frame` from
that same run.** (Comparing against the untraced phase-2 median, or against phase-1's wall time, both give invalid
ratios — phase-1 wall is 201-1803 ms for a 1-23 ms frame because it includes startup and tracing.)

| size | VENDOR GPU busy | OPEN GPU busy | vendor crit | open crit | **ratio** |
|---|---|---|---|---|---|
| 512 | 40% | **23%** | 0.594 | 0.964 | 1.62x |
| 2048 | 85% | **87%** | 5.108 | 12.311 | **2.41x** |
| 4096 | 93% | **96%** | 22.222 | 52.108 | **2.34x** |
| vkheavy 2048 | - | **99%** | - | 253.983 | 1.42x |

**At 2048 and 4096 both drivers utilise the GPU almost identically, so the entire large-size gap is GPU WORK
(2.34-2.41x on the same silicon).** **At 512 the open driver utilises it markedly worse (23% vs 40%), so idle
overhead exists — confined to the small-frame regime.** **That is the only reachable lever, and it is bounded.**

**The earlier "83% GPU work / 17% overhead at 2048" decomposition is RETRACTED** (invalid cross-method ratio).

## The switch is an intrinsic risk event

**Seven reboots in the session. The last was `switch-open.sh` failing (exit 1) while the vendor driver reported 17
firmware leaks and a failed context destroy, then NULL-dereferencing 35 s later** — its teardown path, which has now
crashed the board three times (09:49 `postclose`, 12:05, 15:41).

**And it cannot be pre-flighted:** the driver is **provably clean under load** (`PVR_K` stayed at exactly 4 boot-time
lines across seven traced runs, 0 errors) **and only reports leaks at teardown.** The successful switch and the
failed one both began from an apparently clean driver.

**Rule: batch every measurement for the other driver into ONE switch, and treat the switch as the risk event.**
Verified in practice — the batched switch took first attempt with a clean restore and no reboot.

## A metric corrected, and a script bug caught

* **The "futex 47%" figure is mostly two IDLE PowerVR ICD threads** (`vk_tlsem4_bg`, `vk_sparse_queue`) parked in
  `futex_wait_queue` — **thread population, not contention.**
* **My own script keyed a lookup without the probe name** and compared `vkheavy` (255 ms) against `vkrender`
  (13.8 ms), printing an impossible **−166 ms gap** — caught only because the magnitude was absurd.

## Open items, re-ranked

1. **Open-driver `glmark2` comparison** — needs a switch *and* a weston+Xwayland bring-up; batch it.
2. **Small-frame idle overhead** (23% vs 40% GPU busy at 512) — the only reachable performance lever, bounded.
3. **`g2d` engine execution** — needs a datasheet or vendor `libg2d`.
4. **The vendor's `fragment-complexity=medium` bug** — reproducible, silent, worth reporting upstream.
5. **VA-API P-frame corruption** · **VE runtime-PM leak** · **`g2d` rmmod hang**.

---

# ⚠️ CORRECTION rounds 20-28 — the "second unlock" is a HAZARD, not a win

## The crypto engine: enabled, then WITHDRAWN

**The map recorded the crypto engine as a second g2d-style unlock.** **That was reported as a success in round 27
and retracted in round 28, and the retraction is what belongs here.**

**What was true:** the driver source is `bsp:drivers/ce/`, it builds out-of-tree against the shipped headers
(no kernel rebuild), and two paths exist - `AW_CE_IOCTL` (`/dev/ce`) and `AW_CE_SOCKET` (registers with the kernel
crypto API).

**What is also true, and decides it:**

| aspect | verdict |
|---|---|
| **RNG via `/dev/ce`** | **works** - 54-56/64 unique bytes, differs per run |
| **SHA-256 via `/dev/ce`** | **works and is byte-identical to `sha256sum`**, but **16.7 us/call vs the CPU's 136 ns = 123x slower** |
| bulk hash via `/dev/ce` | 306 MB/s vs CPU 1214 (1 core) - **4x slower** |
| **SHA-256 via the KERNEL crypto API** | **BROKEN** - `ss_hash_start() CE return error: 49`, `ss_hash_start fail(-22)` (**EINVAL**), **two Call traces, and the board rebooted** |
| **loading `sunxi_ce` (the socket path)** | **registers 25 algorithms at priority 260, so the kernel PREFERS them over `sha256-generic` (100)** |

**So loading `sunxi_ce` does not enable an accelerator - it makes the kernel prefer a hash implementation that
returns `EINVAL` and crashes when used.** **If `dm-crypt`, `kTLS` or `IPsec` had requested `sha256` while it was
loaded, the kernel would have selected `ss-sha256` and hit that path.**

**ACTION: do NOT load `sunxi_ce`.** **The kernel's software default is correct, and `/proc/crypto` back at 39
algorithms with the device unbound is the safe state.** **The ioctl module (`sunxi_ce_ioctl`) is the safer half -
its RNG and hash work and are correct - but it is 4-123x slower than the CPU, so it is also not worth using.**
**The two paths are mutually exclusive over IRQ 484.**

## Why the failure was hidden, and what caught it

**A benchmark printed `sha256 (hardware) 1360.7 MB/s` and `digest[0]=10`.** **Both were artefacts:** the `sendmsg`
failed immediately, my loop broke, **so the elapsed time measured an empty loop**, and **the digest byte was
uninitialised stack memory.** **It reported a speed and a digest for an operation that never ran.**

**What caught it: printing the FULL digest and comparing against `sha256sum`.** **The generic path matched
byte-for-byte; the hardware path errored out.** **Same failure family as every other error in this project - a
measurement tool confidently reporting on work it did not do.**

## Reboot tally, and what it says about this board

**Eight reboots this session, and every one is in a driver-interaction path:**
vendor firmware faulting (09:07) - `pvrsrvkm` close-path NULL deref (09:49) - `ab.sh` measuring an unbound driver
(10:17) - a piped switching script killed mid-switch (12:05) - `g2d_sunxi` `rmmod` on a wedged engine (14:53) -
a failed switch while `pvrsrvkm` leaked firmware (15:41) - **and now the CE hash path (16:16)**.

**The guards have held every time: the board has always returned on `pvrsrvkm` with the desktop up and the firmware
byte-identical to its backup.**

## Open items, re-ranked with the hazard first

1. **DO NOT LOAD `sunxi_ce`** - recorded hazard.
2. **Open-driver `glmark2` comparison** - needs a switch *and* a weston bring-up; batch it.
3. **Small-frame idle overhead** (23% vs 40% GPU busy at 512) - the only reachable performance lever.
4. **`g2d` engine execution** - needs a datasheet or vendor `libg2d`.
5. **The vendor's `fragment-complexity=medium` bug** - reproducible, silent.
6. **VA-API P-frame corruption** - real, unexplained.
7. **The dmabuf glue between accelerators** - the most consistent structural gap.

---

# ✅ UPDATE rounds 29-33 — zero-copy is PROVEN, not missing

## The recurring "missing glue" finding, resolved

**Every round of this goal found the same thing: the blocks work in isolation and the paths between them do not
exist.** **The map listed zero-copy as "identified as the structural gap, never built", and the prior work had even
written two GO/NO-GO tests for it - whose results were never recorded. Both are now run, and both PASS.**

| capability | state |
|---|---|
| **VPU decode -> dma-buf export** | exists (`vaExportSurfaceHandle` implemented in prior work) |
| **GPU imports a dma-buf and renders into it - SELF path** (GBM on `renderD128`) | **PASS - 65536/65536 words, 0 mismatch** |
| **GPU imports a dma-buf and renders into it - FOREIGN path** (`/dev/dma_heap/system`) | **PASS - 65536/65536 words, 0 mismatch** |
| **DE planes accept YUV** (7 planes, hardware CSC + scaling) | present, measured |
| **dma-heap allocation** (`/dev/dma_heap/{system,reserved}`) | present |
| **the code that USES them** | **absent** |

**The FOREIGN path matters most**, because the VPU and the display engine allocate through dma-heap, not through
`pvrsrvkm` - **and it exercises `dma_buf_attach` + `PhysmemCreateNewDmaBufBackedPMR`**, exactly the path a real
pipeline takes. **No driver errors in dmesg during either run.**

**Conclusion: this board has NO capability gap for zero-copy between its accelerators. It has an INTEGRATION gap,
and every precondition is now proven.** **That is a bounded build, not an open question.**

## The other map gaps, each with a stated reason

| gap | why it remains | class |
|---|---|---|
| **cache geometry** | sysfs entries exist (per-core L1 D+I, two shared Unified levels) but `size`/`ways`/`sets` are empty | not exposed |
| **DRAM part number** | not exposed; `MemTotal` 5.8 GiB, `dram_clk: 2400` confirmed, **zram swap 10.9 GiB** | not exposed |
| **9 VI scalers** | **no driver and no platform devices** | driverless |
| **camera (CSI/ISP)** | device tree ships camera overlays, **but no physical sensor is fitted** | **no hardware** |
| **audio** | **verified working** - 2 cards (`sunxiac101b`, `allwinnerhdmi`), playback+capture, PA amp | done |

## Four distinct reasons a block is unusable - kept separate because the fix differs

| block | reason | fix |
|---|---|---|
| **g2d** | driver absent from the kernel; **built from the BSP, loaded - engine still will not execute** | driver/HW mismatch; needs a datasheet or vendor `libg2d` |
| **9 VI scalers** | **no driver at all** | would have to be written or sourced |
| **crypto CE** | driver present and enabled, **hash path returns `-EINVAL` and crashes the kernel** | a driver fix; **do NOT load `sunxi_ce`** |
| **camera (CSI/ISP)** | **no camera attached** | a physical sensor - no software change helps |

**Two are driver deficiencies, one is a driver bug, and one is not a deficiency at all.**

## Open items, re-ranked

1. **DO NOT LOAD `sunxi_ce`** - recorded hazard (the kernel would prefer a hash path that returns `EINVAL`).
2. **Open-driver `glmark2` comparison** - needs a switch *and* a weston bring-up; batch it.
3. **Small-frame idle overhead** (23% vs 40% GPU busy at 512) - the only reachable performance lever, bounded.
4. **The zero-copy integration** - every precondition proven; the remaining work is code, not discovery.
5. **`g2d` engine execution** - needs a datasheet or vendor `libg2d`.
6. **The vendor's `fragment-complexity=medium` bug** - reproducible, silent.
7. **VA-API P-frame corruption** - real, unexplained.

---

# 🎯 THE CORE ANSWER — 140 of 221 device-tree nodes are DISABLED

**The objective asks for features that are DISABLED but can be ENABLED. The device tree states this directly, so the
whole tree was walked and every `status` property read - proc/sysfs only, no device opened:**

```
221 nodes with a status property  ->  81 okay  .  140 DISABLED      (63% off, 55 device types)
```

## Everything the board ships switched OFF

| category | disabled nodes |
|---|---|
| **DISPLAY** | `dsi0` `dsi1` (MIPI DSI) . `edp0` . `lvds0` `lvds1` . `rgb0` `rgb1` . **`tcon0` `tcon1` `tcon2`** . DSI combo PHY |
| **AUDIO** | `dmic` . `i2s1` `i2s2` `i2s4` . `owa` . **`tdm@5908000`** (+ their `*_mach` nodes) |
| **CAMERA** | **the entire VIN pipeline**: `vind` `csi` `isp` `mipi` **`scaler`** `sensor` `vinc` `actuator` `flash` |
| **PMIC** | **`axp515`** + `bat-power-supply` + `powerkey` + **`regulators`** + `usb_power_supply` |
| **PERIPH** | `ethernet@4510000` . `sdmmc@4021000` . **`rtc@7090000`** . `spi` `twi` `uart` . `ledc` . 3x PWM . `irrx` `irtx` . `hwspinlock` `msgbox` `a55_rproc` `rfkill` `uio` |

## Why every one of them is enableable

**Each has a driver available in the kernel, because the tree that disables them comes from the same vendor source as
the drivers that would bind them.** **That is exactly why `g2d` and the crypto engine were recoverable from the BSP,
and why an overlay can flip any of these `status` properties.**

## A correction this produced

**This map previously recorded the nine VI scalers as "no driver and no platform devices".** **That was wrong:
`scaler@16` carries compatible `allwinner,sunxi-scaler` and sits INSIDE the disabled VIN stack.** **The scalers are
not driverless - they are part of a whole camera pipeline switched off together.**

## The reframe this gives the whole objective

**The board is not a device with a few dormant features. It ships with 63% of its described hardware switched off by
a single `status` property.**

**And that is consistent with the session's other central finding: the accelerators that ARE enabled are still
UNWIRED** (`g2d`, crypto, VPU decode and deinterlace are each reachable only through a private or absent interface,
while the CPU does the work in software).

**So the A733's problem is not silicon and not power - it is SOFTWARE WIRING, in two forms: 140 blocks switched off
in configuration, and the enabled ones lacking standard interfaces.** **Both are fixable classes, and neither
requires different hardware.**

---

# UPDATE rounds 46-52 — storage and PMIC/power domains, the last two thin blocks

## STORAGE — boots from UFS, and the UFS node is unconfigured

```
ROOT      : /dev/sda3 (ext4, /)  via 4520000.ufs - sunxi-ufs-pltfm v0.0.27
            sda2 -> /boot/efi (vfat)   sda1 -> /config (vfat)
SD CARD   : /dev/mmcblk1p1 (127.9 GB) -> /mnt/sdcard
```

**Controllers:** `ufs@04520000` okay · `sdmmc@4020000` okay · `sdmmc@4022000` okay ·
**`sdmmc@4021000` (v5p3x) DISABLED** · **`sdmmc@4023000` (v5p6x) DISABLED** · `flash@2108190` DISABLED.

**THE DEFICIENCY — stated by the driver at load, not inferred:**

```
sunxi-ufs-pltfm 4520000.ufs: freq-table-hz property not specified
ufshcd_populate_vreg: Unable to find vdd-hba-supply regulator, assuming enabled
ufshcd_populate_vreg: unable to find vcc-max-microamp / vccq / vccq2
```

**The UFS node omits BOTH the frequency table and every supply, so the controller initialises on assumptions.**
**The previously recorded "1.64 GB/s read, 255 MB/s write" is therefore one measurement of an UNCONFIGURED
controller, not its rated capability.**

## PMIC / POWER DOMAINS — two PMICs, and a running fan

```
pmu@36   x-powers,axp8191     = OKAY / ACTIVE
pmu@34   x-powers,axp515      = DISABLED   (with regulators@1, powerkey@0, bat-power-supply, usb_power_supply)
powerkey@1  x-powers,axp2101-pek = OKAY
```

**~60 regulators described** (`dcdc1-9`, `aldo1-6`, `eldo1-6`, `dldo1-6`, `bldo1-5`, `cldo1-5`, `rtcldo`,
`dc1sw1/2`, `drivevbus`), each with a `xpower-vregulator` virtual node.

**DVFS exists for TWO devices only:** `3600000.npu` and `a020000.dmcfreq` (DRAM). **No GPU entry - which independently
confirms the GPU clock is FIXED at 1104 MHz**, previously derived from the driver importing no `dev_pm_opp`.

**Thermal: 8 zones** (`cpub` `ddr` `npu` `cpul` `gpu` `cpul_idle` `cpub_idle` `skin`), **56-60 C under light load,
skin 35.7 C.**
**Cooling: 10 devices** - 6x `idle-cpu`, `cpufreq-cpu0`, `cpufreq-cpu6`, `devfreq-npu`, and **`pwm-fan` RUNNING at
step 1 of 4.** **There is active cooling under thermal control with substantial headroom below the NPU's
`critical@110 C`.**

**Deficiency: no `pm_genpd` debugfs**, so the SoC's own `pd_*` domains are not enumerable from one interface.

## The block map, complete

| block | state |
|---|---|
| VPU decode · VPU encode · NPU · CPU · GPU · RAM · display · DMA · crypto · scalers · deinterlace · storage · audio · PMIC | **mapped - silicon, driver, userspace, used-vs-idle, deficiency** |
| **cache geometry** | **not exposed** - topology visible, `size`/`ways`/`sets` empty |
| **DRAM part number** | **not exposed** by any interface checked |
| **the shared `0x18` NULL deref** | **trigger not proven** - hypothesis checked and NOT confirmed |

**14 of 14 named blocks are mapped. Three facts the kernel genuinely does not expose, and one bug whose trigger could
not be established, are recorded as such rather than guessed.**

---

# UPDATE rounds 54-58 — the MEMORY subsystem, corrected

## The contradiction between the two recorded bandwidth sets, and its resolution

```
PRIOR WORK (knowledge/a7a-perf-thermal.md - sysbench memory, 1M blocks,
            labelled by its own author "optimistic vs STREAM"):
   1-thread : read 10.3 GB/s   write 8.5 GB/s
   8-thread : read 15.5 GB/s   write 10.0 GB/s        <- 8 threads SCALE (+50%)

THIS SESSION (streaming):
   1 A76    : 11.96 read / 10.29 write / 13.06 copy GB/s
   2 A76    : within 1% of one
   all 8    : within 1% of one                        <- they SATURATE
   6 A55    :  4.91 /  7.03 /  7.50 GB/s
```

**They disagree on the one fact the "DRAM is the wall" conclusion depends on.** **The resolution is methodological:**
**sysbench-memory over a 1 MiB set is cache-friendly; a streaming workload is not.** **So the 15.5 GB/s is an
artefact of working-set size, and the streaming figure - saturating at ONE A76 - is what predicts real
memory-bound throughput.** **Both measurements are correct; they measure different things.**

## DRAM efficiency is NORMAL - the "missing 20-30%" is not a fault

```
2400 MHz -> 4800 MT/s  (LPDDR5-4800 class)   governor=performance, pinned at 2400
  32-bit bus peak : 19.2 GB/s   -> measured 68% (copy)    <- normal DDR efficiency (60-75%)
  64-bit bus peak : 38.4 GB/s   -> measured 34%           <- implausibly low for streaming
```

**So the bus is 32-bit (inferred from plausibility, not proven) and the missing ~32% is refresh, bank conflicts
and read/write turnaround.** **The memory subsystem is behaving normally.**

**NEW: the DRAM controller HAS DVFS** — `/sys/class/devfreq/a020000.dmcfreq`, bins **400/800/1200/2400 MHz**,
currently 2400, governor `performance`. **A power/performance knob the map previously lacked.** **The only two devfreq
devices are the NPU and the DRAM controller - confirming the GPU has none.**

## CMA: 256 MB, fully used, and that is NORMAL

```
kernel cmdline : ... coherent_pool=4M ... cma=256M
DT reserved    : ONE region only - bl31 @0x48000000, 16 MB (ARM Trusted Firmware), no linux,cma node
CmaTotal 256 MB  ·  CmaFree 2 MB
dma-bufs alive : 7 objects, 19.6 MB total, ALL GPU render targets, all fences signalled
```

**CMA is a boot-time reservation from the cmdline, and it is MIGRATABLE - the kernel deliberately fills it with
ordinary page cache and migrates those pages out when a driver needs contiguous DMA memory.** **So `CmaFree` at
2 MB is normal behaviour on a busy 5.9 GB system, NOT accelerator occupation and NOT a constraint by itself.** **It
only becomes one if migration fails under fragmentation, which nothing here measured.**

**CORRECTED: rounds 56-57 called this "a real constraint on the accelerators". That was too strong.**

**The genuine remaining ceiling is `coherent_pool=4M`** - the pool for ATOMIC coherent allocations is 4 MB and
**cannot draw on CMA**; the GPU, VPU, NPU and display share it. **Small, but noted as small rather than measured as
exhausted.**

## The memory block, dimension by dimension

| dimension | finding |
|---|---|
| **what the silicon can do** | LPDDR5-4800 class; a 32-bit bus at ~19.2 GB/s peak; **4-bin DVFS (400/800/1200/2400)** |
| **what the driver exposes** | `a020000.dmcfreq` (devfreq, governor `performance`), `CmaTotal/CmaFree`, `coherent_pool` |
| **what userspace exists** | devfreq sysfs governor control; `/sys/kernel/debug/dma_buf/bufinfo` for dma-buf accounting |
| **used vs idle** | **DRAM pinned at 2400 MHz; CMA fully used by the page allocator (normal); 19.6 MB of live dma-bufs** |
| **deficiency** | **`coherent_pool=4M` is small for four accelerators sharing it** - **and the DRAM part number is still not exposed** |

**NOT EXPOSED: the DRAM part number** (the prior work's `YMUS8A1TE2D1C1` is the UFS chip, from SCSI inquiry - a
different device). **Recorded as unobtainable rather than guessed.**

---

# UPDATE rounds 60-66 — the correctness gate, and the open driver's GL path

## The gate, both sides, with confidence stated

| side | GL scenes (`glmark2-es2 --validate`) | confidence |
|---|---|---|
| **VENDOR** | **26 / 27 PASS** | **HIGH** - reproduced 3x, shape-specific, and **softpipe passes the scene the vendor fails** |
| **OPEN** | **0 / 8, then HANG** (`exit=124`) | **LOW - CONFOUNDED** |

**The vendor's single failure is `fragment-complexity=medium:fragment-steps=5`**, and it is **three times
independently shown to be a vendor driver bug**: deterministic 3/3, shape-specific (low and high pass at the same
step count), and **passed by software rendering**. **It also does NOT report an error - the wrong output is silent.**

## The open GL path: found, and it works at the stack level

**For the first time the open driver's GL stack initialised.** **The fix was to SOURCE the environment script rather
than rebuild it:**

```
. /home/radxa/gpu-open-stack/env26.sh
export DISPLAY=:0        # env26.sh unsets it (it targets Wayland)
glmark2-es2 --validate
```

**Result:**

```
GL_VENDOR:   Mesa
GL_RENDERER: zink Vulkan 1.3(PowerVR B-Series BXM-4-64 MC1 (IMAGINATION_OPEN_SOURCE_MESA))
GL_VERSION:  OpenGL ES 2.0 Mesa 26.3.0-devel (git-d253e35777)
```

**So GL on this board's open stack is `zink` (GL-on-Vulkan) over the OPEN PowerVR Vulkan driver, on Mesa
26.3.0-devel at this session's own HEAD.**

## Why the 0/8 is NOT a verdict on the driver

```
DRM_IOCTL_MODE_CREATE_DUMB failed: Permission denied
ZINK: vkEndCommandBuffer failed (VK_ERROR_OUT_OF_DEVICE_MEMORY)
```

**`Permission denied` on a DRM ioctl while running as ROOT is an ENVIRONMENT fault** - the client lacks DRM master or
seat access, and `env26.sh` sets `LIBSEAT_BACKEND=noop`, which is a strong candidate for exactly that. **The
out-of-device-memory error follows from being unable to allocate the dumb buffer.**

**So 0/8 measures the HARNESS, not the driver.** **Recording it as a driver verdict would be the confident-but-unfounded
claim this project has corrected 38 times.**

**To make it fair: give the client weston's seat/DRM access, or run it outside weston.**

## The switch cycle is now routine

**Five consecutive clean switch cycles this session** (`powervr` -> restore `pvrsrvkm`, kwin alive, guard active,
firmware intact, **no reboot**), against the earlier failures. **The round-15 rule holds: batch everything into ONE
switch and verify the bound driver at BOTH ends.**

## Correction count

**38 self-corrections**, including two this stretch that were my own process errors (grepping away the output that
explained a failure, twice) and two that were wrong conclusions of mine about the memory subsystem.
