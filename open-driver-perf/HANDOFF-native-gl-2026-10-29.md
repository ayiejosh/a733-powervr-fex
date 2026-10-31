# HANDOFF: open PowerVR native GL on Radxa Cubie A7A (2026-10-29, network drive)

## What this is
Status snapshot after the 2026-10-29 sessions on top of `HANDOFF-native-gl-2026-10-28.md`. Read that
first for the driver architecture; this file corrects the measurement methodology and lists what is
actually done, open, and blocked.

## The one thing to know first
**The bench30 "native" numbers were zink.** The distro `glmark2-es2` X11 binary is GLX-only and the
native driver only loads through EGL, so `MESA_LOADER_DRIVER_OVERRIDE=pvr` silently fell back to zink
(`GL_RENDERER: zink Vulkan 1.3(...)` in the saved logs). Every benchmark artifact that used
`open-perf/bench30.sh`/`glbench.sh` measured zink, including the handoff's 746/744 and all A/Bs that
used that harness. Wayland glmark2 and the EGL/surfaceless tools (gltri, drawbench - they print the
renderer) were real native and are fine.

Corrected harness: glmark2 built from source with `-Dflavors=x11-glesv2` (EGL/X11), installed durably at
`open-perf/glm2x11/` (22 MiB; `/tmp/glm2egl` must be a symlink to it because the binary's data path is
baked into the build), driver script `open-perf/bench31.sh`. One binary measures all three stacks:
native under Xwayland (open stack), zink under Xwayland, vendor IMG GLES under bare Xorg (pvrsrvkm).

## Honest numbers (corrected harness, same binary, perf governor, client cores 6-7, two runs)
| stack | scores | total FPS | renderer |
|---|---|---|---|
| native (open stack, Xwayland) | 675 (680 / 671) | 23,9k | PowerVR B-Series BXM-4-64 (36.56.104.183) |
| zink (open Vulkan, Xwayland) | 762 (763 / 761) | 25,2k | zink Vulkan 1.3 open |
| vendor (pvrsrvkm, bare Xorg) | 1199 / 1181 | 39,607 | IMG GLES 3.2 24.2@6603887 |

CORRECTED after reverting invalid patch 0329 (see round-68): native is behind zink by ~11% overall; remaining driver-specific deficits
are terrain 0.81, build use-vbo=false 0.83, ideas 0.85, conditionals/loops 0.87-0.88, desktop blur 0.89;
refract is now level with zink, native wins effect2d (1.04-1.09) and is level on shadow. The vendor gap is dominated by scenes where zink is
also far ahead (desktop blur, ideas, effect=shadow) - i.e. stack-wide, not one driver bug.
The earlier "parity on drawbench" was a harness-specific result (EGL surfaceless, vendor path being
zink-on-vendor-Vulkan): treat it as such.

## Done this session (patches pushed to PR #12, series 328)
- **0326** depth-only framebuffer renders as z-only (no scratch colour clear/store, empty-tile PBE,
  `zonlyrender`); correctness change, neutral in full-suite A/B (722/722 vs 723/726).
- **0327** B5G6R5 refused for sampler bindings (TPU decode of packed 16-bit colour reads black), so
  the state tracker converts RGB565 to RGBX8/RGBA8; that also gives those textures working mip
  chains. RGB565 sampling and a CPU-uploaded RGB565 mip chain now match zink checksums exactly.
- **0328** USC/PDS program uploads bump-allocated from per-heap chunks instead of one VM-heap BO per
  program (fixes an unbounded kernel-BO/heap leak that terrain's per-frame program rebuilds hit).
  Terrain ioctl count 24022 -> 16896 (-30%), FPS unchanged (GPU/firmware bound).
- **0329 (REVERTED by 0330)** claimed blit renders skip the background load; in this driver a blit IS a
  load-program render, so the patch turned blits into no-ops (mip gen, readback broken). Its +23%/+2.2%
  numbers were fake. Post-revert honest numbers: native 680/671 vs zink 763/761.
- Tooling/measurement: `bench31.sh`, `glm2x11/` build, `gltri`/`drawbench` X11 modes, methodology
  correction records (round-60..64 in `TA-KICK-DISASSEMBLY.md`).
- Board stays vendor + desktop + original firmware between sessions; 0 kernel faults at every
  handoff; session log in `open-perf/session.log`.

## Open - ranked by expected value
1. **Terrain/refract mip regeneration cost.** Terrain re-uploads animated textures per frame, the
   state tracker regenerates 4 mip chains (40 level addresses/frame) through our per-level render
   path: 10-20 full render submits per frame, each with a firmware TA/3D turnaround. zink does the
   levels as cheap Vulkan blits in one submit. Fix: batch multi-level mip generation into fewer
   submits (or a dedicated mip-gen path). Largest native-vs-zink gap that is driver-specific.
2. **build use-vbo=false / ideas / conditionals / loops / desktop blur** (native 0.83-0.89 vs zink):
   profile per-scene on the corrected harness; effect2d native already wins.
3. R8/RG8 sampler decode is unverified/broken (accepted formats, read back black-ish); either
   implement TPU decode or refuse them like B5G6R5. WIP branch `wip/linear-mips` in the board's Mesa
   repo has the experimental linear mip-chain + pck-info work, not pushed.
4. glmark2-class vendor comparison is now possible but bare-Xorg vs Xwayland remains a harness
   difference; if exact parity matters, run the same EGL/X11 binary on Xorg for both (needs Xorg on
   the open driver - untested).
5. Cosmetic: PR title still says "patches 0270-0296" (needs the user's OK to change).

## Blocked / do not retry casually
- Xorg on the open driver (display-engine NULL-deref/watchdog history).
- GBM/DRM-master harnesses against the open driver (hard hangs).
- Starting weston/gltri while the vendor driver is bound (caused a reboot during this session; the
  board came back vendor+desktop with the original firmware, no data loss).
- The goal "beat the vendor on glmark2" is **not** achieved; current honest state is native behind
  zink ~6% and behind IMG GLES ~1.6x on the corrected harness. Tech-debt items above are the path.

## Protocol (unchanged)
Board: `tools/verify/switch-safe.sh open|vendor`; never sustained vendor GL with a live compositor;
never DRM-master/GBM against open; firmware trace mask 0 during benchmarks; kill by explicit PID;
log every change in `open-perf/session.log`; end sessions vendor + desktop + original firmware.
