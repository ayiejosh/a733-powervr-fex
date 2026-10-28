# Handoff: open PowerVR stack on Radxa Cubie A7A - native GL status and review pointers (2026-10-28)

Read this together with `TA-KICK-DISASSEMBLY.md` (rounds 1-55), `NATIVE-TFBC-PORT.md`, `STATUS.md`,
`PRIOR-ART.md` and the bench/tool directories. Everything below is on the board; PR #12 carries the
patches and the dossier.

## What the branch is

`mesa-tf-port-2026-10-27` = TF port (0270-0294) + native DM2/TA fix (0295-0296) + the native GL bring-up
and performance series (0297-0318), on top of the upstream open-driver series (0001-0269). Mesa work is
patch files in `mesa/open-driver-series/`; the tree rebuilt from them matches the Mesa branch tree
exactly (`git read-tree` + `git apply --cached` check). Kernel side is the mainline `powervr` backport
(`~/kernel-src/powervr`, not part of this PR); firmware is the official IMG open build
`rogue_36.56.104.183_v1.fw`, sha256 `1db1c399…` (untouched).

## Where the native driver is now

- DM2/TA stall solved (patches 0295-0296): the native client's own draw data was wrong - a VM_MAP that
  silently replaced code-heap pages (zero-filled vertex PDS program) and PDS const-map entries written at
  byte offsets instead of dwords. With those fixed the probe reaches `Kick TA -> TA finished -> Kick 3D ->
  3D finished` with no fault.
- Native GL then got the full draw path (0297-0314): varyings, uniforms, textures, depth, blending,
  culling, cached PDS programs, async submission, refract FBO fix (0315).
- Performance (0316-0318): colour render targets are pow2-twiddled + lossless TFBC-compressed; depth and
  sampled depth textures are twiddled too (ZLS load/store twiddled, TPU twiddled descriptors, PBE
  TWIDDLE_2D + 8x8 compression + log2 extents, header area below the data).
- Value verification (no CPU twiddle needed): `tools/verify/rttest.c` renders a pattern into the twiddled
  (+TFBC) target, samples it into a linear target and compares: **262144/262144 pixels match**, for
  twiddled, twiddled+TFBC and the linear control. `tools/verify/depthtest.c` gives identical
  `colour=00ff00ff depth=0x40` for native twiddled / native linear / zink.

## Measurements (recording protocol in TA-KICK-DISASSEMBLY rounds 52-55)

- Full glmark2-es2 suite, Wayland harness: linear baseline **471** -> colour twiddle **542/541** ->
  +twiddled depth **641** -> +twiddled depth textures **659** (+40%). Native vs zink, same harness:
  **653-659 vs 573**.
- Common surfaceless draw benchmark (`tools/verify/drawbench.c`, runs on both stacks, no X/Wayland/DRM):
  native **1427 / 1440** FPS vs vendor GL **1290 / 1290** FPS, forward and reverse run order -> native
  **~1.11x the vendor stack** on this draw path.
- The vendor glmark2 **887** number is bare-X11; the native driver currently cannot load there (no DRI
  entry point), so it is not a like-for-like number.

## Open

1. CPU readback of twiddled/TFBC targets: `transfer_map` refuses twiddled resources; needs a GPU-blit
   staging path (`NATIVE-TFBC-PORT.md` step 5). Until then `glReadPixels`/gltri on render targets fail.
2. Loadable `pvr_dri.so` (DRI module build target) so native GL runs under X11/GLX; this is also the only
   route to a glmark2-class vendor comparison on the bench28 harness.
3. Real mipmap support (generate_mipmap is a no-op, sampling is level-0 only - enough for refract, not for
   general GL).
4. Shadow depth-only pass still goes through a scratch colour target (wasted store); P4.3 in the notes.
5. Small: refract ~3% slower than before; `tools/verify/verify.sh`'s shim-clear check is stale after clears
   moved to the GPU; test-only debug hooks (`PVR_FORCE_LINEAR_RT`, `PVR_TWIDDLED_RAW_READBACK`) should be
   stripped before any upstreaming.

## Blocked / external

- PR #12 merge and the `v0.1.2` tag are the owner's call (release notes drafted in `docs/`).
- A fair vendor glmark2 comparison is blocked on item 2 (or a safe vendor-Wayland harness, which has
  watchdog-reset this board twice - not to be retried casually).
- IMG-side MTS observability is no longer needed for the old stall (root cause was client data).

## Board rules learned the hard way (see AGENTS.md / TA doc rounds 39-55)

- Never run sustained vendor GL with a live compositor (two watchdog resets).
- Never run DRM-master/GBM harnesses against the open driver (two watchdog resets); safe harnesses are
  headless weston + Wayland client (EGL) and surfaceless EGL.
- Always switch-safe to the open driver before any open-driver userspace probe (a probe against a
  vendor-bound GPU NULL-derefs the vendor driver).
- End every session on vendor + desktop, original firmware; `switch-safe.sh` handles the swap and rollback.

## How to reproduce

1. Build: `cd ~/mesa/mesa-main && ninja -C build` (branch `pr12-native-ta-fix`, matches the series).
2. Open stack: `sh open-driver-perf/tools/verify/switch-safe.sh open`; back: `... vendor`.
3. Full suite: headless weston + `~open-perf/glm2w/usr/bin/glmark2-es2-wayland --off-screen`
   (`MESA_LOADER_DRIVER_OVERRIDE=pvr`); zink with `MESA_LOADER_DRIVER_OVERRIDE=zink` + the open Vulkan ICD.
4. Value tests: `tools/verify/rttest.c`, `tools/verify/depthtest.c` (surfaceless EGL; see the files' headers).
5. Native vs vendor draw path: `tools/verify/drawbench.c` with the env in the round-53/55 notes.
