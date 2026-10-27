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

## Still open

- Native GL frontend (`pvr_native`): the native geometry kick is processed identically by the firmware but MTS
  never dispatches DM2/TA (firmware times out, HWRs). Ruled out over this session: firmware image and build,
  driver version (upstream + 30 fixes), context/stream/HWRT/FWCtx/kernel-KICK-command data, MTS host pokes,
  vendor-vs-open firmware kick-path code, and user/root privileges. The only remaining input is IMG-side
  firmware/MTS observability (request drafted in `docs/IMG-OBSERVABILITY-REQUEST-DRAFT.md`).
- GL remains served through zink -> open Vulkan; the DM2/TA stall does not affect Vulkan, compute or the
  compute-fallback draw path.

## Carried over from v0.1.1

- The A733 hardware map, the open-driver series through commit 269 (native `pvr` driver work, probe tooling),
  and the DM2/TA dossier under `open-driver-perf/`.
