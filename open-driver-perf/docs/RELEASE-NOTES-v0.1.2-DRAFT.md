# v0.1.2 draft release notes (pending PR #12 merge)

Third tagged release - the `trixie` line after PR #12.

## Open driver (Mesa) - new in this release

- **`VK_EXT_transform_feedback` on the open driver**: the official Mesa MR !44385 series (Ashish Chauhan,
  IMG; merged into Mesa main 2026-10-13) carried as patches `0270…0294`, together with its two upstream
  u_trace prerequisites and three integration fixes for this branch:
  - `pvr_pipeline_destroy_shader_data()` gained a `vs` argument (our alt-fragment and versioned-shader retry
    call sites updated);
  - the MR's dummy render-target dataset must be initialised after `pvr_rt_dataset_cache_init()` and finished
    before `pvr_rt_dataset_cache_finish()` (patch 0294 - otherwise device creation crashes in
    `pvr_rt_dataset_cache_take()`).
- Series now **294 patches**; `mesa-main-open-driver-all.patch` = whole delta vs `9a2b374` (173 files,
  +25495 / -928); `git am` of all 294 reproduces the ported branch tree byte-identically.
- **Result**: zink reaches **OpenGL ES 3.1 / OpenGL 3.2-class features** on the open stack (previous branch:
  GL 2.1 / ES 2.0); Vulkan `vkrender` PASS as user and as root; glmark2 800x600 **578-608** vs 543-594 before
  the port (no performance regression).

## Native DM2/TA fix (added in this release cycle)

The native Gallium driver's geometry kick was stalling in firmware because of bugs in its own draw data, found
by differential capture/replay at the ioctl boundary (`open-driver-perf/tools/pvrcap/`, patches 0295-0296):
- `pvr_vm_map()` did not reserve code-heap pages, so VM_MAP silently replaced the shader-stage mappings and
  the VDM executed a zero-filled vertex PDS program;
- `pvr_shader_vertex_data()` wrote the PDS const-map entries at byte instead of dword offsets and skipped the
  LITERAL32/LITERAL64 entries, so the attribute DMA base/stride/control words were zero;
- once the TA ran, the 3D stage exposed a PBE linestride passed in bytes instead of pixels (page fault) and a
  render-completion syncobj that no job signalled.
Verified on the board: geometry-only reaches `Kick TA -> TA finished`; the full render reaches
`Kick 3D -> 3D finished` with no fault. The render target is written but currently all black - the remaining
work is the draw/vertex data, not dispatch.

## Still open

- Native render output: black render target (draw/vertex data) - next driver bug to fix.
- IMG-side MTS observability is no longer needed for the blocker; the drafted request is kept for reference.
- GL remains served through zink -> open Vulkan; the DM2/TA stall does not affect Vulkan, compute or the
  compute-fallback draw path.

## Carried over from v0.1.1

- The A733 hardware map, the open-driver series through commit 269 (native `pvr` driver work, probe tooling),
  and the DM2/TA dossier under `open-driver-perf/`.
