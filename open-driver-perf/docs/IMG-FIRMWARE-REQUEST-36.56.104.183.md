# IMG request - UPDATED 2026-10-27: firmware image already public; only DM2/TA observability remains

**Status: the original firmware request is resolved.** IMG publishes the open-ABI image in its own
linux-firmware fork: branch `powervr` of https://gitlab.freedesktop.org/imagination/linux-firmware .
`powervr/rogue_36.56.104.183_v1.fw` there (commit `8a58f818`, sha256
`1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2`) is byte-identical to the file this
board runs. No new firmware build is needed.

**What is still worth asking IMG** (only if DM2/TA work is pursued): any documentation, debug build,
firmware log mask, or known-erratum note that explains why MTS accepts a DM2/TA kick from the Mesa Vulkan
client but never dispatches it for this project's own gallium client, given byte-identical firmware-visible
inputs (traces, registers, KCCB KICK command, FWCtx/RTData, context-creation args). The community out-of-tree
driver `apercel` (upstream drm/imagination + 30 backported fixes) was tested with the genuine firmware and
shows the same stall, so driver fixes are ruled out.

Everything below is the historical (now-superseded) text of the original request for the record.
---

# IMG request: genuine open-ABI firmware for BXM-4-64 BVNC 36.56.104.183 (Allwinner A733)

Status: finalised request text, ready to send. No IMG-side contact exists in this project; this file is the
complete material the user needs to file it.

**Attach this bundle** (created 2026-10-27): `../img-evidence-bundle.tar.gz` - request text, TA analysis and
performance/parity docs plus the kick-time evidence (full-mask firmware traces for native and Vulkan kicks,
FWCtx/RTData dumps, 64 KiB register dumps, microsecond timelines, 3D-fault trace, benchmark logs).

## Where to file
- To: gpu-driver@imgtec.com (IMG PowerVR DRM driver contact in MAINTAINERS), linux-firmware@kernel.org
- Cc: dri-devel@lists.freedesktop.org, linux-sunxi@lists.linux.dev, linux-sunxi irc/ML as appropriate
- Could also be opened as a linux-firmware bug/issue.

## Subject
Request: powervr/rogue_36.56.104.183_v1.fw for BXM-4-64 on Allwinner A733 (sun60i)

## Body

Hello,

Imagination publishes open-ABI PowerVR firmware in linux-firmware under powervr/. The BXM-4-64 GPU is
supported by drm/imagination, and `powervr/rogue_36.52.104.182_v1.fw` was added for the T-HEAD TH1520 BXM-4-64
implementation. The Allwinner A733 (sun60i) carries the same BXM-4-64 GPU at BVNC **36.56.104.183** (Radxa
Cubie A7A / A7Z). The mainline drm/imagination driver works on this board with a relabelled image, but a
genuine build is needed for full correctness, and we are chasing a hardware-side stall that may be related to
running a build made for a different BVNC.

What we have observed and verified on-device:
1. The board image ships `powervr/rogue_36.56.104.183_v1.fw` (131072 bytes). It is **byte-identical to the
   upstream `rogue_36.52.104.182_v1.fw` except for five bytes**, i.e. it is the 36.52 build relabelled:
   - `0x1f010` and `0x1f014`: BVNC C and V fields in the open FW info header (0xB6→0xB7, 0x34→0x38);
   - `0x1e021`: one byte of the device-info BRN mask (BRN mask 0xA01 → 0x201);
   - `0x23fc`: a BVNC immediate in firmware code (0xB6→0xB7);
   - `0x1a8a1`: the version string ("…52" → "…56").
   The ELF and all firmware code are otherwise the 36.52.104.182 build (open FW info version 3, fw v1.1,
   build 6976702, MIPS microMIPS, open-source flag set).
2. With that relabelled image, the mainline stack runs: Vulkan (Mesa pvr) renders correctly, compute and
   transfer work, and the driver is at performance parity with the vendor Vulkan driver on our A/B set.
3. One open-firmware client-specific anomaly remains: a render context's **geometry (DM2/TA) kick never starts
   the TA hardware** when submitted by our own gallium driver for this GPU, while the *identical* submission
   (byte-identical firmware command stream, HWRT dataset, static context state, FWCtx/RTData contents modulo
   addresses, and an identical firmware trace up to and including `Kick TA`) from the open Vulkan client
   reaches `TA finished`. Every firmware-visible input we can compare is identical; only the post-TA
   execution state differs (a consequence). This could be a property of the relabelled 36.52 build on 36.56
   silicon, or a genuine firmware behavior we cannot see without firmware-side observability.

Asks:
- Would IMG be willing to publish or build an open-ABI `powervr/rogue_36.56.104.183_v1.fw` for A733/BXM-4-64
  (the same build pipeline as 36.52.104.182, built for BVNC 36.56.104.183)?
- If that cannot be provided, any guidance on the DM2/TA kick path in the open firmware (or a debug build /
  interface that can log it) would let us close the last open-driver gap on this SoC.

## On-device evidence available (this project)
- `open-driver-perf/TA-KICK-DISASSEMBLY.md` and `captures/` (firmware traces, register timelines, FWCtx and
  RTData dumps, pre/post-kick registers, HWRT/context comparisons).
- Firmware comparison data: `/tmp/rogue_36.52.104.182_v1.fw` vs board image (5 byte diff, listed above);
  upstream file: linux-firmware `powervr/rogue_36.52.104.182_v1.fw`.
- Vendor firmware reference: `/lib/firmware/rgx.fw.36.56.104.183` (closed ABI, DDK 24.2@6603887, info_version 2)
  — cannot be used by drm/imagination.

Regards,
<user + board details>
