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
