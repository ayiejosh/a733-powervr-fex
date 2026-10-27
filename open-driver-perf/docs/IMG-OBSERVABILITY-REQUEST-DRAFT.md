# Draft — IMG message: DM2/TA dispatch never happens for a non-Mesa client

Subject: BXM-4-64 (BVNC 36.56.104.183) open stack — MTS never dispatches DM2/TA for a Gallium client whose
kick data is byte-identical to the working Vulkan client

Hello,

We run the open-source PowerVR stack on an Allwinner A733 (BXM-4-64 MC1, BVNC 36.56.104.183, kernel
6.6.98-aw2511) with the open kernel driver `drm/imagination` and the official open-ABI firmware from your
linux-firmware fork (branch `powervr`, commit `8a58f818`, `rogue_36.56.104.183_v1.fw`, sha256
`1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2`).

Mesa Vulkan renders correctly, and GL through zink works. We are also developing a native Gallium GL client
(`pvr_native`, our own command-stream builder) that submits a geometry job through the same UAPI. Its kick is
processed by the firmware — the firmware trace prints the same `Kick TA` line, and it writes the same
firmware-visible state — but the hardware never raises the DM2 completion event (`EVENT_STATUS` 0x20).
After ~125 ms the firmware times out, tears the context down and hard-resets the GPU.

We have compared everything visible between the working Vulkan client and ours: userspace command streams,
context-creation arguments, HWRT/RTData/FWCtx memory contents, the kernel KCCB KICK command (type
`0x2abc0065`; `client_woff_update`, `wrap_mask`, cleanup CTLs identical), the register write sequences at
the kick, and full-mask firmware traces. They are identical modulo device addresses. The first observable
difference is after the kick: on the working client `MTS_BGCTX (0xba0)` transitions ~18 µs after the
doorbell; on ours it never does. We also built the community `apercel` tree (upstream `drm/imagination` +
30 backported fixes) — same result — so the driver version is ruled out.

Questions:

1. Is this a known erratum/limitation for non-Mesa clients, or is there an additional MTS/DM2 precondition
   we are not programming (e.g. a context/MTS state our submission leaves incomplete)?
2. Is there a firmware log mask, debug build, or HW documentation that exposes the MTS DM2 dispatch decision
   (why a kick request is accepted or dropped)? Our current log mask (all groups) shows no MTS decision lines.
3. Would a firmware build with extra MTS/DM2 logging be available to us, or could you suggest the register
   sequence/conditions MTS checks before it starts the TA DM?

Our evidence bundle (traces, register timelines, firmware-VA instrumentation, KCCB dumps) can be shared.
Thank you.

— <name>
