# PR #11 review queue (2026-10-27)

Source: three read-only reviews of patches 0044-0269 (merged PR #11). Findings are UNVERIFIED until each gets a
board test or a check against real Mesa source (reviewers 2/3 compared with Vulkan code from memory).

## A. Native GL path (fold into the black-screen / native GL work)
1. FS compiled with no output formats (`pco_data` zeroed; no VS/FS link) - 0117/0118. Colour lost before EOT.  [DONE 0298]
2. ISP scissor + dbias table bases never written while ISPCTL.scenable is set - 0171/0150. Can reject all pixels.  [DONE (scissor) 0297/0298; dbias still open]
3. glClear on the CPU but the fragment job never loads the surface (bg/clear regs zero) - 0123 vs 0171-0176.  [DONE 0298 (clear = background object)]
4. Context lacks create/delete_vertex_elements_state and sampler hooks: real GL draw would NULL-call - 0261/0262.  [DONE 0298]
5. draw_vbo turns every draw into one fixed red triangle (compute fallback) and reports success - 0261/0262.  [DONE 0298 (TA/3D path; fallback only for unsupported state)]
6. PDS fields given absolute addresses where heap offsets are expected; code addr = data+bytes unaligned - 0136/0153/0157/0158.
7. PBE address check refuses bit 32/33 addresses; likely wrong shift assumption - 0159/0160 (+ resource alloc).  [DONE 0299]
8. HWRT data set hard-coded 512x512 (pvr_hwrt_sizes ignored) - 0184/0185.  [DONE in the draw path 0298 (probe still 512)]
9. Texture descriptor stride in bytes, not texels; no pitch alignment for odd widths - 0144/0115.  [DONE 0299 (stride in pixels; pitch alignment still open)]
10. General heap: bump allocator (pvr_vm_heap_alloc) and util_vma share one range - overlap class - 0219/0261.
11. Compute fallback: half-built state kept on failure; shared params BOs rewritten while jobs pending; no waits in map/clear/destroy - 0261/0266.
12. Heap pages/BOs never freed (ctx switch, compute) - leak per GL context - 0219/0261.
13. CDM context PDS1 sizes copied from capture; compute ctx with zero static state - 0260/0261.
14. Frag ISP words raw from a 512x512 capture; PIXEL_PHANTOM slot ignored - 0232.
15. BRN49927 ext header hand-written, no frag ext block - 0187.
16. Static EOT built from an empty program; size check after write - 0128.

## B. Vulkan / pco / perf (after native GL)
1. Regression to bisect: off-screen glmark2 690 (10-14, PR#11 head) -> 611-633 (PR#12 build, same harness).  [NOT A REGRESSION: same-session A/B 10-14 driver 620/617 vs PR12 625/616; the gap is the CPU governor (performance+isolation: 677/671 vs schedutil 616-625)]
2. Batched buffer copies load/store the whole clip box: can touch up to ~8 KiB past a buffer end, and overwrite  [CONFIRMED on hardware: 400 B copy crossing the 8 KiB row at buffer end lost 199 bytes + GPU fault; fix = bound the box by the dst buffer size (in verification)]
   host writes in gaps - 0095. Bug (high).
3. Held clear-only passes miss UPDATE_AFTER_BIND images written after recording - 0099. Bug (medium-high).  [NOT REACHABLE: no image update-after-bind, templates use tracked writes, no push descriptors]
4. SPM EOT cache: 16 append-only slots keyed on addresses, never evicted -> recompiles + leak - 0078 (0076 same design).
5. Per-RT EOT cache thrashes >32 targets; make the binary address-free (PBE words via shareds) - 0094. Perf.
6. pco single-use fold has no same-block check: hoisted mul re-run per loop iteration - 0047/0054. Perf.
7. Suspected: back-propagation can move a register write past a propagated read (loop swap) - 0098. Miscompile risk.
8. Constant-channel fold assumes RGBA byte order; BGRA8 may get wrong channel - 0067.
9. Loop live-range widening uses the current loop, not the outermost - 0097.
10. Dual-issue with rpt>1 not checked for phase-1-overwrites-phase-0-source - 0049.
11. Unused-job-type semaphore waits still CPU-waited - 0069. Perf.

## C. Cleanup (before anything goes upstream)
- Debug getenv hooks: PVR_CSB_DUMP/OFFSETS/TAIL in the Vulkan driver; PVR_VK_GEOM_ONLY (hangs apps);
  PVR_DUMP_COMPUTE_*; ~30 trace hooks in zink/wsi/pvr; PVR_PROBE_* hooks in pvr_vm.c/pvr_static.c (one corrupts state,
  one leaks a syncobj per submit).
- 380 KiB `struct pvr_transfer_submit` on the stack (0064); duplicate forward decl (0046); device caches not torn
  down on create_device error path; misplaced doxygen comments in pco helpers.
- Missing GLSL source for pvr_compute_triangle_spv.h; captured-value scaffolding in the probe.

## Status 2026-10-28 (PR #12 head 62c9f37, series 0001-0303)

- B4/B5 EOT caches: measured, no change. glmark2 off-screen: per-RT EOT cache 23 misses / 240472 lookups; SPM EOT cache 2 misses / 249100 lookups, never full.
- B6 single_use_alu_src same-block check: 0 of 115 glmark2 shaders change (13169 vs 13169 groups). Reverted, no effect.
- B7, B9, B10: not reproduced with the cstest differential compute harness vs lavapipe (bit-exact; B10 only FMA rounding, exact with `precise`).
- B8: not reproduced through zink (GL_BGRA_EXT, uniform rg FS).
- B11: CONFIRMED and FIXED (0301). Windowed glmark2: 15769/15777 submits CPU-waited, 8.7 s of 32 s. Leftover semaphore waits now carried on the GPU. Windowed subset 206/207 -> 211/211.
- C cleanup: DONE (0303). Debug/trace hooks gone from pvr, zink, WSI, EGL; ZINK_EXTRA_IMAGES removed; create_device error path tears down caches; duplicate decl and 5 doxygen comments fixed.
- New bug found during cleanup, FIXED (0302): X11 WSI geometry cache read uninitialised flags (vk_alloc2, not zalloc), double-read the first reply, and never refreshed under a slow X server.
- 380 KiB pvr_transfer_submit on the stack: upstream design (0064 only dropped the zero-init). No change.
- Verification: gate 44/44, cstest suite bit-exact, gltri via zink correct, 520-case synchronization CTS sample identical to HEAD (181 pass, 339 not supported).
