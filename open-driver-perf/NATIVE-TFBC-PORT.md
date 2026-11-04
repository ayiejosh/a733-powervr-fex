# Porting twiddled + TFBC colour targets into pvr_native (design, 2026-10-28)

Goal: close the measured native-vs-vendor gap (native 635/630 vs vendor 887/880 on bench28; native Wayland
534). The 3D pass is the cost (build scene: TA 835, 3D 2797 ticks), and the reference driver (Vulkan) writes
twiddled, TFBC-compressed colour targets while native writes linear uncompressed ones.

Reference implementation (already in this tree, written by this project):
- `ff0dfe79` pvr: lossless TFBC frame buffer compression for colour targets (layout, PBE, TPU, transfers)
- `dd17f331` TFBC for sampled/input-attachment/transfer-destination images
- `46c46b3d` only compress images >= 8x8
- `5fd7b28b` / `bc1be45b` partial render area handling (glmark2 renders full targets, so not needed first)

## Facts extracted from the reference

**Twiddled layout** (`pvr_image.c`): physical extent = `next_pow2(width)` x `next_pow2(height)`; for 2D
(extent_alignment=1): pitch = cpp * physical_width, size = pitch * physical_height. (For 800x600 RGBA8:
physical 1024x1024, pitch 4096, size 4 MiB before headers.)

**TFBC header**: 8 header bytes per 256-byte block of 32 bpp data, placed BELOW the data address growing down.
`header_size = ALIGN_POT(data_size / 32, 4096)`; total_size = header_size + data_size; the GPU data address
(`dev_addr`) points past the headers (base + header_size). Headers are written by the PBE itself; a fresh
allocation needs no separate init as long as the first use is a PBE clear/render (glmark2 clears each frame).

**PBE** (`pvr_arch_job_common.c`): `memlayout = ROGUE_PBESTATE_MEMLAYOUT_TWIDDLE_2D`, `linestride` as for
linear (pixels in units of 2), and when fbc: `compression = ROGUE_PBESTATE_COMPRESSION_ENABLED`,
`compress_size = ROGUE_PBESTATE_COMPRESS_SIZE_BLOCK_8X8`, `comp_indirect_table = false`.
Native equivalent: `pvr_csb_pbe_words()` in `pvr_csb.c` (currently hard-codes `MEMLAYOUT_LINEAR`), called from
`pvr_draw_flush()` with `rt->stride` and `rt->dev_addr`.

**TPU texture state** (`pvr_arch_tex_state.c`): twiddled 2D uses `textype = ROGUE_TEXSTATE_TEXTYPE_2D`
(there is no STRIDE_TEXTYPE for twiddled) and `TEXSTATE_IMAGE_WORD1` (not STRIDE_IMAGE_WORD1); fields
`num_mip_levels`, `mipmaps_present`, `baselevel`, `depth`, `texaddr`, and for fbc
`tpu_image_state_v2_compression_mode = ROGUE_TEXSTATE_COMPRESSION_MODE_FB_DIRECT_8X8`.
Native equivalent: `pvr_csb_emit_tex_desc()` (currently STRIDE word0/word1 only), used for sampled textures
and for the background load program (`draw_load_program` samples the target itself).

**Feature gates** (`pvr_image.c`): `PVR_HAS_FEATURE(dev_info, tpu_image_state_v2)` and
`PVR_GET_FEATURE_VALUE(dev_info, fbcdc_algorithm, 0) == 50`; formats RGBA8/BGRA8 only; single-sample,
single-level, single-layer; PBE-written only.

## Port steps (native driver)

1. `pvr_resource.c`: add `layout` (linear/twiddled), `fbc`, `fbc_header_size`, `dev_addr` to `struct
   pvr_resource`; use twiddled pow2 layout for colour-render-target formats (RGBA8/BGRA8/X8) when the
   GPU has the features; allocate headers; on map/read address use dev_addr.
2. `pvr_csb_pbe_words()`: pass the resource layout; emit TWIDDLE_2D + compression bits when fbc.
3. `pvr_csb_emit_tex_desc()`: add a twiddled variant (textype 2D, TEXSTATE_IMAGE_WORD1 + fbc compression
   mode); use it in `draw_tex_binding` and `draw_load_program` when the resource is twiddled.
4. `pvr_draw.c`: PBE/EOT/load addresses from `res->dev_addr` (header offset).
5. Transfers (`pvr_transfer_map`): readback of a twiddled/TFBC target must go through a staging resource
   (GPU blit to a linear staging, then map) or be refused. glmark2 off-screen does not read the target back;
   the gltri tests do, so keep them working with a staging path or skip the fast path for them.
6. Verify in order: single-scene `build` fwtime (expect 3D to drop), zink-vs-native pixel checks via gltri,
   full Wayland score A/B with `PVR_NO_FBC=1` escape hatch, then desktop/shadow.

## Notes

- glmark2 always renders the full target and clears every frame, so the partial-render-area machinery in
  `5fd7b28b`/`bc1be45b` can wait.
- Power-of-two twiddling doubles the allocation footprint at 800x600 (4 MiB vs 1.9 MiB) - the reference
  driver does the same; compression is what pays for it.

## Result (2026-10-28, patch 0316 in PR #12)

Implemented steps 1-4 (layout, PBE, TPU descriptor, draw addresses); step 5 (CPU staging) is still
refused (`transfer_map` on a twiddled resource errors) because glmark2 does not read its targets back.
The CPU-readback test tool (`tools/verify/rttest.c`) verifies the GPU path instead: render a pattern into
the twiddled+TFBC target, sample it into a linear target (test-only `PVR_FORCE_LINEAR_RT`), read that back.

- **Pixel-exact**: 262144/262144 for twiddled, twiddled+TFBC and the linear control.
- One real bug found on the way: the PBE also needs the log2 surface extents in word1 `size_x/size_y`
  for twiddled layouts (from `pvr_arch_pbe_pack_state`); without them it wrote only ~514 pixels.
- **Performance** (same Wayland harness, off-screen): full suite linear **471** vs twiddled+TFBC
  **542 / 541** (+15%). Per scene: buffer updates +46-58%, textures +23-28%, pulsar +24%, effect2d
  +12-22%, build (A/B) 824 -> 1121 FPS; only `refract` is 3% slower.
- Debug toggles kept for A/B: `PVR_NO_TWIDDLE`, `PVR_NO_FBC`; test-only:
  `PVR_FORCE_LINEAR_RT`, `PVR_TWIDDLED_RAW_READBACK`.

## Depth verified too (2026-10-28, patches 0317-0318)

`tools/verify/depthtest.c`: a red quad at depth 0.75 then a green quad at depth 0.25 through the depth
test (green must win), then the target's colour and its depth texture are each sampled into a linear
target and read back. Native twiddled, native `PVR_NO_TWIDDLE` and zink all give `colour=00 ff 00 ff`
and `depth=40 40 40 ff` (0.25 in 8 bits): the twiddled ZLS store and the twiddled depth-texture decode
match the linear path and the reference driver, so patches 0317/0318 are value-verified.
