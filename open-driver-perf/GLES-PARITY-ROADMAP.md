# GLES parity roadmap: match and beat the vendor driver (2026-10-29)

Target: feature and performance parity with IMG's shipping **GLES 3.2 build 24.2@6603887**, then beat it.
Measured state the same day (single binary, EGL/X11 glmark2, perf governor, 2 runs):
native **740/741**, zink **776/768**, vendor **1199/1181**.

GL versions today (probe `open-perf/glinfo`):
- vendor: **ES 3.2**, 120 extensions
- native: **ES 2.0**, 59 extensions (after MRT: `GL_EXT_draw_buffers`/`GL_NV_draw_buffers`)
- zink (open Vulkan): ES 3.1, 136 extensions

## Gap matrix (vendor extensions absent from native, 88, grouped by subsystem)

**Core ES3 requirements (must land before the version can be raised at all)**
- ~~MRT: >1 colour attachment end-to-end~~ **DONE (2026-10-29, patches 0332 + 0333)**: 2 and 4
  colour attachments, per-attachment EOT emits + PBE words, fragment data0..3 -> output dwords,
  MRT GPU clear, pixel-output width derived from the dword count; `GL_EXT_draw_buffers` advertised;
  mrt_test matches zink for 1/2/4 targets and for mixed RGBA8+BGRA8 (patch 0334: PBE source data is
  packed in logical channel order, which also fixed BGRA8 targets). Open MRT sub-items: all
  attachments must be one dword per pixel; no per-render-target blend state (ES2 has none).
- 2D array + 3D textures: `resource_create` refuses them; twiddled/stride descriptors are 2D-only
- UBOs: uniforms are push-constants/shared-reg only; needs a real constant-buffer binding path
- ~~Instancing (core; also EE: `GL_NV_draw_instanced`, `GL_NV_instanced_arrays`)~~ **DONE (patch
  0335)**: instance count in the index list (count-minus-one encoding), PDS instance-rate divisors,
  `GL_EXT_draw_instanced` + `GL_EXT_instanced_arrays`; non-indexed and indexed instanced draws match
  zink for 2/3/4 instances. Open: gl_InstanceID (needs ES3 GLSL), max 16 instances for large VS output
- Compressed formats (ES3 requires ETC2/EAC; vendor also ships ASTC): absent or CPU-fallback only
- ~~sRGB formats and sRGB render targets (`GL_EXT_sRGB`, `_sRGB_write_control`, `_sRGB_decode`)~~
  **DONE for ES2 (patch 0336)**: sRGB sample decode (TEXSTATE gamma + alpha_msb) and sRGB targets
  (PBE gamma, 16F source, multi-dword colour outputs) match zink; `GL_EXT_sRGB` advertised. ES3
  `SRGB8_ALPHA8` renderability follows the same paths.
- Integer vertex/fragment formats (ES3 core integer API; `_gpu_shader5` needs more)
- Occlusion queries (`GL_EXT_occlusion_query_boolean`)
- MSAA 4x (`GL_EXT_multisampled_render_to_texture*`, `_sample_variables`, `_sample_shading`)
- VAOs, primitive restart, sampler objects, PBO, RBO, texture LOD, transform feedback (st gives some free)
- GLSL ES 3.00 language level (`essl_feature_level` cap is the version gate; raising it without the above
  makes st call paths the driver does not have)

**ES2-level extensions that are cheap and honest** (raise ES2 parity immediately)
- `GL_EXT_texture_rg`: R8/RG8 sampler decode is currently unverified/black - implement TPU decode
  (WIP branch `wip/linear-mips` has the pck-info work), or convert; either way stop advertising formats
  that read back wrong
- `GL_OES_standard_derivatives` **DONE (patch 0337)**: pco lowers ddx/ddy; cap set, matches zink.
  `GL_EXT_shader_texture_lod` is a Mesa-core gap (no ES2 entry in the extension table);
  `GL_OES_fragment_precision_high` not in Mesa's list.
- `GL_OES_texture_float` **DONE (patch 0337)**: RGBA32F sampler decode (TPU F32F32F32F32) matches
  zink. `GL_OES_texture_half_float_linear` **DONE (patch 0338)** from `has_pbe_filterable_f16`;
  float_linear correctly still absent (no 32-bit float filtering in the hardware).
- `GL_IMG_texture_npot`: alias advertising (we already do NPOT via OES_texture_npot)
- **Cube maps (ES2 core) are missing entirely** - `resource_create` refuses PIPE_TEXTURE_CUBE and
  `max_texture_cube_levels = 0`; a probe reproduces `Bad t->Target in
  _mesa_test_texobj_completeness`/GL_INVALID_VALUE while zink renders the six faces correctly. Needs
  cube resource layout (six twiddled faces, mips per face), `TEXSTATE_TEXTYPE_CUBE` + `word1.depth=5`
  and `PCO_IMAGE_META_LAYER_SIZE`, cube uploads. This is the next big ES2-core item (and
  `GL_OES_depth_texture_cube_map` builds on it). `GL_OES_texture_stencil8` is an ES3.1-era entry,
  not an ES2 gap.
- `GL_EXT_texture_type_2_10_10_10_REV` **DONE (patch 0337)**: 1010102 sampler decode (TPU
  A2R10B10G10) matches zink. `GL_EXT_texture_format_BGRA8888` already advertised.

**ES3.1/3.2-era extensions (vendor advertises; larger or software-emulated)**
- Geometry/tessellation shaders (`GL_EXT_geometry_shader`, `_tessellation_shader`): hardware has no
  dedicated stages; vendor emulates. Lowest priority for matching - software emulation is its own project.
- Texture buffers, cube-map arrays, sparse textures, YUV/external, multiview, texture storage
  compression (`GL_EXT_texture_storage_compression`), EGL image storage/array, memory objects
- `GL_EXT_shader_pixel_local_storage`/`_framebuffer_fetch`: hardware-assisted on PVR; medium effort
- `GL_KHR_blend_equation_advanced` (coherent fb fetch), `GL_EXT_draw_buffers_indexed` (follows MRT),
  `GL_EXT_robustness`/`GL_KHR_robustness`, `GL_EXT_clip_control`, `GL_EXT_conservative_depth`,
  `GL_EXT_buffer_storage`, `GL_EXT_clear_texture`, `GL_EXT_copy_image`, `GL_EXT_polygon_offset_clamp`
- `GL_IMG_program_binary`/`GL_IMG_shader_binary`: vendor-specific blob formats; implement own cache or skip

## Phases and order (each gated by a runnable test before the next starts)
1. **ES2 cheap wins + honesty** (1-2 sessions): texture_rg decode fix, derivatives/textureLOD/precision
   caps, float/half-float caps, 1010102/BGRA/IMG_npot/depth-cube/stencil8 format work. Every extension
   added must have a gltri mode proving it renders identically to zink.
2. ~~**MRT** (foundational, also ES3)~~ **DONE**: 2/4 targets, PBE state/EOT programs per attachment,
   fragment output map via pco, MRT GPU clear, mixed formats (RGBA8+BGRA8) and `mrt_test` matching
   zink; leftover MRT work is `GL_EXT_draw_buffers_indexed` and >32-bit attachment formats.
3. ~~**Instancing**~~ **DONE (patch 0335)**: VDM index/instance state + PDS per-instance attribute
   fetch (divisor); test `open-perf/inst_test.c` matches zink (2/3/4 instances, indexed too).
4. **ES3 texture/model core**: 2D-array + 3D textures, UBOs, integer formats, sampler objects,
   occlusion queries, MSAA 4x, primitive restart, then raise `essl_feature_level` to 300 and run
   GL-CTS/glmark2-es3 as the gate. sRGB formats/RT done in patch 0336 (ES2 EXT_sRGB parity).
5. **ES3.1/3.2 extras and vendor-only extensions**: as ranked above; skip geometry/tessellation emulation
   unless an app demands it.
6. **Performance**: pco codegen parity (round-66), terrain mip-submit batching (round-64), per-scene
   tuning; re-run the same-binary three-way after each phase.

## Standing rules
Board between tests: vendor + desktop + original firmware (`tools/verify/switch-safe.sh`). Never start GL
harnesses while vendor is bound (caused a watchdog reset). Every feature needs its own gltri (or new test)
mode comparing against zink checksums, plus the existing full gltri gate and 0 MMU faults before pushing.
Patch series continues at the next number in `mesa/open-driver-series/` (currently 0338).

### MRT design notes (recon 2026-10-29; implemented as patch 0332 - see round-73 notes for the
`nir_lower_blend` colormask trap that ate attachment 1's store)
The plumbing is closer than it looks; the reference reads as follows.

- Fragment static stream: `pvr_frag_static_stream()` is already sized for `PVR_MAX_COLOR_ATTACHMENTS`,
  takes `nr_attachments` and `pbe_words[]` (3 dwords per attachment: PBESTATE_REG_WORD0/1/2). The
  current flush fills one attachment (`nr_attachments = 1`); S2 of the work is just looping
  `pvr_csb_pbe_words()` over `ctx->fb.cbufs[]` with each attachment's resource/level/format/layout.
- End-of-tile: one EOT **program per render**, not per attachment. The reference's
  `pvr_usc_eot()` takes `emit_count` and a `state_words[]` array (2 dwords per attachment), emits one
  pixel event per attachment with `nir_wop_pco()` between, and `freep` on the last. Our
  `pvr_shader_eot_create()` currently takes a single pair; extend its signature to
  `(ctx, eot, nr_attachments, state_words)` and mirror the loop. EOT program caching then keys on
  all addresses + formats instead of one address.
- Fragment variants: `pvr_shader_create_with()` sets `data.fs.output_formats[FRAG_RESULT_DATA0]` and
  the blend options for rt[0] only. MRT needs `output_formats[DATA0..DATA{n-1}]`, `blend.rt[i]` for
  each attachment (same blend state in ES2, but formats differ), and the variant cache key must
  include the attachment format list.
- Background/clear: the clear background program stores one packed colour into the tile buffer; the
  EOT program's per-attachment state words then emit it to each attachment, so a same-colour
  `glClear` works once (2) and (3) are in. Per-attachment `glClearBuffer` colours and MRT *load*
  backgrounds (fetch each target's previous content per attachment) are v1 limitations to document -
  load on MRT should fall back to a clear or the app's own writes.
- Caps: `caps->max_render_targets = 4` (u_init_pipe_screen_caps defaults to 1; ES2
  `EXT_draw_buffers`/`NV_draw_buffers` are already advertised by st for ES2, so the driver must not
  lie: today those extensions are advertised while a second attachment would be silently ignored).
- Test: new `gltri` mode with 2 RGBA8 attachments, `glDrawBuffersEXT`, fragment writes
  `gl_FragData[0]=red`, `[1]=blue`, clear both, `glReadPixels` each in turn; compare against zink
  (zink reports GLES 3.1 so it has MRT). Add `rt8test`-style pixel checks to the gate.
