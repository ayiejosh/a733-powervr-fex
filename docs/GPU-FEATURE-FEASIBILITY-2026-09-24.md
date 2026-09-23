# Feasibility: the five features both drivers lack

`descriptorIndexing` (+11 sub-features), `geometryShader`, `tessellationShader`, `multiViewport`,
`fillModeNonSolid`. None is a "catch up to the vendor" item - **the vendor driver lacks all five too** -
so each is a question of going *beyond* Imagination's own closed driver. This is what the probe found,
including the prior work on this board, which turned out to be substantial.

Method: the DDK's own hardware feature list for this exact core, the published register/instruction
definitions the open driver generates its CSB and ISA from, the Mesa driver source, and the session notes
from earlier work on this board. Read-only; nothing was changed to produce this.

## 0. Prior work on this board, found and assessed

The user's recollection was right, and it is more than a note - there is a committed layer and a
full DXVK branch.

| artefact | where | what it is |
|---|---|---|
| `gpu/vk-feature-strip/` | **this repo, committed** | a Vulkan layer that fakes `geometryShader` and `VK_EXT_robustness2`/`nullDescriptor` at query time and strips them back out of `VkDeviceCreateInfo`, so zink accepts a driver that has neither |
| `gs-compute` branch @ `3dd76cf` | `/home/radxa/gpu-experiment/DXVK-Sarek` | geometry-shader emulation by lowering DXBC GS to a compute shader plus indirect draw |
| `session-notes/pathGS-progress.md` | `/home/radxa/gpu-experiment/` | 466 lines: the GS emulation's full record, M1-M4 |
| `session-notes/pathA-findings.md`, `pathB-findings.md`, `zink-probe.md` | same | the GS feasibility study, the "can Mesa pvr run on the closed kernel" study, and the zink breakthrough |
| `gpu/zink-trixie.md` | **this repo, committed** | the zink write-up and the recorded ceiling |

**What the prior work established, and it holds up:**

* GS on this hardware cannot be run natively - a GS pipeline **crashes at Draw (RC=3)** on the vendor
  blob, and the blob reports `geometryShader=0`.
* A compute-based GS emulation is architecturally sound *and was made to render correctly*:
  `M3 Gate-1 CLOSED: gs.exe renders (GS_OK, green, RC=0)`.
* Its cost was **measured, not estimated**: `0.9428 ms/frame` baseline against `75.4813 ms/frame`
  emulated, 2000 frames x 64 draws, warmup-verified in both modes. **~80x**, and the analysis of why is
  honest - the emulation turns one native draw into a serialized multi-pass chain (capture dispatch,
  counter clear, GS dispatch, counter-to-indirect copy, `drawIndirect`), with driver-inserted barriers
  between each, so 64 draws/frame means 64 serialized chains. It is per-draw overhead, not GS math.
* The zink ceiling was **GL 2.1 / GLES 2.0**, with `fillModeNonSolid` named by zink as the missing base
  requirement.

**Two things in the prior record need correcting, and they are the reason this probe was worth doing:**

1. **`robustness2` is no longer a real gap on the open driver.** The layer exists partly because the
   *vendor* blob has no `VK_EXT_robustness2`/`nullDescriptor` at all. The open driver now advertises both
   (it is in `regress.sh` that the vendor ICD "has no nullDescriptor, so zink cannot start on it" while
   the open one does). One of the three blockers the old note named is closed, on the open side only.
2. **The GLES 2.0 ceiling is not a blob artifact.** It reproduces *identically* on the open driver:

   ```
   vendor + PVR_strip layer + system Mesa 25.0.7:
     context GLES 3 -> rejected (egl error 0x3005)   GL_VERSION: OpenGL ES 2.0 Mesa 25.0.7
     WARNING: ... doesn't support base Zink requirements: feats.features.fillModeNonSolid
   open driver + this repo's Mesa 25.3.0:
     context GLES 3 -> rejected (egl error 0x3005)   GL_VERSION: OpenGL ES 2.0 Mesa 25.3.0
   ```

   Same ceiling, same rejection code, on two different drivers, two different Mesa versions and two
   different EGL platforms. So the ceiling comes from the *device's Vulkan feature set*, which both
   drivers share, and not from the blob. (`glceiling.sh` was added to measure this; note that
   `regress.sh` runs GL with `MESA_GLES_VERSION_OVERRIDE=3.2`, which forces the reported version and
   therefore hides the ceiling - that override is why this was not visible before.)

   What it is *not*: zink's own caps. `zink_screen.c:998` sets `caps->glsl_feature_level = 460`
   unconditionally, and the base-requirement list in Mesa main no longer contains `fillModeNonSolid`
   (`zink_screen.c:2929-2934`: `logicOp`, `scalarBlockLayout`, `KHR_maintenance1`,
   `EXT_custom_border_color`, `EXT_line_rasterization`). So the GLES 2.0 advertisement comes from
   somewhere else in the Mesa/EGL version path, and **that is an open lead, not a settled one**.

   **One unexplained observation from the same runs, recorded rather than smoothed over.** With the
   version override the GLES 3 path renders correctly on the open driver (`regress.sh` passes). Without
   it, both stacks fall back to the GLES 2 shader path - and there the vendor rendered 262144/262144
   pixels correct while the open driver produced **all zeros**. That may be the driver, the newer zink
   build, or the test's GLES 2 path, and the two runs change **two** variables at once (driver *and*
   Mesa/zink build), so it does not isolate anything. It is the kind of thing that is easy to lose by
   only reporting the passes, so it is written down as an open question with its confound named.

## 1. `tessellationShader` - hardware-absent, proven

The DDK ships a per-core capability list. `RGX_FEATURE_TESSELLATION` appears in **26 configuration
files, every one of them `hwdefs/volcanic/`, and zero of them `hwdefs/rogue/`**:

```
$ grep -rilE "tessell" $DDK | sed 's|.*/hwdefs/||;s|/.*||' | sort | uniq -c
     26 volcanic
$ grep -rlE "TESSELL" $DDK/hwdefs/rogue/
(no output)
```

This GPU is Rogue (BVNC 36.x, `rgxconfig_km_36.V.104.183.h`). The tessellation engine is a
**Volcanic-architecture** feature. Not a driver gap, not an emulation question: the hardware block is
not there. **Closed.**

## 2. `geometryShader` - hardware-absent, but the emulation is proven and priced

No geometry-shader capability token exists anywhere in the DDK - only `RGX_FEATURE_GS_RTA_SUPPORT`, which
is present on both Rogue and Volcanic and does not mean "geometry shader" (`RTA` is render-target array).
Combined with the prior measurement that a GS pipeline **crashes at Draw** on the blob, this is hardware
absence.

The interesting part is that hardware absence is not the end of it, because the prior work **built and
measured the alternative**:

* an emulation that lowers a GS to compute + indirect draw exists, passes `spirv-val`, and **rendered
  correctly** on this GPU;
* it costs **~80x** on the measured workload, dominated by per-emulated-draw serialization.

So the honest verdict is **not** "impossible" - it is "possible at a price that has been measured and is
bad". Worth noting for anyone revisiting it: the 80x is per *draw*, so a workload with far more vertices
per draw - or one where the emulation could be batched into fewer serialized passes - would see a much
smaller ratio. The prior record says exactly this and it is the right framing.

## 3. `multiViewport` - the strongest lead of the five

**Correction to the first version of this section**, which said the mechanism "appears to be present" on
the strength of one field and the driver being "halfway". A deeper probe found considerably more
hardware state than that, and one hard blocker that the first pass missed.

The hardware has a full multi-viewport path:

```xml
ppp.xml:170-175  TA_STATE_HEADER:
  <field name="view_port_count" start="12" end="15" type="uint">
    <doc>The number of viewport targets minus 1.</doc>          <!-- 4 bits: up to 16 -->
  <field name="pres_viewport" start="11" end="11" type="bool">
    <doc>If set, the Viewport Transform words are present.</doc>

cr.xml:761-766  PPP_CTRL.vpt_scissor
  <doc>When 0 the PPP will insert state updates on change of VPT ID. When 1 this feature is disabled.</doc>
```

The viewport transform is an **array** of 6-dword groups in the CSB, one per viewport; the scissor is an
**indexed array in memory** (`cr.xml:1147-1151` `ISP_SCISSOR_BASE`, selected per object by
`ppp.xml:318-325` `STATE_ISPDBSC.dbindex/scindex`), not a register. `vdm.xml` and `pds.xml` contain zero
viewport/scissor matches - it is all PPP/TA and CR state.

**And the driver is further along than "halfway".** `pvr_setup_viewport()` already loops over the
viewport count and emits `MAX2(1, viewport_count)` groups of six dwords, and
`header->view_port_count` is already computed from `ppp_state->viewport_count - 1`
(`pvr_arch_cmd_buffer.c:7468-7516`, `:7657-7658`, `:7768-7787`). The CSB emission is generalised
already. What is missing is bounded and enumerable:

| missing piece | where |
|---|---|
| `PVR_MAX_VIEWPORTS` is **1** | `common/pvr_limits.h:37` - pure Mesa policy, no hardware limit exists |
| `ppp_state->viewports[]` is a one-slot array | `vulkan/pvr_cmd_buffer.h:373-382` |
| two asserts spelling out the limitation | `pvr_arch_cmd_buffer.c:7126-7129` ("We don't support multiple viewport calculations.") |
| `TA_REGION_CLIP` derived from `viewport[0] ∩ scissor[0]` | `:7131-7185` - has to be reworked for N viewports |
| **`shaderOutputViewportIndex = false`** | `pvr_physical_device.c:367` - **the hard blocker: without this an app cannot legally write `gl_ViewportIndex` at all**, and the SPIR-V capability is generated from it |
| `.multiViewport = false`, `.maxViewports = 1U` | `pvr_physical_device.c:274`, `:817` |

Note `PVR_MAX_MULTIVIEW` is **6** (`pvr_limits.h:39`) - layered multiview works, so "no multiViewport
because there is no layered rendering" is false, and the driver already replays a draw once per view
index for it (`pvr_arch_queue.c:331-335`).

**Verdict: native support is plausible and probably the intended path, but unproven on silicon.** The
XML never documents how the per-vertex viewport ID selects a transform group, nor its width; and
Imagination's own DDK reports `multiViewport=false` on this exact core, which is weak evidence they do
not use it either. The alternative - replaying the draw once per viewport - is **not equivalent**: Vulkan
lets one draw contain primitives for different viewports, and there is no geometry stage here to express
a per-primitive predicate.

## 4. `fillModeNonSolid` - I was wrong; the hardware has an object type for it

**Correction: the first version of this section said "no fill/polygon/wireframe field exists in `ppp.xml`,
`cr.xml`, `vdm.xml` or `pds.xml`". That was asserted rather than grepped, and it is false.**

`ppp.xml` defines the object type the rasteriser is given, and two of its values are exactly
wireframe and point fill:

```xml
ppp.xml:100-110  OBJTYPE: TRIANGLE=0, LINE=1, SPRITE_10UV=2, SPRITE_UV=3, SPRITE_01UV=4,
                          LINE_FILLED_TRIANGLE=5, POINT_FILLED_TRIANGLE=6, ...
ppp.xml:269-270  TA_STATE_ISPA.pointlinewidth
   <doc>The width/pitch used for rendering lines, point-filled and line-filled triangles.</doc>
ppp.xml:266-268  linefilllastpixel  <doc>If set, the last pixel of a line is filled.</doc>
```

That doc sentence - "lines, point-filled and line-filled triangles" - is the hardware saying these object
types rasterise a triangle as lines or points. Mesa sets the object type from the topology at
`pvr_arch_cmd_buffer.c:6711-6712` and `:6776`, and uses neither value 5 nor 6 anywhere.

So the cheap path exists and does not involve index expansion at all: when `rs.polygon_mode` is
`LINE`/`POINT` and the topology is a triangle, select `ROGUE_TA_OBJTYPE_LINE_FILLED_TRIANGLE` /
`POINT_FILLED_TRIANGLE` instead of `TRIANGLE`. Line width and point size plumbing already exist
(`wideLines = true`, `PVR_LINE_WIDTH_MAX 16.0f`, `rs.line.width` programmed at `:6747-6762`).

Two honest caveats. Vulkan requires polygon mode to affect "only the final rasterization of polygons"
(vertices are still shaded, the polygon is still clipped and possibly culled before it applies) - an
object-type swap is the right shape for that, but whether B-Series honours values 5/6 is unverified and
needs silicon. And `VK_IMG_relaxed_line_rasterization`, which the vendor advertises, is **not** related:
it is the OpenGL diamond-exit line rule for GL emulation layers, a Zink knob with no wireframe meaning.

**This looked like the cheapest of the five to try, so it was tried - and it does not work.**

### 4.1 The experiment, and its result

Selecting the object type from `rs.polygon_mode` in `pvr_setup_isp_faces_and_control()` is a handful of
lines, and `vkrender` gained `POLYGONMODE=line|point` to test it: the same full-screen triangle, with the
result *counted* rather than pattern-matched, because a wireframe touches only its edges and a point fill
only its corners, so "a small, non-zero fraction of the target" can only pass if the mode changed
rasterisation.

Measured on the open driver, 512x512, four frames:

```
POLYGONMODE=line    polygon mode line:  0 of 262144 pixels written   FAIL   31.47 ms/frame
POLYGONMODE=point   polygon mode point: 0 of 262144 pixels written   FAIL   31.04 ms/frame
(default, FILL)     RESULT: PASS - 262144/262144 pixels correct             1.96 ms/frame
```

Two things are clear from that. The object type is **accepted** - no error, no hang, and the frame goes
from 1.96 ms to 31.5 ms, so the hardware is genuinely on a different path. And it **rasterises nothing
visible**. The line width was not the problem: `pointlinewidth` is `15` (1.0 in 4.4 fixed point) because
the driver always programmes it from `rs.line.width`.

So `LINE_FILLED_TRIANGLE` needs something else that is not in the published definitions, and the cheap
path is **ruled out by measurement rather than by argument**.

### 4.2 What was done about it

Both driver changes were **reverted**: `.fillModeNonSolid` is back to `false` and the object type is not
consulted, with a comment recording the measurement so nobody re-tries it blindly. `regress.sh` is
**28 passed, 0 failed** after the revert. The `POLYGONMODE` test mode was kept - it is a probe, it is not
in the regression suite, and it documents the negative result.

The remaining route is the fallback: triangle-to-line index expansion in `pvr_emit_vdm_index_list`,
which is a real project with documented deviations (vertex shading count and order change, lines are
clipped instead of the polygon, and polygon culling is bypassed), because the specification says polygon
mode "affects only the final rasterization of polygons".

**Verdict: not a hardware wall in principle, but the obvious native mechanism does not work, so
`fillModeNonSolid` moves from "cheapest" to "the index-expansion project".**

## 5. `descriptorIndexing` - **not one question. It splits three ways.**

The first version of this section said "unresolved", on the strength of my own reading. A deeper probe
found that the honest answer is three different answers depending on which part you mean, and that one
part is a **software gap, not a wall** - which is the single most useful result in this report.

### 5.1 Hardware wall: native bindless, i.e. the texture unit fetching a descriptor from memory

This part is real and it is proven by absence of capability, not by guesswork:

* The 46-feature capability list for this core has no bindless or indirect-descriptor bit.
* `RGX_FEATURE_BINDLESS_IMAGE_AND_TEXTURE_STATE` exists **only in the Volcanic hwdefs** - it is not
  defined for any `36.V` config and is absent from the Rogue family entirely.
* The only use of that feature in the whole DDK is to gate a **memory-resident texture-state heap**:
  `TextureStateIsPresent()` in `rgxinit.c:4035-4047` decides whether the firmware reserves
  `RGX_TEXTURE_STATE_HEAP`. On Rogue the macro is never defined, so it returns false unconditionally.
* Descriptors are not fetched from memory at all. They are **DMA'd by the PDS into the USC "common
  store"** at compile-time-assigned register indices (`pvr_arch_pipeline.c:473-527`, destination and size
  via the PDS `doffset`/`a0`/`bsize` fields against `COMMON_STORE`), and the shader names them by
  register number.

So there is no descriptor table in memory for the texture unit to index. **Bindless in the usual sense is
closed on this part.**

### 5.2 Not a wall: dynamically-uniform descriptor indexing - the compiler side already exists

The ISA has a runtime register-index mechanism. `F_REGBANK` in `pco_isa.py:326-337` includes
`idx0` and `idx1`, and the 11-bit source index field packs as `[pointee bank:3][immediate offset:8]`
(`pco_map.h.py:73-141`), which is why `ROGUE_MAX_REG_OFFSET` is 255. Only three source slots can be
index-register operands (`pco_internal.h:3009-3022`), and `O_SMP`'s operand map puts tex state in S0 and
sampler state in S2 - exactly those slots.

And upstream pco **already contains the whole lowering path**: `O_SMP_DYNIDX` / `O_SMP_WRT_DYNIDX`
(`pco_ops.py:546-554`) are consumed by `legalize_smp_dynidx()` (`pco_legalize.c:338-453`), which computes
`elem * stride` into an index register and attaches it to the tex/sampler state ref. The generic
non-sampler version is `legalize_dynidx()` (`:248-330`).

Three independent idioms in the driver agree that a hardware register number is
`IDX + immediate offset` (inferred - no ISA prose states it, but the three are mutually consistent).

**What is actually missing is Vulkan plumbing and budget, not compiler work:**

| missing piece | evidence |
|---|---|
| the master `.descriptorIndexing` and everything at the 1.2 level | `pvr_physical_device.c:342` and `:346-362` |
| runtime descriptor arrays | `pvr_descriptor_set.c:200` - `if (!binding->descriptorCount) continue;` |
| update-after-bind / partially-bound / variable count | binding flags are stored (`:222`) and never interpreted; every update-after-bind limit is hard 0 (`pvr_physical_device.c:921-935`) |
| descriptor budget for a resident heap | 1024/1024/2048 shared registers per stage (VS/FS/CS), 4 dwords per combined image+sampler, so **~256 image descriptors is the plausible ceiling, not 64k** |

**And a correction to how this was framed:** the driver is *not* missing uniform dynamic indexing. The
core-1.0 features `shaderUniformBufferArrayDynamicIndexing`, `shaderSampledImageArrayDynamicIndexing`,
`shaderStorageBufferArrayDynamicIndexing`, `shaderStorageImageArrayDynamicIndexing` are **already
advertised** (`pvr_physical_device.c:289-292`). What is false is the 1.2-level set. So the uniform path
may already work today and has simply never been tested - which is the top lead below.

**And on part of it the open driver is already ahead of the vendor.** Measured from the two audits, three
of the 1.2-level dynamic-indexing features are `1` here and `0` on the vendor:

```
vk12.shaderInputAttachmentArrayDynamicIndexing        open=1  vendor=0
vk12.shaderUniformTexelBufferArrayDynamicIndexing     open=1  vendor=0
vk12.shaderStorageTexelBufferArrayDynamicIndexing     open=1  vendor=0
```

Those are three of the four features the open driver has that the vendor does not (the fourth is
`vk13.descriptorBindingInlineUniformBlockUpdateAfterBind`). So on the dynamic-indexing axis this is not
a catch-up story at all - the open driver is past the vendor, and the question is how much further the
same mechanism can be pushed.

### 5.3 Undetermined: non-uniform (per-lane divergent) indexing

One `smp` instruction carries exactly one texture descriptor, so expressing per-lane divergent image
descriptors needs either divergent `IDX0`/`IDX1` values that the TPU can service, or a compiler
emulation that makes the index dynamically uniform per iteration. **No source in either tree answers the
divergence question**, and there is no `nir_lower_non_uniform_access`-style pass in the driver. This is
the one thing that separates "non-uniform is a wall" from "non-uniform needs emulation".

### 5.4 The buffer half is emulatable

A UBO/SSBO descriptor is not a texture-unit descriptor at all - it is a 4-dword
`{u64 addr; u32 size; u32 offset;}` in the shared file, read as registers and turned into a 64-bit
address for an ordinary global load (`pco_trans_nir.c:1317-1388`). Since buffer device address and
`nir_address_format_2x32bit_global` now work here, a genuinely per-lane dynamic lookup could be built by
putting the descriptor table in memory and giving the shader its address - and there is precedent for
delivering a 64-bit GPU address into a shader per draw (`PVR_BUFFER_TYPE_SPILL_INFO` →
`pco_trans_nir.c:1793-1834`). The missing piece is a shader-visible handle on the descriptor-set buffer,
which today is consumed by the PDS and never seen by the shader.

### 5.5 Ranked leads

1. **Prove the uniform path on hardware.** Index a descriptor array with a dynamically *uniform* index
   and see whether the TPU honours an `idx`-banked state operand. The compiler side exists; nobody has
   ever shown the hardware side works. One test decides whether the image/texel half is only plumbing.
2. **Settle divergence with a microbenchmark**: same array, index = `gl_SubgroupInvocationID & 1`, two
   descriptors, read back and compare. This is the single unknown that separates wall from emulation.
3. **Buffer half via descriptor-in-memory** (§5.4) for genuine per-lane dynamic UBO/SSBO indexing.
4. **Vulkan plumbing** (§5.2) - definitely software, independent of 1-3.
5. Only if 1 and 2 succeed, implement non-uniform emulation and advertise the `*NonUniformIndexing`
   features with `*Native = false` - which is already the posture the driver takes
   (`pvr_physical_device.c:910-914`).

## 6. The lead that is actually worth chasing first: framebuffer compression

While checking the hardware capability list, something more useful than any of the five turned up.

This core reports `FBCDC` (framebuffer compression) as a hardware capability, and Mesa even carries the
algorithm number:

```
common/device_info/bxm-4-64.h:  .has_fbcdc_algorithm = true,
                                .fbcdc_algorithm = 50U,
```

**Nothing in the driver uses it** - those lines are the only occurrences, and the CSB generator's
`cr.xml` models the state that would program it:

```xml
<field name="compression" start="3" end="3" type="bool">
  <doc>Frame buffer Compression enabled.</doc>
<field name="compress_size" start="2" end="2" type="COMPRESS_SIZE">
  <doc>Block size for the compressor ... 8x8; 16x4 or 32x2 size.</doc>
```

Why this matters more than the five features: **the actual bottleneck is bandwidth and tile cost**
(render is 2.39-2.54x slower than the vendor and that is the per-tile number from the earlier analysis),
and this is an unused hardware compression path sitting directly on it. It is also the one item here with
a *user-visible* payoff rather than a checkbox.

The open question before promising anything: whether the mainline `powervr` kernel driver can allocate a
framebuffer in a layout the compressor accepts. That is a kernel/UAPI question and it has not been
checked.

## Bottom line

| feature | verdict | lead |
|---|---|---|
| `tessellationShader` | **hardware-absent, proven** | none - Volcanic-only block, 0 Rogue configs |
| `geometryShader` | **hardware-absent**; emulation proven and priced at ~80x | the DXVK branch exists; the cost is per-draw and could be amortised |
| `multiViewport` | **hardware path exists and the CSB emission is already generalised**; unproven on silicon | `view_port_count` (4 bits, up to 16), `vpt_tgt_pres`, `PPP_CTRL.vpt_scissor`, indexed scissor array; blockers are `PVR_MAX_VIEWPORTS 1`, two asserts, and `shaderOutputViewportIndex = false` |
| `fillModeNonSolid` | hardware object types 5/6 exist but **draw nothing** - tried, measured, reverted | index expansion is the remaining route; the native path is ruled out |
| `descriptorIndexing` (+11) | **splits three ways**: native bindless is a **hardware wall**; uniform indexing is a **software gap** whose compiler path already exists; non-uniform is **undetermined** | prove the uniform path on hardware first - one test decides whether the image half is only plumbing |
| *framebuffer compression* | **not asked about, and the highest-value lead** | hardware present, device info knows it, `cr.xml` models it, driver never uses it, and it sits on the bottleneck |

Ordered by what to try first, given the corrections and the one experiment that was run:
**`descriptorIndexing`'s uniform half** (the compiler path already exists and the core-1.0 features are
already advertised - one test decides whether the rest is plumbing), then **`multiViewport`** (hardware
path identified, CSB emission already generalised, but one unproven assumption about the silicon and a
hard blocker in `shaderOutputViewportIndex`), then `geometryShader` only if the 80x can be amortised,
then `fillModeNonSolid` as the index-expansion project; `tessellationShader` is closed; and framebuffer
compression remains the only item here with a user-visible payoff.

Four of the five now carry a measured or proven answer rather than an argument: tessellation is provably
absent, geometry-shader emulation is priced at 80x, the fillModeNonSolid object type was tried and draws
nothing, and descriptor indexing turns out to be one wall, one software gap and one open question rather
than one unknown. That is a better place to stop than a list of five maybes.

The three artefacts worth keeping from this: the PVR_strip layer and the GS branch are prior work that
still stands; `glceiling.sh` is new and shows the GL ceiling is a device-level wall rather than a blob
one; and the compression lead is the only one here that would change what a user feels.

## Appendix: method and its limits

Sections 1, 2 and 6, plus the prior-work section, come from my own probe. Sections 3, 4 and 5 were
rewritten from deeper delegated probes after the first version of each was inadequate - in the
`fillModeNonSolid` case wrong because the negative claim ("no fill/polygon/wireframe field exists") was
**asserted rather than grepped**, and in the `descriptorIndexing` case because "unresolved" was a
give-up rather than an answer. Both are worth remembering: a negative grep result is only evidence if
the grep was actually run, and "I could not determine this" is worth saying only after the question has
been split into the parts that *are* determinable.

The descriptor probe also surfaced the correction that the core-1.0 uniform dynamic-indexing features are
**already advertised** - so the framing "the driver has none of descriptor indexing" was wrong in the
same direction as the fill-mode error: a claim about absence that had not been checked against the
feature table's actual contents.

What is still open and would need silicon rather than source: whether this core honours an `idx`-banked
state operand at all; whether `IDX0`/`IDX1` may hold divergent per-lane values; whether this core
implements `view_port_count > 1` and object types 5/6; the width and encoding of the per-vertex viewport
ID; and whether `PPP_CTRL.vpt_scissor` inserts viewport transforms as well as scissor/depth-bias. The
vendor's user-space capability table would answer several of these and is a stripped binary, so it cannot
be read.
