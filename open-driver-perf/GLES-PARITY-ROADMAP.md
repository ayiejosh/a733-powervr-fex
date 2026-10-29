# GLES parity roadmap: match and beat the vendor driver (2026-10-29)

Target: feature and performance parity with IMG's shipping **GLES 3.2 build 24.2@6603887**, then beat it.
Measured state the same day (single binary, EGL/X11 glmark2, perf governor, 2 runs):
native **740/741**, zink **776/768**, vendor **1199/1181**.

GL versions today (probe `open-perf/glinfo`):
- vendor: **ES 3.2**, 120 extensions
- native: **ES 2.0**, 58 extensions
- zink (open Vulkan): ES 3.1, 136 extensions

## Gap matrix (vendor extensions absent from native, 88, grouped by subsystem)

**Core ES3 requirements (must land before the version can be raised at all)**
- MRT: >1 colour attachment end-to-end (driver currently renders `cbufs[0]` only; caps say 1)
- 2D array + 3D textures: `resource_create` refuses them; twiddled/stride descriptors are 2D-only
- UBOs: uniforms are push-constants/shared-reg only; needs a real constant-buffer binding path
- Instancing (core; also EE: `GL_NV_draw_instanced`, `GL_NV_instanced_arrays`): draw path rejects
  `instance_count != 1` and `instance_divisor != 0`
- Compressed formats (ES3 requires ETC2/EAC; vendor also ships ASTC): absent or CPU-fallback only
- sRGB formats and sRGB render targets (`GL_EXT_sRGB`, `_sRGB_write_control`, `_sRGB_decode`)
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
- `GL_OES_standard_derivatives`, `GL_EXT_shader_texture_lod`, `GL_OES_fragment_precision_high`: check pco
  codegen support and advertise if the instructions exist (dfdx/dfdy/textureLod)
- `GL_OES_texture_float` / `half_float_linear`: RGBA16F path exists in the format table, needs caps+tests
- `GL_IMG_texture_npot`: alias advertising (we already do NPOT via OES_texture_npot)
- `GL_OES_depth_texture_cube_map`, `GL_OES_texture_stencil8`: small resource/format work
- `GL_EXT_texture_type_2_10_10_10_REV`, `GL_EXT_texture_format_BGRA8888`, `GL_IMG_texture_format_BGRA8888`:
  format table + decode/encode

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
2. **MRT** (foundational, also ES3): cap `max_render_targets` 4, PBE state/EOT programs per attachment,
   fragment output map via pco, fb state validation, tests: 2/4-target FBO with distinct clear/draw values.
3. **Instancing**: VDM index/instance state + PDS per-instance attribute fetch (divisor), tests:
   instanced triangle with per-instance offset, matching zink.
4. **ES3 texture/model core**: 2D-array + 3D textures, UBOs, integer formats, sRGB formats/RT, sampler
   objects, occlusion queries, MSAA 4x, primitive restart, then raise `essl_feature_level` to 300 and run
   GL-CTS/glmark2-es3 as the gate.
5. **ES3.1/3.2 extras and vendor-only extensions**: as ranked above; skip geometry/tessellation emulation
   unless an app demands it.
6. **Performance**: pco codegen parity (round-66), terrain mip-submit batching (round-64), per-scene
   tuning; re-run the same-binary three-way after each phase.

## Standing rules
Board between tests: vendor + desktop + original firmware (`tools/verify/switch-safe.sh`). Never start GL
harnesses while vendor is bound (caused a watchdog reset). Every feature needs its own gltri (or new test)
mode comparing against zink checksums, plus the existing full gltri gate and 0 MMU faults before pushing.
Patch series continues at the next number in `mesa/open-driver-series/` (currently 0329).
