> **⚠️ CORRECTION (this session): the shading rates used throughout this analysis are ~1.8x LOW.**
> The GPU benchmark figures were re-measured as **7355 / 2211 / 574 / 147 Mpix/s** where this document uses
> **4198 / 1216 / 315 / 80**. So every `~300 Mpix/s` below should read **~574 Mpix/s**, and the headline
> feasibility verdict softens: **~70 ms/frame at 1080p becomes ~37 ms** - still above a 16.7 ms budget,
> but ~2x over rather than ~4x over. The conclusion "not viable at 1080p" stands; "impossible" is too strong.
> **The per-pass conclusions and the memory-traffic dominance argument are unaffected**, since they scale with
> pass count rather than with the absolute rate.

# LSFG 2.0 / "MAKO" — what it is, what it costs, and whether any of it belongs on an A733

Scope: desk research, 2026-10-09. No GPU/VE/NPU workload was run for this file; every
performance figure below is either **sourced** or **derived from a numbered source**, and
board-local figures are cited to files in this repo.

Convention: **[F]** = sourced fact (link given), **[I]** = inference of mine, **[?]** = unresolved.
Numbers marked *unmeasured-on-this-board* are exactly that.

---

## 0. Corrections to the premise (read this first)

Your two assumptions are both wrong, in opposite directions.

1. **"LSFG is the frame-generation technology in Lossless Scaling" — correct, but LSFG 2.0 is
   two major versions stale.** LSFG = *Lossless Scaling Frame Generation*, a paid Windows/Steam
   app by the developer who signs as **THS** ([site author page](https://losslessscaling.com/author/admin/),
   [Steam app 993090](https://store.steampowered.com/app/993090/Lossless_Scaling/)). As of this
   session the shipped line is **LSFG 3.1 + Adaptive Frame Generation + a Performance Mode hotfix**
   ([losslessscaling.com](https://losslessscaling.com/)) — LSFG 2.0 is a 2024 release
   ([Introducing LSFG 2.0](https://losslessscaling.com/introducing-lsfg-2-0/)), and 2.2 landed
   [Jul 2024](https://losslessscaling.com/framepacing-and-lsfg-updates/). **LSFG 2.0 is also the
   most expensive version ever shipped** — the vendor's own 2.0 notes say *"GPU load has increased
   by 1.5 – 2x depending on the resolution"* vs 1.1, and LSFG 3 exists largely to claw that back
   (*"A 40% reduction for X2 mode compared to LSFG 2 (non-performance mode)"*,
   [LSFG 3](https://losslessscaling.com/lsfg-3/)). So "is LSFG 2.0 feasible here" is asking about
   the worst case, not the current one. Judge LSFG 3 or ANVIL, not 2.0.

2. **"Mako is an associated renderer/codename" — wrong.** MAKO (**M**otion-**A**daptive
   **K**ernel **O**rchestration) is a **third-party, GPL-3.0 community project** by *eugeniosegala*
   that brings LSFG to SteamOS/Linux
   ([github.com/eugeniosegala/MAKO](https://github.com/eugeniosegala/MAKO)). It is not THS's
   component, not a codename for anything Inside Lossless Scaling, and per its own README *"MAKO is
   not an official Lossless Scaling, Decky Loader, lsfg-vk, vkBasalt, or ReShade release"*. It
   contains **no** model payload: it reads the user's licensed `Lossless.dll` at runtime and runs
   the extracted shaders through its own reimplemented Vulkan pipeline. The original Linux port is
   [PancakeTAS/lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) (which THS links from his own site),
   and the canonical technical account of the port is
   [Pahheb/lsfg-vk — "Porting LSFG to native Vulkan"](https://github.com/Pahheb/lsfg-vk/wiki/Porting-LSFG-to-native-Vulkan).
   MAKO is the *integration + scheduling + packaging* layer, and that layer is the part worth
   learning from. Nothing here suggests circumventing licensing; the proprietary model ships only
   inside a licensed copy and both projects respect that boundary.

---

## 1. What LSFG is, verified

| | |
|---|---|
| Product | Lossless Scaling (scaling + frame generation), paid, Windows-first |
| Vendor | **THS** — solo developer, `losslessscaling.com` |
| LSFG 1.0 | First frame-gen release; *"designed from the ground up using machine learning to run on a wide range of GPUs, including integrated graphics cards"* ([LSFG 1.0](https://losslessscaling.com/introducing-frame-generation-lsfg/)) |
| LSFG 2.0 | New architecture for large motion; min recommended base rate **30/40 FPS at 1080p/1440p**; **GPU load +1.5–2x**; adds **Performance** mode to restore 1.1 speed ([link](https://losslessscaling.com/introducing-lsfg-2-0/)) |
| LSFG 2.2 | UI detection retuned to *over-detect less*; cursor at target rate; **Max Frame Latency** option ([link](https://losslessscaling.com/framepacing-and-lsfg-updates/)) |
| LSFG 3 / 3.1 | −40% GPU load (X2) / −45%+ (>X2) vs LSFG 2 non-performance; **~24% better end-to-end latency** vs LSFG 2 (OSLTT, 40 base FPS, X2); multiplier unlocked to X20 ([LSFG 3](https://losslessscaling.com/lsfg-3/)) |
| Implementation class | **D3D11 program**, shaders PE-parsed out of `Lossless.dll` at runtime and re-emitted as SPIR-V; reimplemented as a Vulkan compute pipeline ([Pahheb wiki](https://github.com/Pahheb/lsfg-vk/wiki/Porting-LSFG-to-native-Vulkan)) |
| Hosts | Windows (native); Linux via lsfg-vk / MAKO; **x86_64 only** (see §3) |

**[F]** It is not a dedicated hardware block and it does not need engine cooperation. Both ports
work by hooking **present** and post-processing already-rendered frames — that is why the vendor can
say it works on *"games that do not have such support, including emulators"* and why the Linux
layers are Vulkan swapchain layers
([MAKO renderer architecture](https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/RENDERER-ARCHITECTURE.md)).

---

## 2. Methodology — how it actually generates the frame

Everything here is **[F]** from the vendor's pages or from MAKO's shipped configuration/architecture
docs, which are unusually explicit for a proprietary model.

**It is a learned, flow-based, multi-pass network. Not block matching on the CPU, not engine MVs.**

* **Motion is derived, not given.** LSFG never receives the game's motion vectors. Motion estimation
  is a learned step in the network. Corroborating hard evidence: MAKO exposes a setting
  **`flow_scale` = "LSFG motion-estimation resolution from 0.25–1.0"**, default **0.8**
  ([MAKO CONFIGURATION.md](https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/CONFIGURATION.md)).
  A block matcher has no "resolution"; an optical-flow field does, and the vendor confirms the
  quality/cost tradeoff: LSFG 3 lists *"improved quality at lower flow scales"*
  ([LSFG 3](https://losslessscaling.com/lsfg-3/)).
* **Internal resolution.** Flow is estimated below output resolution (`flow_scale`), and there is a
  separate **"Resolution Scale"** feature that *downscales input to 1080p* before processing — the
  vendor's own recommended values are 75% for 1440p and 50% for 4K
  ([LSFG 3](https://losslessscaling.com/lsfg-3/)). So the network is designed to run at ≤1080p
  internally regardless of output; note the vendor also states *"setting it to 90% roughly aligns
  with the LSFG 2 Performance mode"*.
* **Two model sizes.** A full-quality model and a lighter **Performance** model (MAKO calls this
  `performance_mode`, and an `ultra_performance` preset = 70% flow scale + lighter model + FP16 +
  LS1-Performance scaling). **Precision is FP16 when the GPU allows, FP32 otherwise** (`allow_fp16`).
* **Graph shape.** MAKO's memory doc names the model stages — **Alpha0, Alpha1, Gamma0, Gamma1,
  Delta1** — with a prepass, per-output recorded command sets, a **six-phase recording period
  derived from history counts and two source-image slots**, and "history-only" steps
  ([MEMORY-MANAGEMENT.md](https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/MEMORY-MANAGEMENT.md)).
  **[I]** That is a recurrent/warping-refinement network with temporal history (previous
  synthesised frames fed back), i.e. an optical-flow + backward-warp + learned-refinement design of
  the same *family* as RIFE/IFRNet, not a novel class. "Many LSFG model intermediates" are pooled in
  4–32 MiB blocks, so this is a **multi-pass, many-intermediate** graph — pass count, not FLOPs, is
  the shape of the cost.
* **UI handling is a learned detector, not a mask.** LSFG 1.1 shipped *"a new UI detection model"*
  ([link](https://losslessscaling.com/lsfg-1-1-new-ui-detection-model/)); 2.2 then had to *reduce
  over-detection*, which the vendor states had been causing artifacts. Cursor is composited at the
  target rate rather than generated. **[I]** The lesson is that UI protection is a heuristic you
  tune both ways, and over-protecting is itself a visible bug.
* **Disocclusion:** no public documentation. **[F]** The vendor's only public guidance is that
  artifacts fall as base framerate rises (*"in cases where artifacts persist, it is advisable to
  increase the base frame rate"*). **[I]** That is the signature of a flow+history model with no
  explicit occlusion reasoning — it degrades rather than breaking.

---

## 3. Cost, and where it runs

**Cost published by the vendor is relative, never absolute.** There is **no official ms figure for
LSFG at any resolution** — I looked; the site only gives load deltas versus previous versions. Any
"LSFG costs X ms" you find online is a third-party measurement with its own hardware and settings.
State of the sourced record:

| Quantity | Value | Source |
|---|---|---|
| LSFG 2.0 GPU load vs LSFG 1.1 | **+1.5–2x**, resolution-dependent | [Introducing LSFG 2.0](https://losslessscaling.com/introducing-lsfg-2-0/) |
| LSFG 3 X2 load vs LSFG 2 non-perf | **−40%** | [LSFG 3](https://losslessscaling.com/lsfg-3/) |
| LSFG 3 >X2 vs LSFG 2 non-perf | **−45%** | [LSFG 3](https://losslessscaling.com/lsfg-3/) |
| LSFG 3 end-to-end latency vs LSFG 2 | **~24% better** (OSLTT, 40 base FPS, X2) | [LSFG 3](https://losslessscaling.com/lsfg-3/) |
| Resolution Scale 90% | ≈ LSFG 2 Performance mode | [LSFG 3](https://losslessscaling.com/lsfg-3/) |
| Min base rate, 1080p | **30 FPS** (60 preferred) | [LSFG 2.0](https://losslessscaling.com/introducing-lsfg-2-0/), [LSFG 3](https://losslessscaling.com/lsfg-3/) |

**Latency is the real, measured cost**, and it is large. Gamers Nexus measured click-to-pixel with a
photodiode latency tester on a **Ryzen 7 9800X3D + RTX 5060 Ti** (reported by
[VGTimes, 2025-12-08](https://vgtimes.com/tech-and-hardware/140737-the-smoothness-tax-independent-tests-measure-how-lsfg-impacts-game-responsiveness.html);
underlying video: [youtu.be/GDvfIbRIb3U](https://youtu.be/GDvfIbRIb3U) — **[I]** I did not
re-verify the video, so treat as secondhand):

* Cyberpunk 2077, low: **25.3 ms → 44.8 ms** at X2 (**+19.5 ms, +77%**), while real framerate rose
  only 39%.
* Rainbow Six Siege (base ~10 ms): **→ 21.4 ms** at X2.
* X20 multiplier: **~139 ms** average, with the worst 5% at **170 ms**.
* The **dual-GPU** mode (a GTX 1060 doing generation while the main GPU renders) only helps when the
  game is *uncapped*; with a 60 FPS cap the benefit vanished and the second card sometimes made
  things worse.

**[I]** The one-line conclusion from that data: at X2 you are trading ~20 ms of added latency for
~40% more frames. The vendor's own mitigation is to cap base framerate so the render GPU is not at
100%, which is the opposite of what a weak GPU can afford.

**Where it runs:** on the **general-purpose GPU, as compute shaders**, in the present path. Not a
dedicated block, not the NPU, not the video engine. Two model sizes and an FP16/FP32 switch are the
only knobs. **[F]** There is no published low-power variant, and MAKO's own AArch64 policy (§4)
shows the maintainers do not treat low-power ARM as reachable today.

---

## 4. Feasibility on the A733

### 4a. Porting LSFG itself: **impossible**, and the upstream projects agree

* The model is a proprietary **x86_64 D3D11 payload** inside `Lossless.dll`. MAKO extracts and
  re-emits its shaders at runtime; there is no ARM build and no legal path to one.
* MAKO **fail-closes on native AArch64 by design**: *"the current MAKO Renderer payload is **not
  supported or activated there**… Installation is disabled"*, gated on PID-1 ELF identity and
  machine name, with **six** explicit gates (reviewable AArch64 build, FEX/Turnip payload matrix,
  Vulkan loader activation, real Armada/Turnip hardware validation, FP32/FP16 quality comparison,
  tested rollback) before a `host_architectures: ["aarch64"]` package may even declare itself
  installable — [MAKO plugin/docs/ARMADA.md](https://github.com/eugeniosegala/MAKO/blob/main/plugin/docs/ARMADA.md).
  It also explicitly forbids *"download an opaque AArch64 binary"*. And MAKO's shipped packages are
  *"x86_64 Linux hosts… Native AArch64/Armada packages are not included in this release"*
  ([README](https://github.com/eugeniosegala/MAKO)).

**[I]** That is the correct engineering posture and the honest answer: nobody has shown LSFG working
on ARM, and the maintainers consider it unreachable without hardware validation.

### 4b. Board ceilings (measured here, cited to this repo)

| Resource | Measured | Source |
|---|---|---|
| GPU | PowerVR **BXM-4-64 MC1**, BVNC 36.56.104.183, 1 core, Vulkan 1.3.277, GLES 3.2, OpenCL 3.0 | [POWERVR-SITUATION-2026-09-22.md](../Main/POWERVR-SITUATION-2026-09-22.md) |
| GPU clock | default 600 MHz; **1104 MHz** achievable and stable (vendor GLES) | [PERFORMANCE-RESEARCH-2026-09-22.md](../Main/PERFORMANCE-RESEARCH-2026-09-22.md) |
| GPU fill, light shader (vendor GLES `glbench`) | **4190 Mpix/s at 600 MHz**; **7428–7641 Mpix/s at 1104 MHz** | [BENCHMARK-COMPARISON-2026-09-21.md](../Main/BENCHMARK-COMPARISON-2026-09-21.md), [PERFORMANCE-RESEARCH](../Main/PERFORMANCE-RESEARCH-2026-09-22.md) |
| GPU fill, heavier shader (600 MHz) | loop16 **1215**, loop64 **315**, loop256 **80 Mpix/s** | [BENCHMARK-COMPARISON](../Main/BENCHMARK-COMPARISON-2026-09-21.md) |
| Vulkan client render, marginal | **321 Mpix/s**, plus **~0.93 ms fixed per frame**; 1080p = 7.4 ms bare fill | [GPU-FIRMWARE-RE-2026-10-06.md](../Recovery/GPU-FIRMWARE-RE-2026-10-06.md) |
| Vulkan texture sampling | **200 Mpix/s** = 1.5x fill | same |
| **Windowed/composited 1080p frame** | **76.9–90.9 ms (13 FPS)**; compositor term **64–70 ms = 30 Mpix/s**; KMS-no-compositor path is fine at 55.8 FPS | same |
| Vulkan gaps that matter | **no `VK_EXT_descriptor_indexing`** (no bindless), **no texture compression at all** (BC/ETC2/ASTC all false), no `VK_EXT_memory_budget`, `maxComputeWorkGroupInvocations=512`, `maxComputeSharedMemorySize=16 KiB`, `maxImageDimension2D=8192`, `timestampPeriod=512 ns` | [POWERVR-SITUATION](../Main/POWERVR-SITUATION-2026-09-22.md), [GPU-FIRMWARE-RE](../Recovery/GPU-FIRMWARE-RE-2026-10-06.md) |
| `shaderFloat16` / `shaderInt8` | both **true** (int8 only after a driver-limit fix) | [GPU-FIRMWARE-RE](../Recovery/GPU-FIRMWARE-RE-2026-10-06.md) |
| NPU | VIP9000, `vipcore` bound, `/dev/vipcore`, **no userspace**; **3 TOPS** per vendor | [BLOCK-INVENTORY.md](BLOCK-INVENTORY.md), [Radxa Cubie A7A docs](https://docs.radxa.com/en/cubie/a7a) |
| VPU | Cedar VE2: H.264/H.265/VP9 **decode proven**; H.264 encode **74 FPS @1080p on ~0.4 core**. Codec only. | [BLOCK-INVENTORY.md](BLOCK-INVENTORY.md), [LEAD-INTEGRATION-FINDINGS.md](LEAD-INTEGRATION-FINDINGS.md) |
| CPU per-pixel ceiling | 8-core NEON ≈ **28 Mpix/s light, 0.46 Mpix/s heavy** — *supplied with the task, parent-session measurement, not re-derived here* | tasking |
| CPU that does work | libyuv NEON `ARGBToNV12Matrix`: **387 FPS / 2.58 ms per 1080p frame** (30x over libswscale) | [LEAD-INTEGRATION-FINDINGS.md](LEAD-INTEGRATION-FINDINGS.md) |
| DRAM | LPDDR5 @ 2400 MHz; **effective bandwidth UNMEASURED on this board** | tasking |

Two corrections to the figures in the brief: the **~4.19 Gpix/s** light-shader fill is the
**600 MHz** number, not the 1104 MHz number (which is **~7.5 Gpix/s**); and the fill rate is *not*
what a network shader sees — every step from loop4 → loop256 collapses throughput **~52x**
(4190 → 80 Mpix/s). Also note the independent convergence: `glbench loop64` = 315 Mpix/s and the
Vulkan client's measured marginal rate = **321 Mpix/s** — two unrelated methods agree that
**~300 Mpix/s is the realistic shading rate for non-trivial work on this GPU**, and neither has
texture compression available.

### 4c. Estimated budget for LSFG-class interpolation at 1080p

Anchor: **[F]** ANVIL, the current state of the art for *mobile* 1080p x2 interpolation
([arXiv 2603.26835](https://arxiv.org/abs/2603.26835)): **12.8 ms NPU-only, int8, 1080p** on a
Snapdragon 8 Gen 3 **Hexagon V75**, expanding to **28.4 ms end-to-end median** (94.9% of 54,623
frame pairs inside a 33.3 ms budget over 30 min). Its own stage breakdown is the template:

| ANVIL stage | HW | ms | Apply to A733 |
|---|---|---|---|
| MV densify + 4x downsample + YUV pack | CPU | 2.9 | scale by NEON ceiling → **3–8 ms** |
| median5 + Gauss σ=2 + warp + quantize | GPU (Vulkan compute) | 3.7 | **10–30 ms** at ~300 Mpix/s with no texture compression |
| 12 MB uint8 buffer copy | CPU | 0.9 | ~same |
| INT8 residual net, 1080p | NPU | 17.0 | **60–190 ms** — see below |
| dequant + residual + RGB→YUV420 | GPU | 3.3 | **10–30 ms** |
| **total** | | **28.4** | **≈ 85–260 ms** |

The NPU line: Hexagon V75 in an SD 8 Gen 3 is a **~45 TOPS-class** INT8 engine **[I]** (the 45 TOPS
figure is Qualcomm's widely-quoted platform number; the ANVIL paper itself does not state it, so treat
the ratio as order-of-magnitude only); the A733's VIP9000
is **3 TOPS** per Radxa. That is a **~15x** ratio, so ANVIL's 12.8 ms residual becomes **~190 ms** if
it scaled linearly — and it will not scale linearly, because VIP9000 has **no userspace at all**,
needs a different toolchain, and the ANVIL paper's central finding is that int8 *is* the fragile part
(*"quantized accumulation on recurrent flow states"* collapses iterative methods; RIFE/IFRNet lose
quality; `grid_sample` is 3.2x on HTP and **unavailable in MediaTek's public SDK** while consuming a
whole frame budget on the other platform). **[I]** Optimistic floor: 60 ms. Realistic: 150–200 ms.

GPU-only alternative (`flow_scale` ≈ 0.8, full-res warp + blend):

* **[I]** A LSFG-2-class graph is ≥8–10 full-resolution-equivalent passes. 2.07 Mpix × 10 = **~21 Mpix
  of shading**. At the board's realistic **~300 Mpix/s** that is **~70 ms/frame**; at the
  loop256-class rate (80–145 Mpix/s) it is **150–260 ms**.
* Traversal cost matters as much: each pass reads+writes a 1080p RGBA image ≈ **8.3 MB**, so 10 passes
  ≈ **166 MB of DRAM traffic per generated frame**. At an assumed **5–10 GB/s** (*unmeasured*; the
  brief's own placeholder) that is **17–33 ms** of pure bandwidth — before compute. **No texture
  compression exists on this GPU**, so that traffic is not reducible by format choice.

CPU-only: at **28 Mpix/s** light, one full-res 1080p pass is **74 ms**. Two passes is 148 ms. **[I]**
A CPU-side interpolator at 1080p is not a marginal case, it is off by an order of magnitude; the CPU
can only afford the low-resolution MV-densify stage.

### 4d. Verdict

**IMPOSSIBLE at 1080p for game frame generation today; MARGINAL at ≤720p for 30→60 content
playback, and only with a decoder-motion-vector design and a much smaller residual than ANVIL's.**

| Target | Budget | Estimated cost | Verdict |
|---|---|---|---|
| 1080p, 60→120 (game) | 16.7 ms | 85–260 ms | **impossible** (5–15x over) |
| 1080p, 30→60 (video) | 33.3 ms | 85–260 ms | **impossible** |
| 720p, 30→60, MV-prior + tiny residual | 33.3 ms | 25–70 ms | **marginal** — best case only |
| 720p, CPU only | 33.3 ms | >100 ms | **impossible** |

**Dominant cost, in order:**

1. **Pass count × full-resolution memory traffic.** The graph is memory-bound, not compute-bound. The
   ANVIL paper measures this directly: in RIFE, **95% of inference cycles are memory-bound** ops
   (Resize, GridSample, elementwise) and only **5.1%** are compute-bound convolutions — which is why
   int8 acceleration buys almost nothing, and why the paper had to *remove* flow and resampling from
   the accelerator graph to make it work at all.
2. **The NPU is 15x too small** for a 1080p residual network, and has no userspace installed.
3. **The GPU's realistic shading rate (~300 Mpix/s, no texture compression, no bindless) plus the
   board's broken 1080p composited path (13 FPS, 64–70 ms of it in the compositor)** — even a
   perfect interpolator cannot be added to a path that already can't composite 1080p at 30 FPS.
4. **DRAM bandwidth is likely binding** and is *unmeasured* on this board. It should be measured
   before any further work: `dd` to /dev/null is not the answer, a compute-shader streaming test at
   1080p is. Until then 5–10 GB/s is an assumption, and the honest statement is that the compute
   estimate and the bandwidth estimate are of the same order (tens of ms), so neither obviously
   dominates — which is exactly what "memory-bound" means.

The VPU cannot help: Cedar VE2 is a codec engine with no general compute. Its *only* relevance is
the motion vectors it already computes for free — §6.

---

## 5. Open alternatives with numbers

Prefer the ANVIL operator benchmark ([arXiv 2603.26835](https://arxiv.org/abs/2603.26835)) over repo
READMEs, because it is the only source I found that measures these models **on mobile silicon at
1080p under int8**. Its Table II covers 17 operators from 9 VFI methods on Hexagon V75 and MediaTek
APU 790, normalized to `Conv 3x3 = 1.0x`.

| Method | Params | Mobile 1080p reality | Verdict here |
|---|---|---|---|
| **ANVIL** | small (2 sizes) | **12.8 ms** int8 NPU-only, SD 8 Gen 3 V75; 28.4 ms end-to-end; 94.9% within 33.3 ms over 30 min. MediaTek: 24.4 ms (D9300), 25.5 ms (D9400+) | **The one that works** — but see §6, it needs decoder MVs |
| **RIFE HDv3** | 3.04 M | int8 **360p = 14.7 ms** on V75; FP16 fails 33.3 ms **even at 360p**. 95% memory-bound cycles. `grid_sample` = **3.2x** on HTP, **unsupported** on MTK public SDK | **Not viable at 1080p**; RIFE 360p costs more than ANVIL 1080p |
| **IFRNet** | 5.0 M, 4-level iterative refinement | exports/compiles but *"archived artifacts do not establish a quality-preserving 1080p real-time operating point"*; frame-output path **loses 2.95 dB** under unified PTQ; still needs a full-res warp | **Not viable** |
| **AMT-S** | 3.0 M, 3-stage correlation + 32x GridSample | **fails HTP context binary generation entirely** ("Graph Finalize failure") | **Not deployable on that accelerator** (structural, not an implementation bug) |
| **SoftSplat** | — | splatting avoids `grid_sample`, but scatter/accumulate — **[I]** likely unmappable like GridSample; residual-add already 2.3x on HTP | No mobile numbers found → **not realistic** |
| **GMFlow** | — | flow estimator, attention-based. **Self-attention OOMs at 1080p on both NPUs** | **Not viable** |
| **GMFSS / GMFSS_union** | heavy | flow + fusion of the above; no mobile measurements found | **Not realistic** |
| **FILM** | large | designed for quality/large motion, not real-time; no mobile numbers found | **Not realistic** |
| **Hardware MEMC** (TV SoCs, MediaTek MiraVision "MEMC"/Smart Frame Insertion) | fixed-function | dedicated motion-estimation/motion-compensation silicon; exists for *video*, not exposed to games. **[?]** cost/power not published | Not available on A733; nothing to port |
| **NVIDIA Optical Flow Accelerator / AMD AFMF** | fixed-function / driver-level | dedicated flow HW or driver-side frame gen, no engine MVs. A733 has no equivalent block | N/A |

**Cheapest credible published path to one interpolated 1080p frame on low-power silicon: 12.8 ms**
(ANVIL, int8, SD 8 Gen 3 HTP V75) — **28.4 ms end-to-end**. On this board, scale that by the NPU gap.
**[I]** If you want a single "compute floor" to quote: *a 1080p x2 interpolation frame needs a
~45 TOPS-class int8 accelerator, a GPU with texture compression, and decoder MVs — the A733 has none
of the three.*

---

## 6. Reusable methodology — what transfers even though the product cannot

This is the part that pays for the research. Ordered by value to this board.

1. **Use decoder motion vectors as flow priors; do not learn optical flow.** This is ANVIL's entire
   contribution and it is the single most reusable idea here. The VPU already computes per-macroblock
   MVs as a by-product of decode — for **free**. ANVIL's pipeline is: CPU densifies + 4x downsamples
   the MVs (2.9 ms), GPU median-filters, blurs and sub-pixel remaps them (3.7 ms), then a *small*
   int8 conv residual (13–17 ms) refines what's left. It removes learned flow, `grid_sample`, and
   iterative accumulation from the accelerator graph — the three things that make everything else
   fail on mobile. **[I] The open question for this board:** does the Allwinner `libvdecoder` /
   `cedrus` / `sunxi_ve` path on A733 expose per-macroblock MVs? The repo's recovery notes describe
   decode at the frame level only and never mention MV output — that is **unknown [?]**, and it is
   the *first thing to test*, because if MVs are available this board has an asset no ARM GPU
   interpolation library can match; if they are not, the whole approach loses its cheap motion source.
2. **Do motion estimation at reduced resolution, always.** `flow_scale` 0.25–1.0 (default 0.8) in
   MAKO; quarter-res in ANVIL. Cost is resolution², and both vendors treat downscaling as the primary
   cost knob. On A733, 1080p→540p flow estimation is a 4x saving for a modest quality loss.
3. **Optimise for memory bandwidth, not FLOPs.** ANVIL's wins were all bandwidth wins: fusing
   upsample+warp+blend+int8-quantize into **one** Vulkan dispatch collapsed ~71 MB of float32
   intermediates into a single uint8 buffer and cut the CPU→NPU copy from ~8 ms to 0.9 ms; moving
   dequant+residual+RGB→YUV to a GPU shader turned 11–21 ms (CPU, big.LITTLE-variable) into 3.3 ms.
   MAKO does the same thing statically: one `ImageMemoryPool` for all model intermediates, and
   explicit scratch borrowing (Gamma1 borrows Alpha0's images after Alpha1 consumes them; Delta1
   reuses Gamma0's outputs) — it *rejects overlapping input sets* rather than silently aliasing.
   **[I]** On a board with no texture compression and an unmeasured DRAM ceiling, this is the whole
   game: fuse passes, keep intermediates at 8-bit, and never round-trip to host memory.
4. **Ship the pipeline, not the stages.** ANVIL's 12.8 ms NPU number becomes 28.4 ms end-to-end —
   **2.2x overhead** — and the overhead is CPU/GPU/copies/sync, not the network. Any budget built
   from a model's inference time will be wrong by 2x. Budget the pipeline.
5. **Latency accounting is a feature, not an afterthought.** +19.5 ms at X2, and the mitigation is
   to keep the render GPU under 100% and cap base framerate. **[I]** On a board whose 1080p
   *composited* path already costs 76.9 ms (13 FPS, 64–70 ms of it unattributed WSI/compositor work),
   adding any interpolator makes the experience worse, not smoother. **Fix the compositor path
   first**; that is a prerequisite, and it is measured and in scope per
   [GPU-FIRMWARE-RE-2026-10-06.md](../Recovery/GPU-FIRMWARE-RE-2026-10-06.md).
6. **UI masking: learned/conservative detection, with a separate UI pass at output rate.** LSFG
   detects UI heuristically and had to *reduce* over-detection in 2.2 because false positives were
   themselves the artifact. The transferable rule for a compositor-side implementation: never
   interpolate a layer you cannot prove is static — composite the cursor/UI overlay at the target
   rate on top instead.
7. **Scheduling and graceful degradation.** MAKO's scheduler pattern is worth copying verbatim in
   spirit: observe cadence, probe workload, promote only when a level reaches <98% of target;
   **recover on acquire-timeout or budget exhaustion, not on ordinary slow frames**; spend at most
   one retirement-protected retry after a rejection and never loop; sample memory at admission
   boundaries rather than polling on a timer
   ([ADAPTIVE-VALIDATION.md](https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/ADAPTIVE-VALIDATION.md),
   [MEMORY-MANAGEMENT.md](https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/MEMORY-MANAGEMENT.md)).
   Note that MAKO's admission sampling needs `VK_EXT_memory_budget`, which **this GPU lacks** — probe
   the device-local heaps directly instead.
8. **Fail closed on unvalidated architecture.** MAKO's ARMADA policy — refuse to activate on AArch64,
   require a reviewable native build and on-hardware validation gates before publishing — is the
   right template for anything built here. It is also the reason not to expect an LSFG port.

---

## 7. Conflicts, marketing-only sources, and unresolved items

* **No absolute LSFG cost in ms exists from primary sources.** The vendor publishes only relative
  load deltas. I am stating this rather than inventing a figure.
* **[?]** The 13x gap between the vendor GLES `glbench loop4` rate (4190 Mpix/s @600 MHz) and the
  Vulkan client's marginal fill (321 Mpix/s) is **not fully reconciled** in this repo; the
  [GPU-FIRMWARE-RE](../Recovery/GPU-FIRMWARE-RE-2026-10-06.md) notes treat 321 Mpix/s as "the
  driver's raw render" and 30 Mpix/s as the compositor. For an interpolator the conservative number
  is the right one (Vulkan compute, short bursts, no compression), and it happens to agree with
  `glbench loop64`.
* **[?]** DRAM bandwidth on this board is **unmeasured**. Every bandwidth number above is derived
  from the brief's 5–10 GB/s placeholder and is labelled as such.
* **[?]** Whether the A733 decoder can emit per-macroblock motion vectors is **unanswered** and is
  the highest-value single experiment this research identifies.
* **Secondhand:** the Gamers Nexus latency figures reach me through VGTimes and I did not verify the
  source video. **[I]** The numbers are internally consistent and match the vendor's own admission
  that latency improves ~24% in LSFG 3, so I treat them as reliable but not primary.
* **Not usable:** PC Games Hardware's LSFG latency article is paywalled (`[+]`); notebookcheck and
  windowsforum served JS challenges and were not retrieved. No claim above rests on them.
* **Version hazard:** anything written about "LSFG 2.0" describes 2024 code. LSFG 3.1 (2026) is
  ~40% cheaper and ~24% lower latency; a feasibility judgement should use LSFG 3, not 2.0.

---

## 8. Sources

**Primary (vendor)**
* [losslessscaling.com](https://losslessscaling.com/) — product, options, news index
* [Introducing Frame Generation — LSFG 1.0](https://losslessscaling.com/introducing-frame-generation-lsfg/) — ML-based, integrated-GPU target
* [Introducing LSFG 2.0](https://losslessscaling.com/introducing-lsfg-2-0/) — large-motion architecture, +1.5–2x load, Performance mode
* [Framepacing and LSFG updates (2.2, 2024-07-18)](https://losslessscaling.com/framepacing-and-lsfg-updates/) — UI detection, cursor, Max Frame Latency
* [LSFG 1.1 — New UI detection model](https://losslessscaling.com/lsfg-1-1-new-ui-detection-model/)
* [LSFG 3](https://losslessscaling.com/lsfg-3/) — −40%/−45% load, −24% latency, Resolution Scale, X20
* [Steam — Lossless Scaling (app 993090)](https://store.steampowered.com/app/993090/Lossless_Scaling/)

**Primary (Linux ports / integration)**
* [PancakeTAS/lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) — original Linux port
* [Pahheb/lsfg-vk wiki — "Porting LSFG to native Vulkan"](https://github.com/Pahheb/lsfg-vk/wiki/Porting-LSFG-to-native-Vulkan) — the D3D11→DXVK→Vulkan porting account
* [eugeniosegala/MAKO](https://github.com/eugeniosegala/MAKO) — integration layer (README)
* [MAKO CONFIGURATION.md](https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/CONFIGURATION.md) — `flow_scale`, `performance_mode`, `allow_fp16`, `ultra_performance`
* [MAKO MEMORY-MANAGEMENT.md](https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/MEMORY-MANAGEMENT.md) — model graph, pools, scratch borrowing
* [MAKO RENDERER-ARCHITECTURE.md](https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/RENDERER-ARCHITECTURE.md) — swapchain layer, scheduler boundary
* [MAKO plugin/docs/ARMADA.md](https://github.com/eugeniosegala/MAKO/blob/main/plugin/docs/ARMADA.md) — AArch64 fail-closed policy and gates

**Primary (research)**
* [ANVIL: Accelerator-Native Video Interpolation via Codec Motion Vector Priors, arXiv:2603.26835](https://arxiv.org/abs/2603.26835) — 12.8 ms int8 1080p on SD 8 Gen 3; end-to-end 28.4 ms; operator compatibility tables; int8 collapse analysis

**Secondary**
* [VGTimes — "The Smoothness Tax" (2025-12-08), reporting Gamers Nexus](https://vgtimes.com/tech-and-hardware/140737-the-smoothness-tax-independent-tests-measure-how-lsfg-impacts-game-responsiveness.html) — click-to-pixel latency; [source video](https://youtu.be/GDvfIbRIb3U)
* [Radxa Cubie A7A docs](https://docs.radxa.com/en/cubie/a7a) — A733: BXM-4-64 MC1, Vulkan 1.3, OpenCL 3.0, **3 TOPS NPU**

**Board-local measurements (this repo)**
* [Main/POWERVR-SITUATION-2026-09-22.md](../Main/POWERVR-SITUATION-2026-09-22.md) — GPU stack, 128 Vulkan extensions, missing extensions and limits
* [Main/PERFORMANCE-RESEARCH-2026-09-22.md](../Main/PERFORMANCE-RESEARCH-2026-09-22.md) — GPU clock sweep to 1104 MHz, glbench at each clock
* [Main/BENCHMARK-COMPARISON-2026-09-21.md](../Main/BENCHMARK-COMPARISON-2026-09-21.md) — `glbench.loop4/16/64/256.Mpix`
* [Recovery/GPU-FIRMWARE-RE-2026-10-06.md](../Recovery/GPU-FIRMWARE-RE-2026-10-06.md) — `vkrender`/`vktex` fill and sampling rates, compositor penalty, FP16/int8 feature state
* [MAPS/BLOCK-INVENTORY.md](BLOCK-INVENTORY.md) — VPU decode/encode proven, NPU bound with no userspace
* [MAPS/LEAD-INTEGRATION-FINDINGS.md](LEAD-INTEGRATION-FINDINGS.md) — VPU encode at 74 FPS/0.4 core, libyuv NEON conversion
