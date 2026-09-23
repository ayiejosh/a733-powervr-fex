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

## 3. `multiViewport` - the one with a real hardware lead

Unlike the first two, the hardware mechanism appears to be present and **the driver is already halfway
to using it**. `TA_OUTPUT_SEL` in `csbgen/rogue/ppp.xml` has:

```xml
<field name="vpt_tgt_pres" start="19" end="19" type="bool">
  <doc>If set, viewport target is present, this is always assumed to be the last thing in the vertex except render target if present.</doc>
</field>
<field name="render_tgt_pres" start="19" ...>   <!-- render target / layer -->
```

and `pvr_arch_cmd_buffer.c` already programs them from whether the vertex shader writes the
corresponding varying:

```c
const bool has_viewport = varyings[VARYING_SLOT_VIEWPORT].count > 0;
const bool has_layer    = varyings[VARYING_SLOT_LAYER].count > 0;
...
state.vpt_tgt_pres = has_viewport;
state.render_tgt_pres = has_layer;
```

That is per-vertex viewport selection being carried into the VDM already - i.e. the piece
`multiViewport` needs at the *shader* level. What is missing is the viewport/scissor **array** state and
per-viewport transforms. **This is the most concrete implementation lead of the five**, and it is
checkable: count how many viewports the VDM/PPP can hold, and whether the viewport transform is indexed.

## 4. `fillModeNonSolid` - no polygon-mode field, and zink only warns

No fill/polygon/wireframe field exists in `ppp.xml`, `cr.xml`, `vdm.xml` or `pds.xml`. There is no
hardware polygon mode. Two things make this less urgent than it looks:

* GLES has no `glPolygonMode` at all, and in Mesa main `fillModeNonSolid` is **no longer on zink's base
  requirement list** (`zink_screen.c:2929-2934`). It was on the list in system Mesa 25.0.7, which is
  where the old note's warning came from.
* The vendor's *only* zink complaint today is this one, and it is a `WARNING: Some incorrect rendering
  might occur`, not a refusal.

Emulation would mean converting triangles to lines at draw time, which changes rasterisation rules
(and which is why drivers that do it gate it carefully). **Bounded but real, with no hardware support to
lean on.** Low priority.

## 5. `descriptorIndexing` + 11 sub-features - the hard one, and honestly unresolved

This is where the probe did **not** reach a verdict, and it should be stated as such.

What is established from source:

* The device has no descriptor/bindless capability token in its 46-feature list.
* pvr's descriptor identity is a **compile-time packed immediate**: `pco_nir_tex.c:462` does
  `pco_unpack_desc(tex->texture_index, &tex_desc_set, &tex_binding)` and earlier code asserts the
  descriptor set is a scalar constant. Descriptors live in compiler-allocated "shared register" slots
  that the CPU fills before a draw.
* The ISA has an extended sampler form `I_SMP_EXTA` (`pco_map.py:1485`) whose operands include
  `('drc', ('pco_ref_get_drc', SRC(0)))` - a **register** source into the sampler. But a `drc` is a
  dependent-read counter from a small fixed enum (`_PCO_DRC_COUNT`), not an arbitrary descriptor index,
  so this is suggestive rather than decisive.

What that means: the *buffer* half of descriptor indexing (UBO/SSBO/texel buffers) could in principle be
lowered to a global memory load through an address fetched from a descriptor buffer - and the machinery
for that now exists here, because buffer device address and global loads landed in this session. The
*image* half needs the texture unit to accept a runtime-selected descriptor, and **that is the crux,
and it is unresolved**.

The strongest evidence is negative and circumstantial: **Imagination's own driver does not advertise it
on this core**, with full hardware documentation in hand. That does not prove impossibility - they may
simply not have implemented it for this generation - but it is the best available signal, and it means
anyone attempting this is on their own.

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
| `multiViewport` | **hardware mechanism present** | `vpt_tgt_pres`/`render_tgt_pres` already programmed; needs the viewport array state |
| `fillModeNonSolid` | no hardware polygon mode; zink only warns now | line conversion, changes rasterisation rules - low priority |
| `descriptorIndexing` (+11) | **unresolved** | buffer half plausibly lowerable to global loads; image half needs a runtime descriptor fetch that is not established |
| *framebuffer compression* | **not asked about, and the best lead** | hardware present, device info knows it, `cr.xml` models it, driver never uses it, and it sits on the bottleneck |

The three artefacts worth keeping from this: the PVR_strip layer and the GS branch are prior work that
still stands; `glceiling.sh` is new and shows the GL ceiling is a device-level wall rather than a blob
one; and the compression lead is the only one here that would change what a user feels.
