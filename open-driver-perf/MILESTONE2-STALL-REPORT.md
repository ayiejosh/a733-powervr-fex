# Milestone 2 blocker: the TA (geometry) kick never reaches the hardware

Status 2026-10-25. This is the escalation summary for the remaining blocker: the native GL driver
(`src/gallium/drivers/pvr_native/`) can create a render context and submit a geometry job, and the
firmware processes the command up to and including logging `Kick TA`, but the TA hardware never
responds (no `EVENT_STATUS` bit, no `TA finished`), the DM2 deadline expires and the firmware runs its
HWR/recovery path. The identical submission from the open Vulkan driver reaches `TA finished`.

## Reproduction

Board: Radxa Cubie A7A, BXM-4-64 (BVNC 36.56.104.183), firmware 36.56.104.183, open kernel driver
`powervr` (Linux 6.16 drm/imagination backport, March-2026 job-submission fixes included).

```
# switch GPU to the open driver (desktop down)
~/open-perf/open-enter.sh
# enable firmware trace
sudo sh -c 'echo 0 > /sys/kernel/debug/dri/1/pvr_params/fw_trace_mask'
sudo sh -c 'echo 0xffffffff > /sys/kernel/debug/dri/1/pvr_params/fw_trace_mask'
# failing case: native probe, geometry-only submission
cd ~/mesa/mesa-main && PVR_PROBE_GEOM_ONLY=1 ./build/src/gallium/drivers/pvr_native/pvr-probe
sudo cat /sys/kernel/debug/dri/1/pvr_fw/trace_0 | tail -n 200
# working case: open Vulkan, geometry-only, minimal session (needs the PVR_VK_GEOM_ONLY / PVR_VK_MIN_CTXS
# patches from the project history applied to the Vulkan driver)
~/open-perf/vkrender_fif 512 1
```

Failing trace signature: ...`TA RTData loaded`, `TA Buffers`, `TE Region headers base`, `Is TA: 1,
finished: 0`, `UFL/FL base`, `ALIST0`, `Kick TA: ... First kick:1, Last kick:1, CSW resume:0` -> nothing
-> `DM2 timed out` -> `DM2 has locked up` -> HWR.
Working trace signature: same pre-kick messages -> `Kick TA` -> `Irq Task (EVENT_STATUS=0x20)` -> `TA
finished` -> `Perform TPC flush` -> ... -> `TA RTData ... finished on HW context 0`.

## Verified equal between the two clients (current builds, 2026-10-25)

Everything the kernel and the userspace client hand the firmware for this job has been compared and is
identical modulo device addresses:

- **Client CCB / kick data**: full `PVRDBGKICK` byte dump, 50 dwords: identical except the HWRT fw
  address, the VDM control-stream base, the TPU border-colour table address and the UFO signal address
  (46/50 dwords equal; refs 1/1, flags 3, ppp_ctrl 0x202, te_psg 0x20000, resume-task size
  0x20010000, UPDATE command identical). Captures: `captures/real-runs/kickdump-{vk,probe}-r58.txt`.
- **TA state block** (25 words, read by hardware's PPP state update): 20/25 words identical; the probe's
  differing words 5-9 were **injected into the working Vulkan client** (`PVR_VK_TA_WORDS`) and it still
  reached `TA finished` - the block content is definitively not the trigger.
- **Control stream**: structural word-for-word identical to the reference's (same command sequence,
  same lengths, same terminate).
- **Firmware-visible objects**: HWRT dataset (common 22/22 words identical, data0/data1 identical modulo
  addresses), render context object (identical modulo addresses and two per-client sequence counters),
  free-list objects (same shapes/sizes, global-first creation order tested), static context state stream
  (192 bytes, same layout; only addresses/program offsets differ), all static firmware data areas
  (PDS VDM-sync launcher, USC VDM-sync binary, PDS pixel-event program, YUV CSC table: byte-identical).
- **PDS/USC programs**: attribute program, EOT program, SR/PT context-switch programs - generators match
  the reference, same sizes/state words; their bytes were validated on the working client's TA.
- **UAPI call sequence**: same ioctl set; the kernel-side path is shared with the working Vulkan client.
- **Address layout**: control stream/TA block at the reference's addresses, whole general heap at
  reference-like low addresses, and fixed Vulkan-like addresses all tested - no effect.
- **Session/state variants**: extra compute/transfer contexts (both creation orders), minimal session,
  booting the board directly into the open driver (probe is the first GPU client ever), held runtime-PM
  reference (device already powered), global-first free-list creation, extra signal sync op, hardware
  RDT index 0/1 - all still hang.
- **Firmware log**: the pre-kick sequence is line-for-line identical to the working client's (only idle
  background chatter differs); the working client's trace with the probe's TA block also works.
- **Vendor stack**: its firmware trace (enabled via `/sys/kernel/debug/pvr/gpu00/apphint/*`) shows the
  same TA-kick message sequence - the vendor path has no extra stage.

## HWR info buffer (new observability, 2026-10-25)

The firmware's HWR records are readable through a safe debugfs dump added to the local module
(`/sys/kernel/debug/dri/1/pvr_hwr/pvr_hwr`; the buffer is allocated by the kernel at firmware boot and
filled by the firmware on HWR; the open driver never read it). With the firmware's
`hwr_debug_dump_limit` set to `ROGUE_FWIF_HWR_DEBUG_DUMP_ALL` the records show:

- **working Vulkan run**: `hwr_counter=0` - no HWR.
- **geometry-only probe (failing)**: one record, `type=0` (ROGUE_HWRTYPE_UNKNOWNFAILURE), `dm=2` (TA),
  `event_status=0x0`, `hwr_recovery_flags=0x20` (ROGUE_FWIF_HWR_DM_STALLING), no poll/BIF/MMU detail
  (union is zero), `cr_time_of_kick` .. `cr_time_hw_reset_start` = ~125k ticks.
  Per-DM counters: `dm_locked_up[2]=1`, `dm_recovered[2]=1`, `dm_overran`/`dm_false_detect` all 0.
- **fragment-only probe (3D hardware fault)**: two records - `[0] type=3` (ROGUE_HWRTYPE_BIF0FAULT) `dm=3`
  with `bif[req=0x0010650000000000 mmu=0x000000000000d041]` (the 3D kick *did* reach hardware), and
  `[1] type=0 dm=2` like above (the subsequent geometry kick).

So the firmware classifies the TA hang as "DM2 stalling, unknown failure": the DM2 firmware thread is
stuck waiting with no poll/BIF/MMU signature recorded, confirming the hardware never accepted/started the
TA kick rather than faulting. Captures: `captures/hwr/`.

## Additional firmware state dumps (safe, read-only)

Three more kernel-mapped firmware structures are dumpable through temporary debugfs entries added to the
local module (all read kernel memory only, no MMIO):

- **HWR info buffer** (`/sys/kernel/debug/dri/1/pvr_hwr/pvr_hwr`) - see above.
- **OS data** (`pvr_osdata`): `fw_os_config_flags` (open kernel: 0; vendor: 0xF "ctx switch: TDM; GEOM;
  3D; CDM"), sync-check marks, `kccb_cmds_executed`, per-thread interrupt counts and the **SLR log**
  (timestamp, fw context, pending UFO count, CCB name) - the SLR log is **empty** for the failing run.
- **Kernel CCB** (`pvr_kccb`): read/write offsets, all 128 command slots and return values. For the
  failing run (and the working Vulkan run) `read_offset == write_offset`: the firmware has consumed the
  KICK command, so it is not stuck reading the CCB; its DM2 thread stalls *after* consuming it.

Experiment from these: setting `fw_os_config_flags = 0xF` (the vendor value) at firmware boot does not
change the failure.

## Other findings

- **Asymmetry**: a fragment-only job from the same native client *does* reach the hardware (firmware logs
  the full 3D path, then `Irq Task (EVENT_STATUS=0x200)` -> MMU fault -> HWR). So the client/VM/kernel
  path can kick and execute the 3D hardware path; only the TA/geometry kick gets no hardware response.
- The KCCB KICK command is fire-and-forget; a probe reading the firmware's KICK return slot 200 ms later
  shows `0x0` in both the working and failing cases (this firmware does not report KICK completion there).
- The firmware image (`/lib/firmware/powervr/rogue_36.56.104.183_v1.fw`) is an ELF32 MIPS32r2
  microMIPS-flagged executable whose code sections do not decode with GNU MIPS (microMIPS) or RISC-V
  binutils and whose `.rodata` has no plaintext; the kernel copies the segments verbatim with no
  decryption. It therefore appears to be in an IMG-private encoding, so the firmware's TA-kick decision
  cannot be inspected with off-the-shelf tooling.
- Cross-check (2026-10-25): the vendor stack's own firmware `rgx.fw.36.56.104.183` (loaded by
  pvrsrvkm, `/lib/firmware/rgx.fw.*`) **does** contain plaintext strings/source paths
  (`firmware/mips/rgxfw_utils_mips.c`, `firmware/rogue/rgxfw_bif.c`, `"Unknown KCCB command"`, ...) while
  its code sections are equally undecodable - i.e. data plaintext, code encoded. The vendor module
  (`/lib/modules/.../updates/dkms/pvrsrvkm.ko.xz`, not stripped) has no decryption/decompression
  functions: `OSLoadFirmware` is just `firmware_request_nowarn` + a 16-byte alloc + optional verify
  callback, so the GPU itself decodes the code sections.
- Substituting the vendor firmware file for the open driver's failed cleanly: the open kernel rejects it
  at probe time with `"Unsupported fw info version 2"` (the open kernel supports fw-info v1) and the
  device simply doesn't bind - a safe experiment, firmware file restored afterwards.
- Parsed the firmware-info blocks (at FILE_SIZE-4K):
  - open image: fw-info **version 3**, header 48 B, 6 layout entries, flags = `PVR_FW_FLAGS_OPEN_SOURCE`,
    fw 1.1 build 6976702, device_info 4096 B; layout: MIPS_CODE @0xc0000000 (alloc 0x17000), exceptions
    @0x9fc02000, boot @0xbfc00000, data @0xc0032000 etc.
  - vendor image: fw-info **version 2** (header 40 B), fw 24.2 build 6603887, same layout shape with
    larger code (0x1a000).
  - The open image's **data sections contain plaintext strings** (`firmware/mips/rgxfw_utils_mips.c`,
    `firmware/rogue/rgxfw_hwr.c`, `"Unknown KCCB command"`, ... at file offsets ~0x1a7c8-0x1acxx) while
    its **code section bytes are not valid MIPS microMIPS or RISC-V** and the kernel does no
    decryption/decompression. The GPU hardware must decode the code sections itself, so the TA-kick
    decision cannot be read with host-side tooling.
  - Boot experiment with the vendor firmware under the open kernel: a crafted image was built from the
    vendor binary (same underlying ELF/segment layout, same BVNC) by replacing the last 8 KiB with the
    open image's 4 KiB device-info block and a synthetic v3 fw-info header (flags
    `PVR_FW_FLAGS_OPEN_SOURCE`, fw 1.1, 6 layout entries copied from the vendor image). The kernel
    **accepts** it (`FW version v1.1 (build 6603887 OS)`), boots the MIPS core, but the vendor firmware
    never completes the boot handshake (`*ERROR* Firmware failed to boot`, probe -110) - so the vendor
    build cannot be run under the open kernel this way (needs vendor-side boot/environment setup or its
    shader binary). Firmware file restored; the experiment failed safely.

- Geometry job kick-flag variants (FIRST|LAST, 0, FIRST-only, LAST-only) all times out identically -
  the kernel-visible kick flags are not the trigger.

**Summary: no host-visible input or configuration remains that changes the outcome.** The blocker is
internal to the firmware/hardware boundary and needs IMG-side input.

## Ask

Everything observable from the host is equal to a working submission; the remaining difference is
internal to the firmware/hardware boundary. An IMG-side answer to any of these would unblock it:

1. What, besides the client CCB/kick data and the firmware objects, can make the firmware's TA kick path
   skip the hardware kick for a context whose command data matches a working client's byte for byte?
2. Is there a firmware build/option for 36.56.104.183 with the logging needed to see the TA-kick decision
   (or a decrypted/objdump-able firmware image)?
3. Any hardware precondition for the first TA kick (register set / DM context / VDM sync) that is not
   visible in the kernel/firmware object layout?

## Board-safety notes

- Never use raw register sampling: it crashed the board twice (`Internal error: Oops` in a data abort
  when a register block was powered down). The local module's sampler is gated behind `dbg_regs=0`.
- Submitting a malformed compute job as a DM-probe coincided with a board reboot; that experiment is
  parked (do not repeat without a supervised setup).
- A board reboot on 2026-10-25 12:42 was caused by our own temporary delayed-work probe in the kernel
  module: a work item scheduled 200 ms after every kick kept a `pvr_device` pointer and fired after
  `rmmod`/`insmod`, giving a kernel paging fault at a `dead0000...` poison address (UAF). The probe was
  removed; any future delayed-work debug code must be cancelled in the module's remove path.
- Always restore vendor + desktop after hardware work and check
  `readlink /sys/bus/platform/devices/1800000.gpu/driver`, `kwin_x11`, `dmesg` faults.
