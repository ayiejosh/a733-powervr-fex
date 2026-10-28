# STATUS (2026-10-27 08:45 local)

One-page state; details in HANDOFF.md, TA-KICK-DISASSEMBLY.md, RESULTS.md, PERF-PARITY-STATUS.md.

## Achieved
- **Native driver draw path integrated** (`pvr_native` gallium driver): real `pipe_context::draw_vbo` renders
  the bound vertex buffer into the bound target through its own VM/job path, pixel-exact on hardware and on
  the drm-shim. Probe `--- ALL PASS`.
- **Native benchmark**: 512x512 triangle **1.4 ms/frame (~700-750 fps, ~190 Mpix/s)**, 1366x768 5.3-5.8 ms,
  1080p 10.0-10.6 ms. Compute-fallback rasteriser optimised 7x (4x2 pixel blocks); async submit path exists.
- **GL benchmark (open stack)**: glmark2 default set, Wayland frontend (zink -> open Vulkan):
  800x600 score **543-594** (vendor native GL 511-533), 720p 429 vs 359, 1080p 265 vs 202, 4K 84 vs 53 -
  open stack ahead at every resolution. X11/Xwayland route for the open stack is unusable (no dumb buffers).
- **Vulkan parity**: open vs vendor Vulkan at parity overall (r2048 ~13% faster, cstp ~19% faster; small-draw
  cases r512/b512 ~10% slower - documented CPU/ioctl analysis and mechanism in
  captures/ab-2026-10-27/MATRIX-REFRESH.md).
- Tools, evidence, runbooks and verification gates are committed (`tools/README.md`).

## Blocked
- **Native 3D/TA path**: the native client's geometry kick is processed identically by the firmware (full-mask
  trace, registers, streams, HWRT/context args, FWCtx/RTData, kernel KICK command all compared) but the
  hardware never raises the DM2 completion event `0x20`; the firmware times out, tears the context down, and
  HWRs. The 3D stage cannot bypass it (needs TA output). The firmware itself is the official IMG open-ABI
  36.56 build (verified by sha256 against IMG's linux-firmware fork), and the latest community open driver
  (apercel, upstream + 30 backported drm/imagination fixes) shows the same stall, so both firmware image and
  driver version are ruled out. What remains is firmware/MTS-side observability - an IMG-only ask now reduced
  from "publish firmware" to "explain/log the DM2 refusal".

## What only the user can do
1. Send the finalised request + evidence bundle: `docs/IMG-FIRMWARE-REQUEST-36.56.104.183.md` and
   `img-evidence-bundle.tar.gz`, to gpu-driver@imgtec.com / linux-firmware / dri-devel.
2. If a genuine open-ABI 36.56 (or an updated BXM) firmware appears, follow `docs/TEST-NEW-FIRMWARE.md`
   (~10 min) - it may unblock the TA outright.

## Board health
Vendor `pvrsrvkm` + desktop, 0 kernel faults, open firmware original
(`sha256 1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2`). Watch + verify:
`tools/firmware/check-open-firmware.sh`, `tools/verify/verify.sh` (both read-only).

## 2026-10-27 (afternoon): TF port shipped as PR #12; open stack now needs a reboot

- `VK_EXT_transform_feedback` port onto the perf branch is validated and published as
  **PR #12** (`mesa-tf-port-2026-10-27`, patches 0270-0293, 293-patch series; Vulkan PASS, zink ES 3.1,
  glmark2 608 vs 543-594 before). PR is mergeable/clean, awaiting review/merge.
- The board was restored to vendor + desktop after the validation runs. The **open `powervr` stack cannot be
  brought back without a clean reboot**: after the repeated unbind/rebind/rmmod experiments its probe now
  reads BVNC `0.0.0.0` (firmware heuristics fail, module never binds), and the vendor-probe recovery cycle
  (built into `switch-safe.sh`) is not enough any more. Vendor stack is unaffected - board is healthy on
  vendor + desktop. A reboot restores the open stack for further experiments.
- Remaining work: (1) IMG-side observability ask for the DM2/TA stall (draft ready, needs the human to send);
  (2) after a reboot, optionally re-run the extra TF validation (`glmark2 --validate`, `vkcube`) for PR #12.

## 2026-10-27 20:05: DM2/TA blocker SOLVED

Native probe now reaches `Kick TA -> TA finished -> Kick 3D -> 3D finished` with no fault (was: DM2 timeout
+ HWR). Root cause was two native-driver draw-data bugs (VM_MAP silently replacing code-heap pages -> zero
vertex PDS program; PDS const-map entries at byte instead of dword offsets), plus two 3D-stage bugs found once
TA ran (PBE linestride in bytes instead of pixels; missing render-completion syncobj signal). Fixes are PR #12
patches 0295-0296, found with the differential capture/replay tools in `tools/pvrcap/`.
Still open: the render target is written but all black (draw/vertex data); and the PR merge + v0.1.2 release.

## 2026-10-28 09:35: refract fixed (patch 0315), perf work ongoing

Refract's FBO failure was the native driver refusing mipmapped textures (glmark2 asks for a full mip chain
when the colour target uses a mipmap min filter); patch 0315 accepts them level-0-only with a no-op
generate_mipmap. Verified: FBO reproducer COMPLETE, refract runs on native GL (82). Wayland harness clean full
run: native 534 (33/33 scenes). Next: performance (TFBC/twiddled colour + depth, multi-render overhead) with
the benchmark-optimization loop; PR #12 head now ebae1d2.

## 2026-10-28 11:55: twiddled + TFBC colour targets (patch 0316)

Native 3D targets are now twiddled + lossless-TFBC compressed (NATIVE-TFBC-PORT.md): full Wayland suite
471 -> 542/541 (+15%), build scene 824 -> 1121 FPS, pixel-exact GPU two-pass verification; only refract
3% slower. Patch 0316 pushed to PR #12 (head 75240bf). Board back on vendor + desktop, 0 faults.

## 2026-10-28 12:10: twiddled depth (patch 0317) - full suite 641

Twiddled ZLS depth buffers: full Wayland suite 471 linear -> 542 twiddled colour -> 641 colour+depth
(+36% over linear); shadow 160 -> 201 FPS. Patch 0317 pushed (PR #12 head 25f08e1). Board vendor + desktop,
0 faults, firmware original.

## 2026-10-28 12:18: twiddled depth textures (patch 0318) - full suite 659

Sampled depth textures twiddled: shadow 201 -> 290 FPS, desktop 80 -> 106; full Wayland suite 659
(471 linear baseline, +40%). PR #12 head fc0f7a6 (patches 0316-0318). Board vendor + desktop, 0 faults,
firmware original. Next: depth pixel verification, CPU readback staging, multi-render overhead.
## 2026-10-28 17:xx: twiddled readback + GPU depth clears (patch 0319)
CPU maps of twiddled/TFBC colour targets now go through a GPU-made linear staging copy (written back on
unmap), so glReadPixels on FBOs and uploads into render-target textures work again; every full depth
clear is a ZLS background depth (depth-only clears were dropped on twiddled depth). Test-only hooks
PVR_TWIDDLED_RAW_READBACK / PVR_FORCE_LINEAR_RT and a per-draw getenv removed; pvr_probe build fixed
(it failed since 0316). gltri gate (all modes incl. new GLTRI_SUBIMG) matches zink. Native glmark2
off-screen through weston + Xwayland runs all scenes: 669 (schedutil, no isolation; 528 before
0316-0318). Open: 61 uploads to mip levels > 0 fail (0315 keeps level 0 only - real mipmaps needed).
