# v0.1.2 draft release notes (pending PR #12 merge)

Third tagged release - the `trixie` line after PR #12.

## Open driver (Mesa) - new in this release

- **`VK_EXT_transform_feedback` on the open driver**: the official Mesa MR !44385 series (Ashish Chauhan,
  IMG; merged into Mesa main 2026-10-13) carried as patches `0270-0294`, with its two upstream u_trace
  prerequisites and three integration fixes for this branch (pvr_pipeline_destroy_shader_data() `vs`
  argument; dummy render-target dataset created after pvr_rt_dataset_cache_init() and finished before
  pvr_rt_dataset_cache_finish(); patch 0294). zink reaches OpenGL ES 3.1 (previous branch: GL 2.1 / ES 2.0).
- Series is now **318 patches** (`0270-0318` on top of the upstream series 0001-0269);
  `mesa-main-open-driver-all.patch` is the whole delta vs `9a2b374` and `git am` of the series reproduces
  the branch tree byte-identically.
- Vulkan `vkrender` PASS as user and as root; glmark2 800x600 578-608 with the TF port (no regression).

## Native Gallium driver (`pvr_native`) - now runs on the 3D path

- **DM2/TA stall fixed (patches 0295-0296)** - found by differential capture/replay at the ioctl boundary
  (`open-driver-perf/tools/pvrcap/`): `pvr_vm_map()` did not reserve code-heap pages (VM_MAP silently
  replaced the shader-stage mappings, so the VDM ran a zero-filled vertex PDS program) and
  `pvr_shader_vertex_data()` wrote the PDS const-map entries at byte instead of dword offsets and skipped
  the LITERAL entries. Now a native draw reaches `Kick TA -> TA finished -> Kick 3D -> 3D finished` with no
  fault.
- **Native GL bring-up (patches 0297-0315)**: full gallium draw path - varyings, uniforms, textures, depth
  testing, blending, culling, colour reload, cached PDS programs, asynchronous submission with fences;
  refract's FBO fixed (mipmapped render targets accepted with a level-0 layout); all 33 glmark2-es2 scenes
  run through the native driver.
- **Performance (patches 0316-0318)**: colour render targets are power-of-two twiddled and lossless-TFBC
  8x8 compressed; depth buffers and sampled depth textures are twiddled too (twiddled ZLS load/store,
  twiddled TPU descriptors, PBE TWIDDLE_2D + compression + log2 extents, TFBC header area below the data).
  - full glmark2-es2 Wayland suite: 471 linear baseline -> 542/541 colour twiddle -> 641 +twiddled depth
    -> 659 +twiddled depth textures (**+40%**);
  - same harness: native **653-659** vs zink **573**;
  - common surfaceless draw benchmark (`open-driver-perf/tools/verify/drawbench.c`, runs on both stacks):
    native **1427/1440** vs vendor GL **1290/1290** FPS in forward and reverse run order (**~1.11x vendor**).
- **Value verification**: `tools/verify/rttest.c` (pattern -> twiddled/TFBC target -> sampled -> linear
  readback) gives 262144/262144 pixels match; `tools/verify/depthtest.c` gives identical
  `colour=00ff00ff depth=0x40` for native twiddled, native linear and zink.

## Still open

- CPU readback of twiddled/TFBC targets (transfer_map currently refuses them; needs a GPU-blit staging
  path). glReadPixels/gltri on render targets fail until then.
- Loadable `pvr_dri.so` (DRI module) so native GL runs under X11/GLX; also the only route to a
  glmark2-class vendor comparison (the vendor 887 figure is bare X11).
- Real mipmap support (generate_mipmap is a no-op, sampling is level-0 only).
- Shadow's depth-only pass still renders through a scratch colour target (wasted store).
- Small: refract ~3% slower than before; `tools/verify/verify.sh` shim-clear check stale after clears moved
  to the GPU; test-only debug hooks (`PVR_FORCE_LINEAR_RT`, `PVR_TWIDDLED_RAW_READBACK`) to strip before
  upstreaming.
- GL can still be served through zink -> open Vulkan as before; the vendor firmware remains untouched
  (official IMG `rogue_36.56.104.183_v1.fw`, sha256 `1db1c399...`).

## Carried over from v0.1.1

- The A733 hardware map, the open-driver series through commit 269 (native `pvr` driver work, probe tooling),
  and the DM2/TA dossier under `open-driver-perf/` (now rounds 1-55, plus `NATIVE-TFBC-PORT.md` and
  `HANDOFF-native-gl-2026-10-28.md`).
