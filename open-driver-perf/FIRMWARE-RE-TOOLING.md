# Firmware reverse-engineering tooling & unlocks (2026-10-25)

The firmware is NOT encrypted: it is a plain ELF32 MIPS32r2 microMIPS image whose *code* decodes
correctly with Capstone 5. This supersedes the earlier "IMG-private encoding" conclusion (that was a GNU
binutils microMIPS decoder failure, not encryption). Everything below is reproducible on the board.

## 1. Disassembling the firmware

- File the open driver loads: `/lib/firmware/powervr/rogue_36.56.104.183_v1.fw` (131072 B, ELF32 little
  endian, `e_flags=0x72001001` = microMIPS, arch 32r2).
- Data sections contain plaintext strings (firmware source paths `firmware/rogue/rgxfw_*.c`, assertion
  expressions, KCCB/CCB error strings) at file offsets ~0x1a7c8..0x1adxx.
- **GNU `mips-linux-gnu-objdump -M micromips` fails on this image** (prints `.word` for most code) -
  do not use it. Use Capstone (pip: `capstone==5.0.7`):

```python
from capstone import *
fw = open('/lib/firmware/powervr/rogue_36.56.104.183_v1.fw','rb').read()
md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN | CS_MODE_MICRO)
md.skipdata = True
ins = list(md.disasm(fw[0x1520:0x1520+0x19ea0], 0xc0000000))   # .text segment
```

- Segment map (file offset, vaddr, size): `.exctext` (0x00d4, 0x9fc02000, 0xd0c), `.bootandnmitext`
  (0x0de0, 0xbfc00000, 0x73c), `.text` (0x1520, 0xc0000000, 0x19ea0), `.text.startup` (0x1b3c0,
  0xc0019ea0, 0xec).
- Sanity check of the decode: `.text` contains real MMIO driver code - e.g. writes to `0xcf800000`-based
  registers (`MTS_SCHEDULE` 0xb00, VDM context store/resume regs, `PERF_TA_CYCLE` 0x6020), a 2000-poll
  busy-wait helper at 0xc0008d7c/0xc0008e20 (`lw (addr); and mask; compare expected`), and the
  "Kick TA" firmware-log site at 0xc0011fd0 (`lui $a0,0x700b; addiu $a0,$a0,0x1077` = SFID
  0x700b1077; `jals` to the log writer at 0xc0002220).

## 2. Vendor DDK source (public) for structures and register names

Cloned TI's published PowerVR Rogue DDK kernel driver (**DDK 24.2.6643903**, close to this board's vendor
DDK 24.2@6603887): `/home/radxa/ddk242`
(`git clone --depth 1 --single-branch --branch linuxws/scarthgap/k6.12/24.2.6643903
https://github.com/TexasInstruments/ti-img-rogue-driver.git`).

Useful parts:
- `hwdefs/rogue/km/rgx_cr_defs_km.h` - full CR register names/fields (kernel-side subset; some
  firmware-only CRs are not there).
- `include/rogue/rgx_fwif_shared.h`, `include/rogue/rgx_fwif_km.h` - firmware-shared structs: contexts,
  HWRT data, DM states, `RGXFWIF_SYSDATA`, HWR state/recovery flags, poll/fault records, init-config
  flags (`RGXFWIF_INICFG_*`).
- `include/rgxfw_log_helper.h` - firmware log macro/SFID encoding (SFID = 0x70000000 | id | group<<12 |
  nargs<<16).
- `services/server/devices/rogue/rgxdebug.c`, `rgxdebug_common.c` - how the vendor driver dumps firmware
  state: HWR flags ("DM stalling;" = RGXFWIF_HWR_DM_STALLING bit 5), poll/fault records, FW trace.
- `services/server/devices/rogue/rgxfwutils.c` - firmware init config, kick commands
  (`RGXFWIF_KCCB_CMD_KICK`), and the `RGXFWIF_KCCB_CMD_STATEFLAGS_CTRL` path used to set firmware HWR
  assertion flags at runtime (e.g. `RGXFWIF_INICFG_ASSERT_ON_HWR_TRIGGER`).

## 3. Firmware sysdata (RGXFWIF_SYSDATA) - kernel maps it, dump it

`pvr_dev->fw_dev.fwif_sysdata` is the CPU mapping of the firmware system data; it contains firmware
fault records and **per-thread failed-poll address/mask/count**:

- `config_flags`, `config_flags_ext`, `pow_state`,
- `fault_info[8]`: cr/os timers, data, and `fault_buf` = firmware source **path/info/line** of a logged
  fault/assert,
- `cr_poll_addr[2] / cr_poll_mask[2] / cr_poll_count[2]`: last failed hardware poll per firmware thread,
- `hwr_state_flags`, `hwr_recovery_flags[PVR_FWIF_DM_MAX]`, `fw_sys_data_flags`, `mc_config`.

Temporary debugfs added to the local module: `/sys/kernel/debug/dri/1/pvr_sysdata/pvr_sysdata`
(captures: `captures/hwr/sysdata-*.txt`). Observation after the failing geometry submission: no failed
polls (all zero), no logged firmware faults, `pow_state` back to idle - i.e. the TA DM thread is blocked
waiting for a hardware interrupt, not spinning in a firmware poll loop (the HWR info buffer remains the
authoritative classification: DM2 UNKNOWNFAILURE + DM_STALLING).

## 4. Firmware asserts: capture the firmware source location of any assertion

- Firmware assert-on-HWR-trigger can be enabled with the vendor bit
  `RGXFWIF_INICFG_ASSERT_ON_HWR_TRIGGER` (bit 20) in `fwif_sysdata->config_flags` before firmware boot.
  The local module has a `dbg_assert_hwr=1` insmod parameter for this.
- The firmware writes the assert's **source path, expression string and line** into its per-thread assert
  buffers (`tracebuf[*].assert_buf`); these are now dumpable via
  `/sys/kernel/debug/dri/1/pvr_assert/pvr_assert` (temporary debugfs added to the local module).
- Result on the failing geometry submission with `dbg_assert_hwr=1`:
  `line=3086 path='firmware/rogue/rgxfw_hwr.c' info='(gsRGXFWCtl.ui32StateFlags &
  RGXFW_STATE_ASSERT_ON_HWR_TRIGGER) == 0U'` - confirming the firmware itself triggered HWR for the stall
  and giving the exact firmware source location of the HWR trigger path (`rgxfw_hwr.c:3086`); the HWR
  trace samples signature registers `USC_UVS0_CHECKSUM` (0x5000), `PPP_SIGNATURE` (0x5020),
  `TE_SIGNATURE` (0x5028), `VCE_CHECKSUM` (0x5030), `PDS_DOUTM_STM_SIGNATURE` (0x5060) and two
  firmware-only registers (0x40e0, 0x4600), all unchanged between samples ("OldValue == CurrValue").
  Captures: `captures/hwr/assert-after.txt`, `captures/hwr/trace-assert.txt`.

## 5. Firmware MMIO mapping and kick-register capture

- The kernel defines `ROGUE_MIPSFW_REGISTERS_VIRTUAL_BASE = 0xcf800000` (`pvr_rogue_mips.h`); it is the
  firmware's virtual mapping of the GPU register block (device-tree `reg` = 0x01800000 + 0x8ffff). The
  firmware computes MMIO addresses as `(0xcf800000 | (thread << 19)) + offset`, i.e. **CR offset
  `thread * 0x80000 + offset`**, with one 512 KB register bank per firmware thread (MAX_THREAD_NUM = 2).
- New safe debugfs entries in the local module (all targeted reads, power ref held, no broad MMIO dumps):
  - `/sys/kernel/debug/dri/1/pvr_kickregs/pvr_kickregs`: snapshot of selected global registers
    (EVENT_STATUS/ENABLE, VDM context status/task regs, MTS) plus both firmware-thread banks (kick
    address/size/tail/callstack, free-list base, mailbox/state fields).
  - `/sys/kernel/debug/dri/1/pvr_kicksamples/pvr_kicksamples`: an **in-kernel µs-resolution capture taken
    immediately after every KCCB kick** (`pvr_cccb_send_kccb_kick`/combined): 8 samples x 50 µs x 37
    registers (global + thread0 bank), ring of 4 captures. This is what made the comparison below possible.
- Comparison, minimal Vulkan geometry-only TA kick (working) vs native TA kick (failing), per sample:
  - Both program the **same VDM/TA kick registers**: TPC (0x248/0x24c), control-stream base (0x408/0x40c),
    stream tail (0x410/0x414), callstack (0x418/0x41c), free-list base (0x210/0x214), plus
    0x420=1, 0x430=1, 0xc88=0x202, 0xc98=0x01ff01ff, 0xce0=1 (addresses differ, values match).
  - The firmware-private mirror register **0x0d20 = 1 in the native samples and stays 1** (the firmware
    stores a 0/1 kick-pending flag in its DM/kick struct field +0xd8 and mirrors it to 0xd20), while the
    working Vulkan samples show 0 - i.e. the native kick is issued and never acknowledged/cleared.
  - The likely kick doorbell `(thread<<19)+0x400` is write-only (reads 0 on both clients).
  Captures: `captures/kickregs/`.

## 6. Kick handshake fingerprint (fine-grained capture, 2 us resolution)

New debugfs `/sys/kernel/debug/dri/1/pvr_kickfine/pvr_kickfine` captures a 256-sample x 2 us register
timeline (ring of 2 captures) immediately after every KCCB kick, covering EVENT_STATUS (0x130), EVENT_ENABLE
(0x128), MTS_SCHEDULE (0xb00), MTS_INTCTX (0xb98), MTS_BGCTX (0xba0), MTS_GPU_INT_STATUS (0xbb0),
IRQ_OS0_EVENT_STATUS (0xbd8), the per-thread kick registers (0x400/0x404/0x408/0x410/0x418/0x420/0x430/
0xc88) and the firmware kick-pending mirror (0xd20).

Working geometry kick (minimal Vulkan geometry-only):
`d20=1` -> kick registers programmed -> `MTS_BGCTX (0xba0)=1` -> **`d20=0`** -> **`EVENT_STATUS=0x20`** ->
event cleared. Capture: `captures/kickregs/kickfine-vk.txt`.

Failing native geometry kick: the same kick registers are programmed and **`d20` stays 1 forever**, no
MTS_BGCTX scheduling value and **no EVENT_STATUS 0x20 ever appears**. Capture:
`captures/kickregs/kickfine-frag.txt` (the fragment/3D kick in the same run does show hardware activity,
i.e. the client/VM/kernel path is fine).

So the failure is now localized to: firmware issues the kick (pending flag set, registers programmed) and
the hardware never acknowledges it; the firmware's pending flag is only cleared by the completion path.

## 7. Full-window register diff at 200 us after the kick

New debugfs `pvr_kickwide` captures, 200 us after every kick, all non-zero CR dwords of the firmware
thread bank (0x0000-0x06ff) and of the second bank (0x80000-0x802ff); ring of 2. Diff of the failing native
geometry kick vs a working minimal Vulkan geometry kick (same client-side data, different client):

- identical kick-register programming (thread bank 0x1f0-0x46c region) - only addresses/timestamps differ;
- the working client additionally shows the hardware's post-TA-execution state at 200 us: `bank 01a8=1`,
  `0270=3` (ALIST0 SP), `02c0=9` (MLIST0 SP), `02f8=4`, `0300=2`, `0310=1`, `0318=9`, `03a0=8` (these are
  the `ALIST0 SP=3, MLIST0 SP=9` values from a successful TA kick);
- persistent control difference: **thread-bank register 0x0348 = 0x1f for the native client vs 0x0e for
  the working client** (a hardware status register the firmware only reads/saves, never writes, so it
  reflects hardware state, not a client-supplied value);
- timestamps/counters differ as expected (0x0008, 0x00e0, 0x00e8, 0x0160, 0x06e8).
Captures: `captures/kickregs/kickwide-*.txt`.

## 8. Context-switch PDS programs proven byte-identical to the reference

The firmware runs two PDS programs for every render context switch (the SR store/load "shared storing"
program and the PT store/resume stream-out programs), built by the client and named through the 192-byte
static context state. These were the last executable client-provided programs not yet compared at the byte
level. With temporary env-gated dumps in both drivers (`PVR_DUMP_CTXPROG=1`: `REF_SR*`/`REF_PT*` from the
open Vulkan driver, `OUR_SR*`/`OUR_PT*` from the native probe; raw dumps in `captures/ctxprog/`):

- `REF_PT_STORE` == `OUR_PT_STORE` and `REF_PT_RESUME` == `OUR_PT_RESUME` byte-for-byte except the embedded
  PT buffer address; code segments are identical (`d0800000 d0800001 ...`, `d3800010 ...`).
- `REF_SR` == `OUR_SR_*` byte-for-byte except the SR buffer address and the USC task program upload
  offsets (both expected: those are per-client allocations);
- data/code sizes identical (SR 16/3 dwords, PT store 8/5, PT resume 8/6).
So the context-switch machinery programs are not the cause of the missing TA acknowledgement.

## 9. VDM control stream compared semantically (final client data path)

Captured the reference VDM control stream at submit time (temporary `PVR_DUMP_CTRLSTREAM=1` hook in the open
Vulkan driver, prints the `gfx.control_stream` words and its device address) and the native one
(`PVR_DUMP_CTRL=1` in the probe; raw dumps in `captures/ctxstream/`). Both streams are **12 dwords and
structurally identical** - same packet sequence and same fixed field values:

- dword 0/1: `PPP_STATE0`/`PPP_STATE1` - TA state update, word_count 25, TA block address (addresses differ
  per client as expected);
- dword 2: `VDM_STATE0 = 0x4c000f80` - identical in both (data-present + other-present, cam_size 31);
- dwords 3..6: `VDM_STATE2/3/4/5` - PDS data/code base offsets (per-client layouts differ: ours 0x2000 /
  0x1000, reference 0x2880 / 0x27c0), `vs_output_size = 5` identical, `VDM_STATE5` ours `0x20000442` vs
  reference `0x20000441` (only the `vs_pds_data_size` field: 2 vs 1 unit - a consequence of the shader's
  constant data, not a structural difference);
- dword 7/8: index list (identical); dword 9/10: PPP terminate update (address differs); dword 11:
  `STREAM_TERMINATE` (identical).

So the control stream is semantically identical; the remaining value differences are legitimate per-client
PDS placements / shader-dependent sizes.

## 10. Extended wide capture (hardware context-state block)

`pvr_kickwide` was extended to also snapshot the hardware context-state block the firmware saves/restores
(CR 0x2000-0x21ff and 0x3800-0x383f). 200 us after the kick the working Vulkan client again shows the
post-execution hardware state (`hsave 0x2000 = 9`, ALIST0/MLIST0 SPs, dm state at 0x3a0 = 8) while the
native client shows none; no new configuration difference appears. Captures: `captures/ctxstream/kw3-*`.

## 11. Firmware patching: doorbell write proven to execute; pre-kick state identical

Firmware patching on a real microMIPS image turned out easy: a 32-bit `addiu` immediate is stored as
byte+2 = low byte, byte+3 = high byte, so a single-byte edit retargets a store.

- Patched the kick-doorbell instruction at 0xc0011cc8 (`addiu $v0,$v0,0x400`; following `sw16 $v1,($v0)`
  writes 1 to `(0xcf800000 | (thread<<19)) + off`) to write to the readable, latched kick-transient
  register **0xc88** instead. After the failing native geometry submission, `0xc88 = 0x00000001` -
  exactly the doorbell value, where the driver/firmware normally leaves 0x202 then 0. **The firmware does
  execute the kick doorbell write for the native client.** (Captures: `captures/prekick/kr-doorbellc88b.txt`.)
- Control experiment: redirecting the doorbell away from 0x400 (e.g. to 0x244) makes the *working Vulkan
  client fail identically* ("262144/262144 pixels wrong", no TA event) - the 0x400 write is genuinely the
  TA kick doorbell, and losing it reproduces our exact symptom. Captures:
  `captures/prekick/vk-doorbell244.txt`.
- New pre-kick capture (`pvr_kickwidepre`, all non-zero dwords of both firmware-thread register banks taken
  just before the KCCB kick is sent) on fresh module loads: the native client and the working Vulkan client
  present **identical pre-kick hardware state**; only free-running counters/timers differ
  (0x00e0/0x00e8/0x0160). Captures: `captures/prekick/pre-{native,vk}-fresh.txt`.
- Also: running a successful Vulkan draw first in the same session does not change the native failure.

Conclusion: firmware programming and the doorbell write are identical and execute; the pre-kick hardware
state is identical; yet the hardware acknowledges the TA kick only for the Vulkan client. This is not
driver-side.

## 12. The native vertex shader binary is accepted by the hardware (Vulkan injection)

Two temporary hooks were added to the open Vulkan driver to run the native driver's exact shader on the
known-working client:
- `PVR_VK_OUR_VS=1`: replace the Vulkan vertex NIR with the same shape the native probe builds
  (position attribute -> gl_Position) before lowering/compilation.
- `PVR_VK_INJECT_VS_BIN=<file>`: upload the given USC binary instead of the compiled one
  (`pvr_gpu_upload_usc`); a matching `PVR_PROBE_DUMP_VS=<file>` hook in the probe writes the native
  vertex USC binary (56 bytes for the probe's shader).

Results: Vulkan geometry-only with `PVR_VK_OUR_VS` reaches `Kick TA` -> `TA finished`. Vulkan with the
**exact 56-byte native vertex USC binary injected** also reaches `Kick TA` -> `TA finished` in the
firmware trace. So the native vertex program is a valid, accepted USC program - the shader binary is not
the TA-kick trigger. Captures: `captures/vs-binary/` (`our-vs.bin`, `vk-injectvs.txt`, trace files).
The hooks were reverted again after the experiment (documented here so they can be re-added).

## 13. Next steps this unlocks

1. **Annotate the firmware's kick path with DDK struct layouts**: locate the geometry/TA kick handler in
   the disassembly (starts from the `Kick TA` SFID site 0xc0011fd0 / the DM kick function around
   c0011c3c) and map its MMIO writes and firmware-context field reads to the DDK structs.
2. **Firmware patching/instrumentation is feasible**: the code is plaintext microMIPS; assemble patches
   with GNU `mips-linux-gnu-as` (`-mips32r2 -mmicromips`), then swap the firmware file, reload the
   driver, and read the firmware trace. Patch targets: the log/return path of the poll/wait helpers and
   the TA kick routine (log the polled register/value/PC or force the wait to succeed) to observe the
   stall point dynamically.
3. **Enable firmware HWR-assert state flags via `RGXFWIF_KCCB_CMD_STATEFLAGS_CTRL`** (vendor sends
   `RGXFWIF_INICFG_ASSERT_ON_HWR_TRIGGER`): the firmware then asserts on HWR and writes the source
   path/line into its assert/fault buffers (`tracebuf[*].assert_buf`, `sysdata.fault_info`), which the
   new debugfs can dump.
4. **Cross-check against the vendor stack's FW state**: the DDK's debug routines show which firmware
   structures/fields the vendor driver considers diagnostic - mirror the same fields for our client and
   compare with a working Vulkan run.
