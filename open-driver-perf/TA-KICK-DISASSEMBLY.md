# Reading the TA/3D kick, line by line (firmware side)

Tooling: `llvm-objdump-19 -d --triple=mipsel --mcpu=mips32r2 --mattr=+micromips` resolves
microMIPS branch targets (Capstone's raw immediates do not). Disassembly excerpts are saved in
`captures/fw-disasm/`. Firmware image: `/lib/firmware/powervr/rogue_36.56.104.183_v1.fw`.

## Firmware layout facts needed to read the kick

- `gp` is initialised in `.bootandnmitext` at `bfc000e6: lui $gp,0xc003; bfc000ea: addiu $gp,$gp,0x4360`
  => **gp = 0xc0034360**, so every `-N($gp)` reference is a fixed `.shdata`/`.shdatabss` address:
  `-0x16f8 -> 0xc0032c68` (the OS/thread index used to pick a 512 KB register bank),
  `-0x1e18 -> 0xc0032550`, `-0x1e1c -> 0xc003254c`, `-0x1e20 -> 0xc0032548`, `-0x1df8 -> 0xc0032568`.
- MMIO base is `0xcf800000`; per-thread bank is `(thread << 19)`; hence
  `lw $t,-0x16f8($gp); sll $t,$t,0x13; addiu $t,$t,<off>; or $t,$t,0xcf800000` = CR `thread*0x80000+off`.
- Kick command dispatch runs on the DM2/TA firmware thread when the KCCB KICK command is consumed.
  Entry/response function: `c0011a00` onwards (`captures/fw-disasm/kick-entry-*`), setup:
  `c0011b00..c0011e80` (`kick-setup-*`), completion/ack + logging: `c0011e80..c0012100`
  (`kick-completion-*`, includes the `Kick TA ...` log site at `c0011fd0`).

## The kick setup, in order (c0011a00 -> c0011d00)

1. `c0011a02..c0011b02` - prologue, saves the caller's kick parameters (s0/s1/s2/s3/s4/s5/s6/s7),
   reads the per-DM state struct pointer into `$s4` and the command pointer into `$s2`, tests
   `$s4->0x108/$0x10c` (a 64-bit address pair, context/stream pointers) and `$s6` (the
   "combined kick / first kick" flag from the cCCB command).
2. `c0011b04..c0011b28` - mirrors a 64-bit value into `0xcf80_0000+0xcb0`/`0xcb8` (firmware-private
   kick-state registers), then `0xd20 = pending_flag` (the "kick pending" mirror the kernel debugfs
   sees as `0xd20=1`), clears `$s4->0xd0/0xd4`.
3. `c0011b2a..c0011b4c` - writes per-kick registers from the per-DM struct:
   `0x248/0x24c` (TPC), `0x258` + jal to the poll helper `0x8ee4` (waits on a hardware register,
   branch to the error/log path `0xc0011f6a` if the poll fails), `0xc88/0xc98` (DM state words from
   `$s4->0x54/$s4->0x4` - the values `0x202` / `0x01ff01ff` in the register captures), `0xc80`
   (two dwords from `$s4->0x8`), `0xc28` (`$s4->0x58`), `0xc38/0xc3c` (`$s4->0x100/0x104`),
   `0xc40` (`$s4->0xf0/0xf4`), `0xc00` (a qword), `0xc08`, `0xc10`, `0xc18`, `0xc20`, `0x4098`
   (a byte from `$s2+0x6c`), `0x17a0` (a qword from `$s2+0x40`).
4. `c0011b4c` - `beqz $s6 -> c0011efc`; `c0011b56` - `bnezc $s4->0x114 -> c0011f6a`; these are the
   two alternative kick paths. The fallthrough (`c0011b5a`) polls `0x258`, then `0x114($s4)=1`,
   writes `0x258=1`, polls again and on failure branches to `c0011f6a`.
5. `c0011b96..c0011c0c` (main path) - `0xc88 = $s4->0x54`, `0xc98 = $s4->0x4`, `0xc80 = qword
   $s4->0x8`, `0xc28 = $s4->0x58`, `0xc38/0xc3c = $s4->0x100/0x104`.
6. `c0011c2c..c0011c5c` - control-stream base from `$s2+0x38` -> `0x408/0x40c`; callstack from
   `$s3+0x8/0xc` -> `0x418/0x41c`. These are the addresses compared in the register captures
   (`0408`, `0410`, `0418`).
7. `c0011c5e..c0011c92` - four gate bits on the global word `0xc0032548...0xc0032550`:
   `bit6 (0x40) -> 0xc0011ff4`, `bit3 (0x8) -> 0xc0011fe6`, `bit1 (0x2) -> 0xc0011faa`, then
   `$s4->0xa8 |= 4` and another global bit test that jumps to `0xc0011faa`.
8. `c0011c94..c0011cce` - the doorbell itself:
   ```
   a0 = 1 << thread           ; write kick bitmask to per-DM state+0x70
   if (global & 0x100) goto c0011f6a
   if (global & 0x40000) goto c0011f12
   v0 = (thread << 19) + 0x400 | 0xcf800000
   *v0 = 1                    ; <-- doorbell write (c0011cce)
   ```
9. `c0011cd0..c0011d00` - reads `0x160/0x164` (a 64-bit free-running value, used for the
   `cr_time_of_kick` in the HWR record), sets `$s1->0x38 = 1`, sets the global
   `0xc0032e18 = 1`, then `MTS_SCHEDULE (0xb00) = 0`, restores registers and returns 0.
10. The completion/ack side (`c0011e80..`, and the branches at `c0011f6a..`) contains the
    `Kick TA: FWCtx ... First kick:.. Last kick:.. CSW resume:..` log (SFID `0x700b1077`,
    `c0011fd0`), the alternate kick helper calls to `0xef9c` (with the DM number as the argument:
    `li16 $2,2` = DM2), and the code that clears the pending flag/state after a hardware event.

## What this says about the stall

- The firmware path taken by both clients is identical up to and including the doorbell store
  (`c0011cce`) - the working Vulkan client and the native client execute the same instructions with
  the same input data (cCCB command bytes, context structures, TA block were compared previously),
  and the doorbell store address/value are the same.
- The working client then gets `MTS_BGCTX (0xba0)=1 -> EVENT_STATUS (0x130)=0x20`; the native client
  gets neither, and `0xd20` ("kick pending" mirror) stays 1 until the firmware's HWR path runs.
- Therefore the reject is **after** the doorbell and **before** MTS schedules DM2: it is the hardware
  MTS scheduling/dispatch of DM2 that does not accept the kick for this client, not the firmware
  deciding to skip it.
- The firmware has an alternate kick path (`0xef9c` helper, DM number passed as 2) reachable at
  `c0011f6a` when `$s4->0x114 != 0` or when the polls at `0x258` fail; the main path additionally
  gates on global bits 0x100/0x40000. Which path is taken is the same for both clients today, so
  the alternate path is a candidate to force (firmware patch) to see whether MTS accepts that form.

## Break plan (next actions)

1. Dump the MTS scheduling configuration registers (not in the current captures) for both clients:
   `MTS_BGCTX_THREAD0/1_DM_ASSOC (0xb30/0xb38)`, `MTS_SCHEDULE_ENABLE (0xbc8)`,
   `MTS_DM0..DM6_INTERRUPT_ENABLE (0xb58..0xb88)`, `MTS_BGCTX_COUNTED_SCHEDULE (0xba8)`, plus
   `MTS_SCHEDULE (0xb00)` - diff native vs Vulkan. Extend the local module's `pvr_kickwide` list.
2. If a difference appears (e.g. DM2 not in the thread's DM association or its interrupt disabled),
   that is the gate; fix it in the kick path (kernel-side register pokes are not needed: the
   firmware owns these, so a firmware patch or an init-config change is the lever).
3. Force the alternate kick path in the firmware (patch the branch at `c0011b56`/`c0011c94` to the
   `0xef9c` helper) and retest the native TA; if it completes, the mechanism is identified.
4. Poke `MTS_SCHEDULE = HOST|DM2 (0x102)` from the kernel after a stuck native kick, to test
   whether a direct MTS scheduling request starts the TA DM.

## First hardware-level results (2026-10-26)

The GPU register block (`0x1800000`, 0x90000 long) is readable/writable through `/dev/mem` as root; the
new tools are in `tools/gpu-regs/` (`regrw`, `mtsdump`, `watchpoke`; a read of one page is safe, writes
are used only for the MTS experiments).

- **MTS configuration is identical between the working Vulkan client and the native client**: sampled
  during a working Vulkan TA render and during the native kick: `MTS_BGCTX_THREAD0_DM_ASSOC (0xb30)=1`,
  `MTS_INTCTX_THREAD0_DM_ASSOC (0xb40)=0xffff`, `MTS_SCHEDULE_ENABLE (0xbc8)=0xff`,
  `MTS_DM0..DM3_INTERRUPT_ENABLE (0xb58/0xb60/0xb68/0xb70)=0x89200/0x0/0xa0/0x8`,
  `MTS_DM4/DM5 (0xb78/0xb80)=4/0`, `MTS_GPU_INT_STATUS (0xbb0)` only shows the working run's IRQ bits.
  So the native client is not rejected by a different MTS association/enable configuration.
- The working Vulkan sample after TA execution shows `EVENT_STATUS (0x130)=0x10`, `0x1a8=1`,
  `0x270=3`, `0x2c0=9`, `0x2f8=4`, `0x300=2`, `0x310=1`, `0x318=9` (ALIST0/MLIST0 stack pointers) -
  exactly the post-TA state the failing run never reaches.
- A first direct `MTS_SCHEDULE (0xb00)=0x102` (HOST|DM2) poke while the firmware mirror `0xd20` was 1
  did not start the TA (no `0x130` bit, no ALIST state; `0xd20` cleared ~30 ms later, which needs to be
  re-tested with a cleaner pending window - the sample may have caught a stale/earlier kick). The
  remaining values to try are `0x2` (BGCTX DM2), `0x32` (INTCTX|counted|DM2), `0x122`, and the poke
  should be issued only on a freshly observed 0->1 transition of `0xd20` with the firmware trace mask
  enabled (the earlier attempt's debugfs directory detection failed, so the trace was not captured).

## Next experiments (in order)

1. Rebuild `pvr_kickwide`-style sampling in the local module (the original debugfs source was lost with
   the old worktree) or use the `/dev/mem` sampler to log the **complete set of changed registers** around
   a native kick vs a Vulkan kick, including `MTS_SCHEDULE`, `MTS_BGCTX`, `0x1a8`, `0x270`, `0x2c0`.
2. Poke the remaining `MTS_SCHEDULE` encodings on a clean 0->1 pending transition.
3. Force the alternate firmware kick path (`0xef9c` helper, argument DM=2) by patching the branch at
   `c0011b56`/`c0011c94` and retest the native TA.
4. If MTS rejects the kick because of a hardware state not visible in these registers, the next level is
   the VDM/TPC/TA unit's own "context loaded" state (CR `0x430 VDM_CONTEXT_STORE_STATUS` and the
   `0xc00..0xc9c` firmware-private kick block) - compare all of it across a real Vulkan TA kick and a
   native one at the same point in the sequence.

## Round-2 results (2026-10-26, goal round 1): clean Vulkan reference captured; more leads ruled out

- Added `PVR_VK_GEOM_ONLY` to the Mesa Vulkan winsys (mesa `2b09bd3`) so the working client can submit a
  geometry-only TA kick. With it, the firmware trace of a **first kick of a fresh context with no preceding
  3D work** still shows `Kick TA ... -> Irq Task (EVENT_STATUS=0x20) -> TA finished` - the minimal TA kick
  works from Vulkan. Trace: `captures/real-runs/fw-trace-vk-geomonly.txt`; register timeline:
  `captures/kickregs/kt-vkgeom.txt`.
- The native geometry-only kick under the same minimal conditions still logs the identical
  `Kick TA: FWCtx ... First kick:1, Last kick:1, CSW resume:0` and then nothing (DM2 timeout). Register
  timeline: `captures/kickregs/kt-natgeom.txt`.
- **Static context state compared word for word** (LD_PRELOAD ioctl interposer extended to dump
  `DRM_IOCTL_PVR_CREATE_CONTEXT`'s `static_context_state` bytes): the 192-byte streams have the same layout
  and the same PDS task state words; only the two context-state buffer addresses and the PDS program
  offsets differ (each driver's own uploads). Capture pairs in `captures/real-runs/interposer-*`.
- The register-write difference at kick time is now precise: Vulkan's kick additionally writes
  `01a8=1, 02c0=3, 02f8=4, 0300=1, 0318=6, 03a0=0xa` before the doorbell; the native kick never writes those.
  Vulkan then gets `EVENT_STATUS=0x20` and the post-TA state; the native one does not.
- Ruled out this round: **low device addresses** (`PVR_PROBE_LOW_VA=1` puts all allocations at 0x800f... like
  Vulkan's 0x8000...; kick still times out); **direct MTS_SCHEDULE pokes** during a live native kick
  (`0x2, 0x12, 0x22, 0x102, 0x112, 0x122` - none start DM2); **prior Vulkan warm-up** (Vulkan render first,
  immediately followed by the native kick - still times out); **concurrent Vulkan load** (inconclusive:
  under load the native kick either arrives with a different FWCtx after the trace ring rotated, or the GPU
  gets stuck when Vulkan is SIGSTOPped; no TA-finished was observed for the native context).

Still-open hypothesis: the DM2 start depends on the register set/context state the firmware writes during
`Setup register set=-1 DM=2, PC address=...`, and the values Vulkan writes at 01a8..03a0 come from the
hardware context state that the working client's *earlier*/first kick initializes differently. Next probe:
instrument the firmware at the doorbell/kick site with a marker written to a readable firmware-private
register and dump the DM2 start path, or compare the `PC address` memory (context register set) by mapping
it through the kernel.

## Round-3 results (2026-10-26, goal round 2): GPU warm-up ruled out; firmware keep-out investigated

- **GPU warm-up before the kick does not help.** Added `PVR_PROBE_GEOM_WARM` (a synchronous compute
  draw through the pipe_context just before the geometry submit). The warm-up draw completes, then the
  geometry kick still logs `Kick TA` and times out with no `TA finished`. Trace after kick:
  `c006f000` style FWCtx -> 126 ms later `UFO Updates` = the HWR recovery. So the stall is not GPU
  power state / idle-timer related.
- **A firmware-address-space reader was built** (`tools/kernel/pvr_fwva-debugfs.patch`, kernel module
  debugfs `pvr_dbg/fwva` + module params `fwva`/`fwvalen`): it translates firmware MIPS VAs through
  the kernel-maintained microMIPS page table and dumps the bytes. Validated by reading firmware code at
  `0xC0000000` (byte-identical to the .text segment in the firmware file, first 32 bytes checked).
- **Firmware page-table occupancy is tiny**: right after boot and even while a live render context
  exists, only ~75 4 KB PTEs are valid (indices 0x000-0x02a = firmware code, 0x032-0x04e = data, plus
  0xfd0-0xff0 = the 192 KB config heap at the top of the 16 MB window). The firmware's runtime heap
  objects reported in the trace (`FWCtx 0xc005f000`, `RTData 0xc0063000`, `MemCtx 0xc002b000`) are **not
  mapped through that page table** (`0xc005f000` reads as `<unmapped>`), so their memory is reached by
  a different mechanism (likely GPU-VA objects inside the firmware's own heap view). Consequence: to
  diff FWCtx/RTData contents the reader must first discover that mapping (next step: find where the
  firmware's heap VA `0xc005f000` is backed - possibly a GPU VA in the reserved internal range, since
  `0xc005f000` and the `PC address 0x1_xxxxxxx` values both fall in GPU-VA ranges the UAPI heap config
  reserves for internal use).
- Nothing on the board was changed permanently: firmware kept original (`1db1c399...` verified), the
  kernel worktree source was reverted (the reader is preserved as a patch in the project), and the board
  was restored to `pvrsrvkm` + desktop with 0 faults before the round ended.

## Round-4 results (2026-10-26, goal round 3): fragment-first and post-kick register window

- **A fragment kick before the geometry kick does not help.** `PVR_PROBE_FRAG_THEN_GEOM=1`: the
  fragment job logs `Kick 3D ... Partial render:0` and the GPU locks up within ~100 us (the known
  bad-fragment-stream lockup), then the geometry kick still times out. Trace:
  `captures/real-runs/fw-trace-native-fragthengeom.txt`.
- **Post-kick hardware window diff** (`tools/gpu-regs/kicksnap.c`): snapshot of registers 0x000-0x4ff
  200 us after the kick-pending mirror goes 1, for both clients (`captures/kickregs/snap-*.txt`). The
  only differences are exactly the TA-execution state that Vulkan reaches and the native client never
  does: `1a8=1, 270=3, 2c0=9, 2f8=4, 300=2, 310=1, 318=9, 3a0=8`, plus the differing addresses. At
  200 us the native kick still has `d20=1` and `130=0`; Vulkan has `d20=0` and the post-execution
  values. No hidden kick-acknowledge register distinguishes the two: the hardware simply never starts
  DM2 for the native client.
- Nothing permanent changed on the board; firmware original.

## Board crash during this round (root cause, recorded for future sessions)

The previous boot ended in a kernel NULL pointer dereference in the **vendor** driver:
`PVR_K: psConnectionPriv is NULL in LinuxServicesConnectionFromFile()`, `Invalid connection data`, then
`Unable to handle kernel NULL pointer dereference at virtual address 0` (level-0 translation fault). This
is the documented "vendor driver unload/reload while a GPU client still holds a connection" crash: the
driver switch ran while the X/kwin connection had not fully gone away (the display-manager stop and the
switch raced). Mitigation used afterwards: `systemctl stop display-manager`, wait, verify no
kwin/weston/Xwayland/Xorg and no `/dev/dri` users, only then run `open-enter.sh`; restore with
`open-exit.sh`. No board damage; firmware sha256 unchanged.

## Round-6 results (2026-10-26, goal round 6): FWCtx/RTData objects finally compared

Captured the firmware heap objects themselves with the new reader + a kick-pending watchdog
(`tools/gpu-regs/stoponkick.c` freezes the submitting process the microsecond the firmware's kick-pending
mirror 0xd20 goes 1, so the context and its firmware objects stay alive):

- Native and Vulkan FWCtx (firmware common context) dumps at a kick: identical modulo addresses; the only
  non-address differences are firmware-maintained activity fields (waiting/run list nodes, a counter) that
  are zero in the freshly-kicked native context. Captures:
  `captures/kickregs/fwctx-{nat,vk}-post.txt`.
- Native and Vulkan RTData dumps at a kick: identical layout and identical kernel-provided fields (pm_mlist,
  tpc/vheap/region-header/freelist addresses, hwrt common addr, flags, state). The differences are the
  firmware/hardware-maintained TA-state fields `vce_cat_base`, `te_cat_base`, `alist_cat_base`,
  `pm_alist_stack_pointer`, `pm_mlist_stack_pointer` - non-zero on the working client, zero on the native
  one. These carry addresses in the reserved internal GPU-VA range with low-bit flags, i.e. exactly the TA
  execution state that the working client reaches and the native client never does; they are a consequence
  of the TA never starting, not an input to it. Captures:
  `captures/kickregs/rtdata-{nat,vk}-post.txt`.
- With this, every firmware-visible object has been compared: FWCtx, RTData, HWRT dataset args/contents,
  static context state, command streams, kick command, pre/post kick registers, and the firmware trace.
  Nothing that could gate DM2 start differs; the remaining difference is the TA-execution state itself.

Blocker state after six goal rounds: the DM2/TA stall is not resolvable with the observability available on
this board. The benchmark half of the objective is complete (compute-fallback draw path pixel-exact at
1.42 ms / 702 fps for 512x512 after the 4x2 rasteriser, with the vendor comparison recorded in
PERF-PARITY-STATUS.md). The native 3D/TA path needs vendor/IMG firmware-side inputs (firmware source or
debug interface) to go further.

## Round-7 results (2026-10-27): complete full-mask firmware trace across both kicks

Enlarged the kernel firmware trace ring from 12000 to 200000 dwords (pvr_rogue_fwif.h constant, temporary
module) and captured a **full-mask (all 16 log groups)** trace across both the failing native geometry kick
and a working open-Vulkan geometry kick. Captures:
`captures/kickregs/fw-trace-fullmask-{native,vulkan}.txt`.

The firmware-side setup before the kick is line-for-line identical (MemCtx activation, register set alloc,
freelist load, TA RTData loaded, TA buffers/VHeap/TPC, TE region headers, `Is TA: 1, finished: 0`,
`UFL-TA-Base ... FL-TA-Base ...`, `ALIST0 SP = 0, MLIST0 SP = 0`), then `Kick TA` with the same fields
(First kick:1, Last kick:1, CSW resume:0, prio:1).

The divergence is the hardware interrupt that follows:
- working Vulkan: `Irq Task complete` -> `Irq Task (EVENT_STATUS=0x00000020)` -> `PM running primary config`
  -> `TA finished` -> `Is TA: 1, finished: 1` -> `ALIST0 SP = 3, MLIST0 SP = 9` with non-zero
  `VCE 0x104dbf001 / TE 0x106fa5001 / ALIST 0x131bfa001` -> `TA RTData ... finished` -> UFO update.
- failing native: `Irq Task complete` -> a subsequent MTS wake with `Irq Task (EVENT_STATUS=0x00000000)`.
  The DM2/TA event bit **0x20 is never raised**. The firmware then runs its DM2 progress/HWR checks
  (`DM2 has taken <n> ticks and deadline is 129360000 ticks`, fast CRC/HWR signature checks,
  `USCSlotsUsedByDM=0`) and eventually reports the DM2 timeout that ends in `GPU has locked up`.

So the stall is now pinned precisely: the firmware kicks DM2 with an identical TA context, and the hardware
never raises the DM2/TA completion event (0x20) for the native client, while the same firmware raises it for
the Vulkan client. The remaining difference is not visible in any firmware-accessible state; it is inside
the hardware/DM2 start path, which needs vendor/IMG input (see docs/IMG-FIRMWARE-REQUEST-36.56.104.183.md).

## Round-8 results (2026-10-27): 64 KiB post-kick register dump comparison

Extended the kick-time register capture from the old 0x000-0x4ff window to the full 64 KiB MMIO window
(robust page-wise reader, captures in `captures/kickregs/regs64k-{native,vulkan}-postkick.txt` plus an idle
reference). Both dumps taken immediately after the client was frozen on kick-pending (d20 0->1), i.e. within
~1 ms of the kick.

- The 0x000-0x4ff system window differs only in the expected per-client addresses (callstack, context state
  pointers) and in activity counters - same conclusion as the old narrow snapshots.
- Above 0x4ff the native dump is **zero in every DM/TA/TPC/ISP register block** (0x200-0x420, 0xc00-0xd14,
  0x1200-0x1264, 0x2000, 0x3840, 0x40e0, 0x4600, 0x5000-0x5060, 0x6008-0x6530), while the working Vulkan
  dump has them set exactly where the TA/USC/ISP execution leaves them (e.g. 0x420=1, ALIST/VCE/TE page
  bases 0x05a6a001/0x3034a001/0x19a73001, USC core regs at 0x5000+). Those blocks are written when the MTS
  actually runs DM2, so this is a consequence of DM2 never running, not an input.
- Conclusion unchanged: the firmware-side setup and kick are identical for both clients; the hardware never
  enters the DM2/TA start path for the native client. Evidence is archived; the remaining requirement is
  vendor/IMG firmware-side input (request finalised in docs/IMG-FIRMWARE-REQUEST-36.56.104.183.md).

## Round-9 results (2026-10-27): userspace context/HWRT creation arguments compared - identical

Closed the last userspace-side differential between the two clients by reading both creation paths against the
UAPI (`drm_pvr_ioctl_create_context_args`, `drm_pvr_ioctl_create_hwrt_dataset_args`):

- CREATE_CONTEXT: both pass `DRM_PVR_CTX_TYPE_RENDER`, priority NORMAL (`DRM_PVR_CTX_PRIORITY_NORMAL` = 0;
  the Vulkan winsys maps its MEDIUM priority to the same value), 192-byte static context state built by the
  same scheme (VDM/GEOM state bases + store/resume PDS tasks), a general-heap VDM callstack, and the same VM
  context. `flags` is zero on both.
- CREATE_HWRT_DATASET: for the 512x512 one-layer, one-sample reference both pass tpc_size 2048 / tpc_stride
  0, no RTC, no macrotile array, the double-buffered pm_mlist/region-header addresses, local/global free
  list handles, and all ISP-merge coefficients 0 with region_header_size 256 (the values the earlier trace
  analysis read out of the reference's dataset).
- The free-list sizes/creation order and heap selection used by the probe are the reference's values too
  (earlier rounds aligned them; the kernel accepts and the firmware loads the same freelists - trace
  "Load Freelist" lines match).

With the firmware trace, HWRT/context contents, kick streams, registers and now the creation arguments all
matched, no in-board differential remains that gates the TA/DM2 start for this client. The blocker is the
missing hardware DM2 completion event (0x20) with identical firmware state; moving it needs vendor/IMG
firmware-side input (request in docs/IMG-FIRMWARE-REQUEST-36.56.104.183.md).

## Round-10 results (2026-10-27): microsecond register timeline around the kick

Captured a 3000-snapshot timeline of GPU MMIO 0x000-0x500 at ~36 us resolution starting the moment the kick
goes pending (0xd20 0->1) for both clients (analysis in
`captures/kickregs/reg-timeline-analysis.txt`; raw binaries in /tmp on the board).

- Both clients: the hardware context registers are already programmed at kick time with the expected
  per-client values (MMU/page-table bases 0x...|0x80/0x9f at 0x210/0x248/0x2d8, context state pointers at
  0x408/0x410, 0x420=1, 0x348=0x1f/0xe). No observable firmware/hardware programming step differs between
  the two in the microseconds after the kick.
- Working Vulkan: those registers stay programmed for the whole 108 ms capture window - the TA runs and the
  context stays valid.
- Failing native: at **t = 29.03 ms after kick-pending** every one of those context/MMU/state registers is
  cleared to zero in one step (0x210, 0x214, 0x224, 0x244, 0x248, 0x24c, 0x2b0, 0x2d8, 0x2dc, 0x348, 0x3d0,
  0x3d4, 0x408, 0x40c, 0x410, 0x414, 0x420). That one-step clearing is the firmware/PM taking the hardware
  context down (either an explicit deactivate, or the core being clock-gated so the CPU reads zeros); either
  way the context is gone for the native client while the working client's stays. The DM2 progress/HCR checks
  follow (0x88 idle-timer toggles at 29.1/40.0/40.9 ms) and finally the lockup is reported.
- So the hardware context is set up correctly for the native client too; the hardware simply never advances
  the TA. This closes the last register-side observation and confirms the stall is inside the DM2 start /
  TA-command path with no software-visible cause - it needs vendor/IMG firmware-side input.

## Round-11 results (2026-10-27): widened register timeline (0x500-0x1000)

Extended the kick timeline to the 0x000-0x1000 window (analysis:
`captures/kickregs/reg-timeline-wide-analysis.txt`). Newly visible:
- The firmware execution/PC debug registers (0x878/0x87c, values in firmware .text) update over time and at
  **29.078 ms** the kick-pending bit 0xd20 itself is cleared - the firmware has processed the DM2 timeout,
  decided the DM2 kick is dead, and torn the hardware context down; the periodic DM2 HCR checks then run at
  ~31/40/41 ms and the lockup is reported later.
- Early side effects of the kick exist for the native client (e.g. 0x8f8 0x2->0x6 at 228 us), but nothing
  that indicates the DM2 was ever dispatched to an execution core.
- The working Vulkan client's capture over the same window shows the expected TA-progress side effects and no
  teardown; the earlier context/state window comparison (previous round) already showed the same.
No software-visible cause; the hardware simply provably never runs the TA for the native client. This closes
the register observation set; moving further needs vendor/IMG firmware-side input.

## Round-12 results (2026-10-27): can the 3D (DM3) path be reached without the TA? No.

Added two env-gated probe diagnostics (mesa commit): submit the fully-populated real fragment stream alone
(`PVR_PROBE_FRAG_ONLY_REAL`), and set explicit UAPI fragment job flags (`PVR_PROBE_FRAG_FLAGS`, e.g. 0x40
PARTIAL_RENDER). Results on hardware:
- fragment-only real stream: firmware does kick 3D ("Kick 3D ... Partial render:0") and the GPU locks up
  ~95 us later - the 3D stage expects TA-produced tile/region data and faults without it;
- fragment job with PARTIAL_RENDER alone: the firmware completes the job silently (cCCB/UFO update, **no
  Kick 3D**) - a partial-render fragment job is only kicked when paired with a geometry job.
So the 3D stage cannot be reached around the TA: the TA stall remains the single gate for every hardware
rendering path on this client. The clean-completion-without-kick behaviour also shows the firmware's job
state machine is healthy for this client - only the TA DM2 start never happens.

## Round-13 results (2026-10-27): the 3D-path fault reason (full-mask trace)

Captured a full-mask firmware trace of the fragment-only 3D kick
(`captures/kickregs/fw-trace-fragment-only-3d-fault.txt`). It shows exactly why 3D cannot run without the TA:

- firmware logs `3D check: missing TA FWCtx 0xc005f098 @ 0, RTD 0xc0063000` and kicks 3D anyway;
- 3D raises `Irq Task (EVENT_STATUS=0x00000200)` -> `MMU fault event: 0x00000200` ->
  `BIF0 page fault detected (Core 0 MMU Status 0x000000000000d041, Req Status 0x0010650000000000)` ->
  `GPU-0 has locked up`;
- firmware then does HWR (`Analysis DM3: Lockup ... need to skip to next command`, `Begin hardware reset`,
  `Finished hardware reset`, `Recovery DM2 ... DM fully recovered`).

So the 3D stage faults on TA-generated data that does not exist, i.e. the dependency is real and cannot be
bypassed; the firmware's reset/recovery path itself works. This also confirms the client's firmware job state
machine is healthy: the only abnormal thing is that DM2 (TA) never starts and never raises its event, and the
firmware times it out.

No in-board avenue remains to make the native client's TA start; the remaining requirement is IMG-side
firmware observability (request in docs/IMG-FIRMWARE-REQUEST-36.56.104.183.md).

## Round-14 results (2026-10-27): context priority is not the cause

Added a probe hook for the UAPI context priority (PVR_PROBE_CTX_PRIORITY) and re-ran the native geometry kick
with LOW (-512) and HIGH (512) priorities (root for HIGH). Firmware trace confirms the new priorities on the
kick line (prio:0 and prio:2) but both still time out: no `TA finished`, then `GPU has locked up`. So MTS
priority is not the gating factor for DM2 start. Default behaviour unchanged; hook committed.

## Round-15 results (2026-10-27): HWRT data index and context-churn variants ruled out

Ran the native geometry kick with the double-buffered HWRT data index 1 (PVR_PROBE_HWRT_IDX=1), with extra
contexts created before the render context (PVR_PROBE_EXTRA_CTXS_FIRST=1), and both together. The firmware
trace confirms each variant reached its kick (`RTD 0xc0064000` for index 1, different FWCtx/RTData addresses
with extra contexts), and every one still times out: no `TA finished`, then `GPU has locked up`. So neither
the RTData double-buffer index nor preceding context churn changes the DM2 outcome.

## Round-16 results (2026-10-27): sync-op shape ruled out

Added a probe hook (PVR_PROBE_NO_SYNC_OPS) that submits the geometry job with no sync ops at all (no signal
dependency). The job is accepted, the firmware traces the same `Kick TA` with the same fields, and the TA
still never finishes (`GPU has locked up` after the DM2 timeout). So the client's signal/wait shape is not
the gating factor - the kick itself is processed identically with or without it.

## Round-17 results (2026-10-27): firmware identity/compcheck variant ruled out

Tested a firmware variant with the built-in BVNC identity byte reverted to 36.52's value (0x23fc 0xB7->0xB6)
while the FW info header still claims 36.56.104.183 (so the kernel loads it, but the firmware's own compiled
identity reports C=182). The driver loaded, the geometry kick reached `Kick TA`, and the TA **still never
finished** (DM2 timeout, `GPU has locked up`). So the relabelled identity/compcheck path is not what gates
DM2 either; original firmware restored (sha256 1db1c399...).

## Round-18 results (2026-10-27): forcing the firmware's alternate kick path does not unblock the TA

Executed the RE break-plan item: the firmware's main kick path is `lw $4,0x114(FWCtx); bnezc $4 -> 0xc0011f6a`,
where `0xc0011f6a` calls the wait/kick helper at `0xef9c` with DM number 2 (DM2) before returning to the
doorbell code. Patched the load to `li $4,1` (file offset 0x12f0a: `94fc1401` -> `01ee000c`) so the alternate
path is always taken, booted it, and ran the native geometry probe: firmware loads, logs the same `Kick TA`,
and still never completes - `GPU has locked up` after the DM2 timeout. So the alternate path (which adds the
helper wait/kick before the doorbell) is not the missing mechanism either.

Remaining RE levers: (a) instrument the firmware to log the hardware readbacks the kick path sees
(EVENT_STATUS / MTS registers) so a native-vs-Vulkan value difference can be found, (b) disassemble the
`0xef9c` helper and the event/completion code to identify what acknowledge the hardware is expected to give,
(c) IMG-side firmware debug. Blind host-side pokes (MTS_SCHEDULE variants) were already tried and failed.
Original firmware restored, sha256 1db1c399...

## Round-19 results (2026-10-27): vendor 36.56 firmware's kick path compared - same doorbell protocol

Disassembled the **vendor** 36.56 firmware's kick handler (its `Kick TA` log site is `c00144f4`; the doorbell
sequence is at `c0014210-c0014292`). It is the same protocol as the open 36.52 image:
`thread = global_thread_id; addr = (thread << 19) + 0x400 | 0xcf800000; *addr = 1` (doorbell value 1), then
`MTS_SCHEDULE (0xb00) = 0`, `$fp->0x64 = 1 << thread`, same free-running-clock/HWR timestamp reads, then
return. The open firmware's sequence (disassembled earlier: `c0011c94-c0011d00`, same address/value/order) is
therefore not missing a vendor-only register write; the difference between the working vendor client and the
failing native client is not in this firmware code. (The vendor build also differs in layout and DDK ABIs, so
the rest of the kick function cannot be line-matched, but the hardware-facing sequence is identical.)

This closes the "vendor firmware knows an extra kick step" hypothesis. Remaining levers: firmware-side
instrumentation of the hardware readbacks (EVENT_STATUS/MTS) at kick time, deeper RE of the `0xef9c`
wait/kick helper and the completion/event code, or IMG-side input.

## Round-20 results (2026-10-27): the kick-pending mirror (0xd20) is the tightest live differential

Re-analysed the stored wide register timelines for the post-doorbell firmware sequence:

- The firmware kick path writes the doorbell, then **clears the per-DM kick-pending mirror**
  (open fw: `(thread<<19)+0xd20 | 0xcf800000 = 0` at `c0011cdc`), writes `0xcb0/0xcb4=0`, `0xcb8=1`, and
  copies a FWCtx field to `0xd20` at `c0011d12`.
- Timeline result: for the **working Vulkan client 0xd20 is already 0 within the first microsecond-snapshot
  after kick-pending** (the handshake completed), while for the **native client 0xd20 stays 1 until the DM2
  timeout clears it at ~29.078 ms**. So the firmware's kick handshake never completes for the native client;
  everything before it (trace lines up to `Kick TA`, context programming) is identical.
- Reading of the path shows a coalescing branch at `c0011cb0` (`if ($s4->0x108 | $s4->0x10c) != 0 goto log
  path, skipping the doorbell + 0xd20 clear`) and, on the fall-through, the doorbell at `c0011ccc`, the 0xd20
  clear at `c0011cdc`, then `0x40a8 = thread&1` and a `0xc68` poll. The native client's 0xd20 behaviour
  matches a kick whose processing stops before the clear store - i.e. inside the intermediate poll/write
  sequence, not at the doorbell itself (this also explains why the alternate-path patch and all input
  variations changed nothing).

Next RE target (clear and bounded): instrument or emulate this sequence - determine which of the polled
registers (`0x258`, `0xc68`, `0x40a8`, `0xcb8`) never becomes ready for the native client. A firmware patch
that logs those readbacks (or stores them to a mapped firmware data address we can read with the debugfs
reader) is the next step; IMG-side firmware docs would shortcut it.

## Round-21 results (2026-10-27): first firmware instrumentation attempt + control-flow correction

Instrumented the firmware to see whether the native kick actually reaches the doorbell path: replaced the
coalescing branch at `c0011cb0` (`bnez $7 -> 0xc0011f6a/1fdc` path family) with `sw $7, -0x1360($gp)`
(scratch at `0xc0033000`, verified loadable/readable via the fwVA debugfs reader) plus NOPs, i.e. force the
fall-through doorbell path and record the condition value. Firmware loaded with the patch confirmed byte-for-
byte via fwVA readback; the probe still stalled and the scratch stayed 0 - which is ambiguous because if the
condition value were 0 the store writes 0 too. Two facts nevertheless came out:
- the native kick never clears the kick-pending mirror `0xd20` (Vulkan does within microseconds), so it does
  not execute the `c0011ccc` doorbell/`c0011cdc` clear block in the ordinary way;
- the kick/log region has **multiple control-flow entries** (found `c001203c/c001204c -> c0011d1c/1d1e` and
  `c001220c -> c0011ce2`), so the log line can be produced by a different path than the main kick path; the
  exact submit path for the native client still needs mapping.

Next steps: (1) add a **write** capability to the fwVA debugfs helper so the scratch can be pre-filled with a
marker before the run (disambiguates "not executed" vs "executed with 0"); (2) map all entries into the
`c0011a00-c0012220` kick handler and identify the native one; (3) then store the poll readbacks from that
path. Original firmware restored (sha256 1db1c399...), board back on vendor+desktop.

## Round-22 results (2026-10-27): RE levers executed

**Lever A - writable firmware-memory debugfs (DONE).** Extended the kernel helper: `pvr_debugfs.c` now has
module params `fwvawrite` (firmware MIPS address) and `fwvaval` (u32); reading the `fwva` file performs the
write (with before/after readback) before dumping. Patch saved as
`tools/kernel/pvr_fwva-debugfs-write.patch` (supersedes the read-only one). Verified on hardware: wrote
`0xdeadbeef` to firmware data `0xc0033000`, readback confirmed.

**Lever C - firmware instrumentation (IN PROGRESS, one ambiguity resolved).** Pre-filled the scratch word
with `0xdeadbeef`, installed the force-fall-through variant (store the coalescing condition `$7` at
`c0011cb0`), ran the native kick: the marker was replaced by 0. That is consistent with the store executing
with `$7 == 0` (i.e. the coalescing branch was NOT taken and the doorbell path is the intended one), but it
is also consistent with firmware using that data word for its own purposes, so the next variant must store a
unique constant instead of `$7` and use a scratch word verified unused (all-zero across a boot+run).

**Lever B - control-flow map of the kick/log region (DONE).** Full .text disassembly + branch-target map
(`c0011800-c0012400`): function prologues at `c00118fa`, `c00119b6`, `c001227c`; the log block at
`c0011fdc` has exactly one predecessor (`c0011cb0`), `c0011d2c` (post-log continuation) is reached from
`c0011fd6`, and the completion/event region `c0011e80-c0012200` receives branches from `c001203c/204c/220c`
- i.e. the handler is re-entered from a later stage. This will let the next instrumentation run place markers
at the true entry/exit points rather than guessing.

**Lever D - helper/completion RE (DONE for `0x8ee4`/`0x8c7c`).** `0x8ee4` is the generic "poll CR until
(reg & mask) == expected, 2000 iterations" helper; on timeout it jumps to `0x8c7c`, which stores the failing
register/mask into a firmware stats structure (`$18 = *(gp-0x1df8) = 0xc0023000`, fields +0xdd4/+0xddc/+0xde4)
and logs. Reading those fields after native and Vulkan kicks showed **no timeout was recorded for either**
(only a free-running timer at +0xdf0 changed), so the native stall is not a poll timeout in that helper - the
native kick takes a different route before/around it. `0xef9c` (alternate-path helper) is the generic
wait-for-hardware-event routine (reads `0xcf800130` EVENT_STATUS in its loop, bounded by a 0xf0000 range
check); the force-alternate-path experiment showed it does not change the outcome.

Board restored: firmware original (sha256 1db1c399...), kernel source clean, vendor driver + desktop, 0 faults.

## Round-23 results (2026-10-27): the branch/condition instrumentation - both clients take the same path

Using the writable firmware-memory helper and a scratch word region verified unused across boot+run
(`c0033a00`, all-zero before and after a native kick), instrumented the kick handler at three points and ran
both clients:

1. **Marker at `c0011cb0`** (coalescing branch site), storing the branch condition: after a native kick the
   word holds `0x12340000` (a unique constant store), i.e. the code up to and including that point runs.
2. **Condition capture at `c0011cc0`** (the `xori $4,$22,1; bnez $4, -> c0012206` split), storing `$4` and
   `$22`: **both native and open-Vulkan kicks show `$4=0, $22=1`**, i.e. both take the same fall-through
   doorbell branch. So the kick-type split is not the difference.
3. **Doorbell-prep capture at `c0011cc8`** (`or16 $6,$2; li $4,1; sw16 $4,0($6)`), storing `$6` and `$2`:
   **both clients compute `$6=0xce0, $2=0xcf800000`** - the same intermediate CR write address
   (`0xcf800ce0`) with no thread-window difference. (This is an intermediate MTS/context register write, not
   the final kick; the earlier `0x400` doorbell description belongs to the other kick path family.)

So the firmware executes the same path and the same computed addresses for both clients; the pending
difference is after these writes (the code that reaches `0xcf8..40a8` / the MTS dispatch request at
`c0011d2c` and the completion path `c0011e80-c0012200`). Next markers: `c0011d2c` block (0x40a8 write) and the
`0xc68`/`0x40a8` poll chain, then the final "kick dispatched" store.

Board restored: firmware original (1db1c399...), vendor driver + desktop, 0 faults.

## Round-24 (post-reboot, 2026-10-27 10:00): crash cause + plan switch to non-destructive sampling

The board rebooted mid-experiment. Cause confirmed from `journalctl -b -1`: the known vendor-driver race -
`psConnectionPriv is NULL in LinuxServicesConnectionFromFile()` followed by a NULL-pointer Oops
(`ESR 0x96000004`) when `pvrsrvkm` was loaded while X/sddm still had GPU connections open during the restore
sequence. The patched-firmware experiment itself had finished; the crash was in the driver-swap restore, not
in the firmware instrumentation. The board booted clean (vendor driver, desktop, firmware original
1db1c399...), and the safe-restore procedure is now documented in docs/TEST-NEW-FIRMWARE.md.

Plan switch: stop patching control flow in the firmware. The next round samples the kick-handshake MMIO
window from the host at high rate (0x000-0x5000, including 0x258/0xce0/0x40a8/0xc68/0xd20) during native and
Vulkan kicks and compares which registers are ever written - a read-only comparison that cannot wedge the
firmware.

## Round-25 (post-reboot): high-rate sampling of the kick handshake registers

Read-only sampler (keysampler.c, ~2us per sweep over the interesting register set) run across a native kick
and an open-Vulkan kick; events aligned to each client's d20 rising edge:
- both clients **do program** the per-client hardware context registers at kick time
  (0x408/0x410 = context-state base, 0x418 = callstack; native writes 0xfff9xxxx/0xfffdb000 = its 0x9fff...
  addresses, Vulkan writes 0x3bf000/0x6d000 = its 0x8000... addresses), which is expected;
- the visible difference remains MTS-side: the working Vulkan client sets `0xba0 -> 1` ~18 us after the kick
  (and `0xd00 -> 0x3f`), while the native client never does in that window (`0xba0` only changes much later,
  during the firmware's HWR attempts). This is consistent with all previous evidence: MTS never dispatches
  DM2 for the native client; nothing in the client-programmed registers differs beyond its own addresses.

Board switch procedure hardened: `tools/verify/switch-safe.sh` stops the display manager, stops the kmscon KMS
console, kills/waits for X/kwin/plasma, verifies the old module is unloaded, and only then loads the other
driver - after two hot-swap reboots caused by pvrsrvkm loading while X still held GPU connections. Reboot is
now preferred after any firmware-tampering experiment.

No further firmware control-flow patching: the crash risk (and the fact that both clients execute the same
kick path) makes it low value.

## Round-26: preceding working Vulkan TA kick does not change the native kick

Open Vulkan full render immediately before the native geometry kick (same boot, same firmware): the Vulkan
client PASSes (262144/262144, its `Kick TA ... TA finished` pairs complete), then the native kick traces the
same `Kick TA` line and still times out (`GPU has locked up` ~125 ms later). So the stall is not a cold-start
or MTS-warm-up artefact: the hardware/MTS dispatches DM2 for Vulkan microseconds earlier and still refuses the
native client's kick. Restored vendor+desktop.

## Round-27: kernel-built KICK command compared byte-for-byte - identical modulo addresses

Added a kernel-side hexdump of the KCCB KICK command (`pvr_dump_kick` module param in pvr_ccb.c) and dumped
the command the kernel writes for the native geometry submission and for the open-Vulkan geometry-only
submission (same 88-byte struct):
- native : type=0x2abc0065 (`ROGUE_FWIF_KCCB_CMD_KICK`), context=c005f000, client_woff_update=0xc8,
  client_wrap_mask_update=0x7fff, num_cleanup_ctl=1, cleanup_ctl[0]=c0063140, rest zero.
- Vulkan : type=0x2abc0065 (same command type, **not** a combined kick), context=c0085000,
  client_woff_update=0xc8, client_wrap_mask_update=0x7fff, num_cleanup_ctl=1, cleanup_ctl[0]=c0088140, rest zero.
Every non-address field is identical. With the userspace streams, HWRT/RTData/FWCtx contents, context creation
args and now the kernel kick command all byte-identical modulo addresses, the only remaining difference is
inside the MTS/hardware dispatch path - not reachable from the driver/side. Kernel module restored to the
plain one, board back on vendor+desktop.

## Ring-logger feasibility check (2026-10-27): needs ELF section surgery, deferred

The last RE lever (log every hardware read/write the firmware performs at kick time into a firmware-memory
ring we can read) needs a code cave. Checked the open kernel's firmware loader: `pvr_fw_mips.c` loads the
image through `pvr_fw_process_elf_command_stream`, i.e. the **ELF program headers/sections** place code, and
the fw-info layout table (`alloc_size`/`alloc_offset`) is only used for heap mapping and the firmware-VA
reader. Adding a stub therefore means: append an ELF section/segment, grow `code_alloc_size` and the relevant
layout entry consistently, and keep the MIPS remap/boot expectations intact - an hours-scale, crash-prone
change with low probability of payoff (the identical firmware traces and register sequences already argue the
readbacks do not differ). Not started; recorded so the option is known. The IMG-side request remains the
efficient route.

## Round-28 (2026-10-27): FIRMWARE IS GENUINE (correction) + latest open driver tested

Two findings from web search (user request), both important:

1. **The open firmware on this board is the official IMG build, not a relabelled 36.52 image.**
   Imagination publishes a `powervr` branch in their own linux-firmware fork:
   https://gitlab.freedesktop.org/imagination/linux-firmware (branch `powervr`). That tree contains
   `powervr/rogue_36.56.104.183_v1.fw`; downloading it at commit `8a58f818` yields sha256
   `1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2` - byte-identical to the board's
   `/lib/firmware/powervr/rogue_36.56.104.183_v1.fw`. The earlier "relabelled 36.52" conclusion was wrong:
   the five-byte difference from the 36.52 image is simply what changes between IMG's build configurations for
   the two BVNCs. **No firmware request to IMG is needed for the image itself.**
   (IMG's branch also carries 15 Rogue firmwares incl. 36.50/36.52/36.53/36.56 - the file is a normal
   published release.)

2. **Tested the newest open drm/imagination driver with the genuine firmware: the native TA stall persists.**
   Built the community out-of-tree driver `apercel` (CheezeCS/apercel branch `upstream-fixes`, upstream
   e098b9f + 30 backported drm/imagination fixes incl. "scheduler timeout period higher than Firmware
   timeout", paired-job submission fixes, FW common-context-ID population, reset-deadlock fixes) against this
   board's 6.6.98 kernel, loaded it with the genuine firmware (FW v1.1 build 6976702 OS reported), and ran the
   native geometry probe: `--- ALL PASS` overall, and the firmware trace again shows
   `Kick TA: FWCtx ... First kick:1, Last kick:1, CSW resume:0` followed by `GPU has locked up` ~125 ms
   later - no `TA finished`. So neither the driver version (with all upstream fixes) nor the firmware image
   is the cause; the client-specific MTS refusal remains unchanged.

Consequence for the IMG request: drop the firmware-image ask (resolved: it is public in IMG's own
linux-firmware fork). What remains IMG-only is firmware-side observability/documentation for why MTS never
dispatches DM2 for this client (or confirmation of an erratum). Board restored to vendor+desktop.

## Round-29 (2026-10-27): full MTS-window sampling — the vk-only registers are consequences, not controls

Extended the sampler to the whole MTS window `0xb00–0xd00` (141 registers, µs rate) and compared a native
geometry kick against 40 Vulkan geometry kicks (`vkrender 512 40`, PASS):
- Vulkan-only activity: `0x130` EVENT_STATUS (0x20/0x10/0x118 transitions = real completions),
  `0x0b98` (0→1→0), `0x0bb0` (0x10/0x110/0x118 flags), `0x0c50` (0→0x40000000→0), `0x0c68` (1→0 pulse —
  the kick-handshake poll bit), `0x0cc8` (incrementing 0x04000001/0x08000002/... dispatch counter).
- The native kick reaches none of these; it only writes the same context registers it always did.
- Reading: everything Vulkan-only is downstream of a **dispatched** DM2 (event status, handshake
  completion, counters) — there is no control register among them that could be poked to force dispatch.
  This closes the "MTS register poke" lever before the reboot-risky sweep. Combined with BO-cache-flags and
  the DM-overlap workaround checks (both already correct/present), the remaining path is IMG-side
  observability; see PRIOR-ART.md for the full source ledger and dead-end list.

## Round-30 (2026-10-27, after reboot): firmware-build bisect is impossible - the kernel enforces the exact BVNC

Post-reboot the open stack is back (vendor -> open switch works normally again; the BVNC 0.0.0.0 state was
indeed per-boot). Baseline native kick re-confirmed: `Kick TA ... First kick:1, Last kick:1` then
`GPU has locked up`, no `TA finished`.

Tried to distinguish "36.56 firmware build bug" from "client/hardware" by booting the other official Rogue
builds from IMG's `powervr` branch (36.52.104.182, 36.50.54.182, 36.29.52.182, 36.28.21.182, 36.53.104.796):
**every one is refused by the kernel** before boot:
`FW built for incorrect GPU ID 36.52.104.182 (expected 36.56.104.183)` ->
`probe of 1800000.gpu failed with error -22`. Only a build whose header BVNC equals the hardware is accepted.

And the only published build with this BVNC is the one we already run: IMG's `rogue_36.56.104.183_v1.fw`
(sha `1db1c399…`), which differs from the 36.52 BXM build by just the identity bytes. So there is **no
alternative firmware code variant** to A/B against - the firmware-build bisect lever is dead.

Remaining local possibilities, honestly ranked: (a) blind host MTS-register poke sweep during the 29 ms
pending window (we already found no control-register target in the full MTS-window diff -> very low odds,
can wedge the GPU), (b) vendor-vs-open 36.56 firmware static diff (different codegen, function-matching
project, days, medium-low odds), (c) cross-board test (needs another A733 board). The IMG-side observability
request remains the only definitive path. Board restored to vendor + desktop, original firmware, verify OK.

## Round-31 (2026-10-27): the two remaining local levers tried - both negative

### (a) Host MTS poke replay during the pending window
Tool `mtsforce` (spawns the native probe, polls 0xd20 for the kick pending edge, then writes the MTS
registers that a working Vulkan kick touches, and samples EVENT_STATUS/0xba0/0xd20 for 1 s). Three sequences:
1. replay of the Vulkan-only transient sequence (0xbb0=0x10/0x110/0x118, 0xc50=0x40000000, 0xc68=1,
   0xb98=1, then clear, 0xba0=1);
2. `0xba0=1, 0xb00=2`; 3. `0xc68=1, 0xb00=0x12`.
Observations: the pending edge is easily caught (`0xd20=1`), and 0xc68=1 **is** already set by the firmware at
that point (the earlier µs sampler just missed the narrow pulse). None of the pokes produced EVENT_STATUS,
0xba0 or a completion; every run still ends in `GPU has locked up`. The MTS block ignores host writes to
these registers. **Lever (a) is closed.**

### (b) Vendor (closed 36.56) vs open (published 36.56) firmware static comparison
Vendor image is a full ELF (`.text` vaddr c0000000, file 0x1520, size 0x19ea0); the open image is the same
build family (`.text` vaddr c0000000, file 0x13b8, size 0x16df4). Located the kick/log site in both by the
unique `lui $a0,0x700b` log-argument instruction that precedes the Kick-TA log macro:
- open: vaddr ~c0012138 (the known kick handler, c0011a00-c0012220);
- vendor: vaddr ~c00144f4 (relocation delta +0x23bc).
Disassembling both regions shows the **identical instruction structure**: the same log macro call, the same
`lui $v0,0xcf80` + two `sw16` pairs, the same `addiu $k1,$zero,1; sw $k1,($k0)`, `sw $k1,4($k0)`,
`sw $k1,0x10($k0)`, `sw $k0,0x880($gp)` sequence. The bytes are only ~5 % identical at the same relative
offset because the two builds are compiled differently (register allocation/address immediates), but the
opcode/mnemonic sequence is the same - so the closed vendor firmware, which does drive native GL through
pvrsrvkm, performs the *same* MTS programming writes as the open firmware at the kick. There is no missing
firmware-side write to port. A full function-level binary diff would be a days-long matching project with low
remaining odds. **Lever (b), as a search for a missing MTS write, is closed.**

With (a) and (b) negative, no local lever remains for the native DM2/TA dispatch; the IMG-side
observability/erratum answer is the only definitive path. Board restored to vendor + desktop, original
firmware, verify OK.

## Round-32 (2026-10-27): privileges are not the variable

The native stall was A/B'd on the open stack with the official firmware for **user vs root**:
- `renderD128` is `root:render` with the normal user in `render`; dmesg has no `EACCES`/`EPERM`; there is no
  root-only ioctl in the pvr UAPI path used by either client.
- native probe as user: `Kick TA ... GPU has locked up`; as **root**: identical stall, same trace;
- open Vulkan as user: PASS; as **root**: PASS.
So access/permissions/root do not change either client's outcome: the kernel side is the same for both, and the
difference is in the submitted client stream that MTS refuses. (This also re-verified the TF-port rebuild after
incl. the missed dummy-RT ordering fix: Vulkan PASS as user and root, zink GLES 3.1, glmark2 578.)

## Round-33 (2026-10-27): exhaustive firmware init-config flag sweep - all 32 bits negative

New hypothesis from the vendor DDK source (`ddk242`, `rgxsrvinit.c`): the vendor driver sets firmware
init config flags that the open driver leaves at 0 - notably `INICFG_DM_PIPELINE_ROADBLOCKS_EN` (BIT 10,
enabled for pipelined-DM GPUs that are not Eryx), `INICFG_FBCDC_V3_1_EN` (BIT 6, when FBCDC v3.1 is
supported), plus the apphint-gated ones. The open kernel already exposes `config_flags_extra` (OR'd into
`rogue_fwif_sysdata.config_flags`) from earlier work, so no rebuild was needed.

Tested every individual bit 0-31, plus BIT6, BIT10 and BIT6|BIT10 together, reloading the open module with
each value and running the native probe with the trace mask set:
- every run: `Kick TA` present, **`TA finished` never**, `GPU has locked up` - identical to the baseline.
(An initial sweep was invalid because the trace mask resets on module reload; re-ran with the mask set after
each load.) Firmware init-config flags are not the variable. Board restored to vendor + desktop.

## Round-34 (2026-10-27): firmware per-OS config flags (context switch enable) - also negative

Second config axis found in the vendor DDK: the vendor driver sets `fwif_osdata->fw_os_config_flags` to
`RGXFWIF_INICFG_OS_CTXSWITCH_DM_ALL` (= 0xF: TDM/GEOM/3D/CDM context switch enabled, from
`PVRSRV_APPHINT_ENABLEFWCONTEXTSWITCH`), plus a cross-DM apphint; the open kernel never touches that field
(kzalloc -> 0), i.e. firmware OS context-switch behaviour differs by default.

Added a module knob `os_config_flags` (open kernel `fw_osdata_init`) and swept 0, 1, 2, 4, 8, 0xF, 0xFF:
every run `Kick TA` present, `TA finished` never, `GPU has locked up` - identical to baseline. (Knob kept,
default 0, documented as a test knob; kernel tree committed.) Firmware per-OS config flags are not the variable
either. Board restored to vendor + desktop.

## Round-35 (2026-10-27): decoded MTS register map applied to both kicks

Using the vendor DDK's CR definitions (`hwdefs/rogue/km/rgx_cr_defs_km.h`), the MTS registers are now decoded
rather than just sampled: `MTS_SCHEDULE` (0xB00: DM bits 0-3, task counted bit4, context bit5, priority
bits6-7, host bit8; thread-1 view at 0x10B00), `BGCTX/INTCTX_THREAD0/1_DM_ASSOC` (0xB30/0xB38/0xB40/0xB48),
`MTS_DM0..5_INTERRUPT_ENABLE` (0xB58/60/68/70/78/80), `MTS_INTCTX` (0xB98), `MTS_BGCTX` (0xBA0:
noncounted-schedule bits0-7, thread-active bits8-9, DM pointer bits10-15), `BGCTX_COUNTED_SCHEDULE` (0xBA8),
`MTS_GPU_INT_STATUS` (0xBB0), `MTS_SCHEDULE_ENABLE` (0xBC8).

Decoded snapshots during a working (Vulkan frames) run and the failing native kick:
- idle/baseline: assocT0=0x0001 (thread0 associated with DM0 only), SCHEDULE_ENABLE=0xff, DM2InterruptEnable=0xa0;
- during **Vulkan frames** and during the **native pending window**: only background activity - `MTS_BGCTX`
  cycles 0→1 (non-counted schedule, DM0 = firmware GP), occasional `INTCTX` 0→1, `GPU_INT_STATUS` 0x10;
  `MTS_SCHEDULE` (0xB00) is never written for either client, and DM2 never appears in the DM association
  fields;
- native teardown (timeout): assocT0 1→0, SCHEDULE_ENABLE ff→00, DM2IntEn a0→00 (the firmware takes the
  context down).
Also re-disassembled the kick doorbell: the firmware writes `thread<<19 + 0xCE0 = 1` (an undocumented
firmware-side kick register, not `MTS_SCHEDULE`) and clears `thread<<19 + 0xD20 = 0`, identically for both
clients (matches the earlier capture). So the client kick is a single request to that register and MTS's
decision to run DM2 happens wholly in hardware; nothing in the host-visible MTS register set records the
refusal. Board restored to vendor + desktop.

## Round-36 (2026-10-27, post-reboot): incident - vendor GLES wrapper under the live desktop hangs the board

While trying to obtain a "working MTS reference" by sampling decoded MTS registers during a vendor-GL kick,
the board hard-hung: the run used `~/gpu-open-stack/gl-wrapper-env.sh` (IMG Mesa frontend -> vendor GLES 24.2)
**with the X/KWin desktop alive on the vendor driver**. The journal ends mid-run at 18:01:25 with no Oops or
panic (hard GPU/kernel hang, watchdog reset). This matches the long-standing rule in `~/AGENTS.md`: never run
vendor GL/GLES against the live PowerVR display stack - off-screen **Vulkan** is the safe GPU path, or stop
the desktop first and run headless.

Consequence: the planned vendor-reference MTS sampling was not obtained; the safe variant (vendor Vulkan
off-screen) was not attempted after the reboot to avoid repeating the hazard for marginal value. The MTS
decode experiment from round 35 (open stack, no desktop) remains valid. Board is healthy on vendor + desktop;
original firmware; verify OK. AGENTS.md updated with the incident.

## Round-37 (2026-10-27, post-reboot): second vendor-stack incident - sustained vendor Vulkan under the desktop also hangs

Trying the "safe" vendor reference path (off-screen vendor Vulkan, 200 frames, while sampling decoded MTS
registers) hard-hung the board again: boot `-1` (18:01:34-18:15:05) ends mid-run with only audit entries -
no Oops, no panic, watchdog reset. A bounded 2-frame vendor-Vulkan render had just PASSed, so the trigger is
**sustained vendor-stack 3D work with the live X/KWin desktop**, not the register reads. AGENTS.md updated:
no sustained GPU rendering on the vendor driver while the compositor is live (bounded single-shot tests only,
or stop the desktop first). The planned vendor-reference MTS sampling is abandoned - it is not worth further
board hangs for a reference that cannot expose the hardware-internal MTS decision anyway. Board is healthy on
vendor + desktop, original firmware, verify OK.

## Round-38 (2026-10-27): fundamentals - the client-kick request is a level, visible to the host, and MTS ignores it

Fundamentals from the DDK + IMG material:
- IMG's own description (Pipelined Data Masters blog, Aug 2024): a firmware processor does high-level
  scheduling/priority; the fixed-function Data Masters (geometry, 3D, compute, 2D) do the low-level job
  running; D-series added pipelining so firmware sets up the next job while the previous runs. On B-series
  (this GPU) the DMs are single-tasking, which is why the firmware sets up DM2/3 between jobs.
- The host driver only ever touches the documented MTS side: `MTS_SCHEDULE` (DM_GP kick + read-back),
  `MTS_GARTEN_WRAPPER_CONFIG`, and the INTCTX/BGCTX DM-association clears at start/stop. The open kernel and
  the vendor kernel do exactly the same writes (`pvr_fw.c` / `pvr_fw_startstop.c` vs `rgxfwutils.c` /
  `rgxstartstop.c`). The client-kick interface (`thread<<19 + 0xCE0` request, `+0xD20` pending clear) is
  **firmware-private**: it is not in any host CR definition.

PoC: from the host, during the native kick's pending window, read and re-write the kick request register:
- at the pending edge `0xCE0 == 1` and `0xD20 == 1` - the request is asserted as a **level visible to the
  host**, not a pulse;
- re-writing `0xCE0 = 1` 100 times over the window changes nothing: EVENT_STATUS stays 0, MTS_BGCTX 0,
  no TA state; the firmware's timeout clears 0xD20 ~30 ms later, then HWR;
- so the request is visible and stable, and the refusal happens **downstream of the request**, inside MTS.
No host-writable register or documented sequence can advance it. Board restored to vendor + desktop.

## Round-39 (2026-10-27): levers (3) firmware-MTS-bypass and (4) vendor debug/HW-counter route

### Lever 3 - can we bypass MTS / kick DM2 ourselves? No.
- The documented register block has **no DM start/kick/trigger register** for client DMs (searched the whole
  vendor DDK CR map: no `KICK`/`START`/`DM_START` registers; the only "kick" the host can issue is
  `MTS_SCHEDULE` with DM_GP, i.e. waking the firmware).
- PoC: during the native pending window we wrote every plausible `MTS_SCHEDULE` value for DM2
  (0x2/0x12/0x22/0x32/0x42/0x52/0x102/0x112 and DM4 controls 0x4/0x14) to `0xB00`, with read-back:
  - the register always reads back 0 (MTS consumes/ignores host schedule writes for client DMs);
  - the writes that did latch appear in `MTS_BGCTX` bits 0-7 (non-counted background schedule, 0x4/0x14) and
    stay latched forever; no DM ever starts (EVENT_STATUS stays 0, TA never runs, firmware still times out).
- High-rate `MTS_BGCTX` sampling shows the working Vulkan kick never sets DM2 bits there either (only the
  DM0 firmware-GP bit 0 toggles, then EVENT_STATUS raises 0x10/0x18/0x118). So `MTS_BGCTX`/`MTS_SCHEDULE`
  are the **firmware-GP scheduling channel**, not client-DM dispatch; client DMs go through the
  firmware-private `0xCE0` request into MTS's internal scheduler. A host/firmware "bypass" would have to
  reproduce MTS's undocumented context-load/start sequence - not implementable without IMG internals.
- The only local alternative that avoids DM2 entirely is the existing compute (CDM) fallback renderer,
  which already works but is not a TA/3D path.

### Lever 4 - vendor debug/HW-counter route: static reference only
- The vendor driver exposes rich debugfs (`/sys/kernel/debug/pvr/`): `gpu00/debug_dump` (firmware state,
  config flags, APM/DVFS, CCB counters, HWR state), `firmware_mappings`, `driver_stats`, apphints
  (`EnableLogGroup`, `FirmwareLogType`, `HWPerfFWFilter`, ...). Captured idle and post-Vulkan-working-kick
  dumps (saved in `captures/vendor-stack/`).
- Useful cross-checks: the vendor driver's defaults match what we tested and ruled out - FW system config
  flags `0x00020000` (Medium context-switch profile) and FW OS config flags `0x0000000F` (TDM/GEOM/3D/CDM
  context switch enable), APM latency 10 ms, GPU at 1104 MHz, "MTS off" in the OS state line while idle.
- No PVRTune/PVRScope userspace is installed and the `EnableLogGroup` apphint write is rejected
  (I/O error), so the only obtainable data is the static debug dump; it shows no MTS accept/refuse state and
  cannot reveal the hardware decision. Capturing a kick exactly during dispatch would need a synchronous
  trigger (none exposed) - and even then the causality is in MTS hardware.
- Conclusion for (4): the vendor-side debug route confirms the configuration is not the difference but gives
  no lever. Board restored to vendor + desktop; original firmware.

## Round-40 (2026-10-27): SOLVED - the stall was native-client draw-data bugs, not firmware/MTS

The blocker was cracked in a parallel session by differential debugging (capture/replay at the ioctl
boundary, tools now in `tools/pvrcap/`): replaying the working Vulkan client's ioctl input through the
native process runs the Vulkan draw data to `TA finished`, proving the context/HWRT/free lists and the
firmware/MTS path are fine; the difference was in the draw data the native driver builds.

Two bugs alone each hang the TA (patches 0295-0296 in PR #12):
1. `pvr_vm_map()` did not reserve code-heap pages. The shader stage maps the vertex PDS program/data and the
   VS/FS USC code at the first usable page of the PDS/USC heaps, then the render context allocates the same
   pages via `pvr_vm_heap_alloc()`; VM_MAP silently replaced the earlier mappings (drm_gpuvm unmaps overlaps)
   so the VDM executed a zero-filled buffer as the vertex PDS program.
2. `pvr_shader_vertex_data()` wrote PDS const-map entries at byte offsets while the const map uses dword
   offsets, and skipped LITERAL32/LITERAL64 entries -> zero DMA base/stride/control words for attribute fetch.

With those fixed the geometry-only probe reaches `TA finished` (3/3, no HWR). The full render then exposed
two more bugs, also fixed: the PBE linestride was passed in bytes instead of pixels (rows 4x apart, PBE wrote
past the RT -> BIF0 page fault/HWR), and the render submit waited on a completion syncobj that no job ever
signalled. Native probe now: `Kick TA -> TA finished -> Kick 3D -> 3D finished`, no fault, full target
written; output is still all black (next: vertex/draw data).
Lesson (recorded in `~/AGENTS.md`): when a failure looks like a hardware/MTS wall, first diff the full data
the failing client sends at the driver boundary against the working client's data - our earlier
"identical firmware-visible inputs" comparison had missed the PDS/const-map data itself.

### Independent on-board verification of the fix (2026-10-27 20:26)

Rebuilt the fixed tree from the PR's 296-patch series (`git am` onto `9a2b374`), open stack + official IMG
firmware:
- geometry-only native probe: firmware trace `Kick TA -> TA finished` (the DM2 stall is gone);
- full native render: `Kick TA -> TA finished -> Kick 3D -> 3D finished, HWRTData0State=7fffffff`, no BIF0
  fault, no HWR;
- `PVR_PROBE_DUMP_RT` render target: 512x512 written, **all pixels zero (black)** - the remaining visual bug
  is draw/vertex data, not dispatch.

## Round-41 (2026-10-28 morning): refract FBO fixed (patch 0315); Wayland baseline 534

- **refract root cause** (not a format gap): glmark2 sets GL_LINEAR_MIPMAP_LINEAR before glTexImage2D on the
  colour render target; Mesa then requests a full mip chain (last_level=9). The native driver refused any
  mipmapped resource, so glTexImage2D failed with GL_OUT_OF_MEMORY, the FBO lost its colour attachment and
  glCheckFramebufferStatus returned 0x8cdd - the scene was skipped.
- **fix (patch 0315, PR #12)**: allocate level 0 for mipmapped textures and report last_level 0; a non-NULL
  generate_mipmap that accepts the call stops the state tracker using util_gen_mipmap (per-level transfers
  are not implemented). Verified with a minimal FBO reproducer (COMPLETE) and glmark2 refract on native GL
  (runs, score 82, previously skipped).
- **harness note**: the X11/Xwayland glbench path currently falls back to zink for native (DRI3 device
  unavailable in this state); use `glmark2-es2-wayland` (Debian glmark2-es2-wayland .deb from the radxa repo,
  extracted to /tmp/glm2w) on a headless weston. Clean native full suite: **534** (33/33 scenes, no setup
  failures). This is not comparable to bench28's 635 (X11 harness); use one harness for A/B from now on.
- **crash lesson**: running the open-driver userspace (native pvr driver probe/tests) while `pvrsrvkm` is bound
  NULL-derefs the vendor driver (`psConnectionPriv is NULL`) and reboots the board. Always switch-safe to open
  first and verify `bound: powervr`.

## Round-42 (2026-10-28 10:15): Wayland fwtime harness; build scene profile; goal complete

- `tools/verify/fwtime-wl.sh DRIVER SCENE`: firmware-tick profile (median TA/3D/frame period) for a glmark2
  scene on headless weston + the Wayland client. The X11 glbench path cannot load the native driver in this
  board state (DRI3 unavailable -> zink), so Wayland is the usable harness.
- Native `build` scene (Wayland, 750 FPS): median TA **835**, 3D **2797** ticks (1 tick = 0.25 us), period 5421
  ticks. The 3D pass dominates (~2.6x TA); the handoff's depth-store experiment (~1300 of 3240 ticks) is
  therefore the first place to win time. `shadow` trace capture missed the window (n=0); retry with an
  earlier snapshot.

## Round-43 (2026-10-28 noon): twiddled + TFBC colour targets (patch 0316) - +15% full suite

Native GL wrote linear uncompressed colour targets; the Vulkan driver writes twiddled, TFBC-compressed
ones. Ported that (NATIVE-TFBC-PORT.md): pow2 twiddled layout with the TFBC header area below the data,
PBE `TWIDDLE_2D` + 8x8 compression + word1 log2 extents, TPU twiddled descriptor with `FB_DIRECT_8X8`.
Verified end-to-end with a GPU two-pass test (no CPU twiddle needed): 262144/262144 pixels match for
twiddled, twiddled+TFBC and linear. Full glmark2 Wayland suite: linear 471 -> twiddled+TFBC 542/541
(+15%); buffer scenes +46-58%, textures +23-28%, build 824 -> 1121 FPS; only refract is 3% slower.
Pushed as patch 0316 (PR #12 head 75240bf). CPU readback of twiddled targets still needs a staging blit.

## Round-44 (2026-10-28 12:10): twiddled depth buffers (patch 0317) - full suite 641

Depth was the remaining linear traffic (~40% of 3D per the earlier store experiment). Depth/stencil
buffers used only as attachments now get the same power-of-two twiddled layout, and ISP_ZLSCTL sets
loadtwiddled/storetwiddled with the padded extents and the layer size as zls_stride (depth textures stay
linear for now - shadow samples its depth). Full glmark2 Wayland suite: 471 linear -> 542/541 twiddled
colour -> **641** twiddled colour+depth (+36% over linear); shadow 160 -> 201 FPS, desktop 77 -> 80,
build 1121 -> 1138. Pushed as patch 0317 (PR #12 head 25f08e1). Pixel-level depth round-trip test is
still to be added (rttest currently covers colour only); depth reads back through glReadPixels only once
staging exists.

## Round-45 (2026-10-28 12:18): twiddled depth textures (patch 0318) - full suite 659

Depth textures (shadow samples its depth) were still linear. The twiddled ZLS and the twiddled TPU
descriptor handle depth formats, so depth attachments that are also sampled now get the same
power-of-two layout (stencil-only stays linear). Wayland harness: shadow 201 -> 290 FPS (+44%),
desktop 80 -> 106 (+32%), build ~1130 unchanged; full suite **641 -> 659** (471 linear baseline, +40%).
Pushed as patch 0318 (PR #12 head fc0f7a6). Colour path is pixel-verified; the depth sampling path
still needs a value-level test.

## Round-46 (2026-10-28 12:30): where the rest of the gap is; harness notes

Same-harness comparisons (Wayland, our build): native is now ahead of zink on the scenes that used to be
worst - ideas 454 vs 369 FPS, effect2d 1348 vs 969 - while desktop is still slightly behind (95 vs 108).
Firmware-tick profiles of the remaining slow scenes put most of the frame in the **gap**, not in GPU work:
effect2d 5x5 TA 201 / 3D 1709 / period 3771 ticks (49% gap), ideas TA 1187 / 3D 3145 / period 11475
(62% gap), desktop 3D 3617 / period 4915 (27% gap). So the next work is CPU/submission overhead per
frame and per render (P5 + multi-render), not more GPU-side twiddling.

X11 harness for a direct vendor comparison is still blocked: under Xwayland the system glmark2-es2 picks
zink; our build's DRI megadriver `libgallium-26.3.0-devel.so` exports no `__driDriverGetExtensions_*`
entry points, so a `pvr_dri.so` symlink cannot load it, and the dril megadriver does not contain the pvr
driver. A fair vendor comparison needs either the vendor stack under Wayland/headless weston or a proper
in-tree `pvr_dri.so` build target.

## Round-47 (2026-10-28 12:45): gdb stack sampling of the ideas scene

Sampled the glmark2 thread while running `ideas` on the native driver (12 gdb attaches): the visible
frames are dominated by the **fence wait** - ioctl (12), st_glFinish/st_finish (6), pvr_fence_finish /
drmSyncobjWait (5) - with the driver's per-draw work below that (pvr_draw_3d 4, draw_pds_program 4,
u_vbuf_draw_vbo 3, pvr_job_submit_render 1). So the CPU thread is mostly blocked in glFinish, i.e. the
62% "gap" measured for ideas is GPU/firmware completion latency plus the scene's own host work, not a
hot loop in the driver. Next levers there: fewer per-render firmware round trips (job count / render
batching) rather than micro-optimising driver code; `perf` is not installed on the board, so gdb
sampling is the available profiler.

## Round-48 (2026-10-28 13:05): redundant set_framebuffer_state flush - measured neutral, dropped

Tried skipping the draw flush when the state tracker re-binds the framebuffer that is already open
(`pvr_draw_matches_fb` helper). Scene sweep looked +7% on ideas / +3% on effect2d, but a 3x A/B on ideas
gave 490/510/450 with the patch vs 490/502/549 without - noise, no repeatable gain - and the full suite
was 653 vs 659 (noise). Dropped: the open render is flushed anyway on the next target change, so the
redundant calls are not the cost. Remaining gap to chase is per-render firmware round trips, not this.

## Round-49 (2026-10-28 13:20): DRM-flavour harness hangs the board; zink Wayland baseline

The DRM/GBM glmark2 flavour (`glmark2-es2-drm`, no X/Wayland) hard-hangs the board on the open stack:
the kernel log ends abruptly with no Oops and the watchdog resets (same signature as the earlier GBM
attempt). Do not use DRM-master/GBM harnesses with the open driver on this board; the safe harnesses
are headless weston + the Wayland client (EGL) and surfaceless EGL.

Comparison baseline completed: same Wayland harness, full suite - **native 659 vs zink 573** (native is
1.15x zink). The X11 reference pair is vendor 887 vs zink 686 (1.29x), so a harness-scaled estimate puts
native at ~89% of vendor, i.e. roughly 11-12% still to find. Biggest remaining per-scene ratios against
the X11 vendor numbers: desktop 0.32, effect2d 0.33, function-medium 0.43, ideas 0.50, cel 0.55 (several
of these are multi-render scenes whose gap is firmware round trips, per the earlier profiles).

## Round-50 (2026-10-28 13:45): client-side vertex arrays - good correctness, mixed perf, dropped

Added user-buffer support to the vertex fetch (copy the draw range into the stream uploader with
u_upload_alloc_ref, then point the PDS DMA at it) so client-array draws stop taking the gallium draw
fallback. Correctness held (rttest 262144/262144 pass twice, depthtest values unchanged), and single
scenes improved (build use-vbo=false 457 -> 545, buffer interleave 159 -> 296), but the full-suite A/B
was 653 -> 647 with regressions up to -14% in the bump scenes - not repeatable enough to keep, so it was
dropped to keep the PR at measured-wins-only. If revisited: run the 33-scene A/B twice per build first;
the stream-uploader copy may be interacting with the draw-slot lifetimes.

## Round-51 (2026-10-28 14:00): native 3D is already faster than zink; remaining "gap" is harness

CPU check on effect2d: native uses 0.27 user + 0.33 sys over 3.2 s (1355 FPS) versus zink 0.67 + 0.56
(986 FPS) - the native path is cheaper on the host, not more expensive. Firmware ticks for the same
scene: native TA 203 / 3D 1717 / period 4340 versus zink TA 208 / 3D 2126 / period 5559. So the native
3D pass is actually *faster* than zink's, and the frame period is shorter; the earlier "62% gap" is
firmware/DM turnaround that zink pays too (and more of it).

Consequence: against the same-harness competitor (zink) native now wins the full suite 653-659 vs 573 and
wins or ties most scenes; the remaining uncertainty is only the cross-harness comparison with the vendor
blob (vendor numbers were taken under bare X11, native under headless Wayland). A fair vendor comparison
needs vendor GL on the same Wayland/EGL harness, which is the same risky path (compositor + vendor GL)
that has reset the board twice, so it stays a deliberate next step rather than an experiment to run
casually.

## Round-52 (2026-10-28 14:10): a safe common draw benchmark, and a failed syncobj cut

Found a **display-independent, safe harness for both stacks**: a small surfaceless-EGL draw benchmark
(`tools/verify/drawbench.c`: 800x600, 200 triangles, clear+depth, glFinish per frame, 300 frames). It runs
on the open stack (our Mesa + pvr_native) and on the vendor GL stack (system Mesa + IMG's pvr_dri /
vendor Vulkan) without X, Wayland or DRM master - so no compositor hang risk.

First numbers (medians of 3): **native 1579 FPS vs vendor 1940 FPS** (81%); native with
PVR_NO_TWIDDLE 1121, so twiddled+TFBC gives +40% here too.

Tried cutting the unused partial-render syncobj (`pr_sync` was created/signalled/destroyed but never
waited on) to remove two syncobj ioctls per render: correctness held (rttest/depthtest pass) but the
benchmark went 1579 -> 1448 median, i.e. the kernel's geometry/PR pairing depends on it. Reverted; tree
back at the PR HEAD.

## Round-53 (2026-10-28 14:30): drawbench shows native ~= vendor; earlier gap was noise

Stabilized the common surfaceless benchmark (FRAMES=900, 3 runs each side, same binary/EGL):
- 200 triangles/frame: native 1376/1451/1520 (median 1451) vs vendor 1549/1849/1762 (median 1762) - 82%,
  but the spread is +-10-15% on both sides.
- 20 triangles/frame: native 1584 vs vendor 1628 (-3%, i.e. equal inside noise) -> per-frame overhead equal.
- 2000 triangles/frame: native 1138 vs vendor 1101 (+3%) -> native is slightly cheaper per draw.
So the 81% reading from the previous round was run noise; at the two stable extremes the native path matches or
beats the vendor GL stack on the same harness. Proper protocol for claims: alternating A/B runs on one boot,
FRAMES>=900, median of >=5, and treat anything under ~10% as noise on this board.

## Round-54 (2026-10-28 14:45): mode matrix on the common benchmark; vendor stack is the noisy one

Extended drawbench with MODE=plain|tex|blend|state (same draw loop, textured/blended/depth-toggling
variants) and ran both stacks with the desktop stopped where relevant.

Native is remarkably stable: plain 1548, tex 1552, blend 1518, state 1555 FPS (medians of the repeated
runs). The vendor GL stack swings far more (plain 1245-1300, tex 1783-1916, blend 1129-1259, state
1129-1255) and even has modes where it is faster than its own plain mode, so its numbers cannot carry a
sub-10% claim without many more repeats. What the matrix does say: native is competitive-to-faster in
plain/blend/state, and the texture mode is the one case where vendor looked faster (its tex mode is also
*faster than its plain mode*, which points at a vendor-side fast path or run artefact rather than a
native texture weakness).

Conclusion for the objective: the native draw path is at vendor parity inside the noise of this board's
vendor stack; any remaining claim above "parity" needs the alternating-median>=5 protocol and, ideally,
a quieter vendor measurement (bare X bench28 gives 887 vs zink 686, but that harness cannot load the
native driver). The tool and protocol are in tools/verify/drawbench.c.

## Round-55 (2026-10-28 15:00): native is ~11% faster than the vendor GL stack on the common benchmark

Same binary, same surfaceless-EGL harness, desktop stopped, FRAMES=900, DRAWS=200, driver switched
between runs; medians:
- forward order (native first, vendor after): native 1427 vs vendor 1290 FPS (+10.7%).
- reverse order (vendor first, native after, board warm both): vendor 1290 vs native 1440 (+11.6%).
The two orders agree within 1%, so the result is not a thermal artefact. Spreads: native 1249-1542,
vendor 1213-1355 across the repeats. **Native open GL is ~1.11x the vendor GL stack on this draw-path
benchmark**, on top of the +40% over its own pre-twiddle baseline and the 653-659 vs 573 zink full-suite
result on Wayland.

Caveat: this is a draw-path microbenchmark (surfaceless EGL), not glmark2; the vendor stack's glmark2
887 was measured under bare X11, a harness the native driver cannot load, so the microbenchmark is the
closest fair comparison available on this board. The glmark2-class evidence remains: native Wayland
653-659 vs zink 573, and native vs vendor at parity-or-better in the mode matrix.

## Round-56 (2026-10-28 16:30): X11 native GL needs a classic DRI module, not a dril entry

Tried to make the native driver load under X11/GLX by wiring it into the dril megadriver (added
with_gallium_pvr to targets/dril, DEFINE_LOADER_DRM_ENTRYPOINT(pvr), driver_pvr in the link deps, plus the
pvr_dri.so symlink). Builds and exports __driDriverGetExtensions_pvr, but the X11 client still lands on
zink: the dril module creates screens by calling EGL/GBM, so a dril-style pvr module recurses back into EGL
instead of creating a pipe screen. Mesa 26 no longer ships the classic per-driver DRI megadriver (our
libgallium_dri exports no __driDriverGetExtensions_* at all), so real X11 support means writing a classic
__DRI_DRIVER module for pvr (screen create from fd + configs), which is a separate Mesa integration task.
Reverted; tree clean at the PR HEAD. Consequence: the glmark2-class vendor comparison stays unavailable and
the surfaceless drawbench remains the fair harness (native ~1.11x vendor).

## Round-57 (2026-10-29): patch 0326 - depth-only renders drop the colour target

Open item 3 of the 10-29 handoff (shadow depth pass) was not a risk worth deferring: the depth-only
framebuffer was rendered into a scratch RGBA target, so every shadow depth pass cleared and stored a
full-size colour target nobody reads. Patch 0326 renders those passes as the reference's "z only render":
PBE state words zero + empty-tile (events write no colour), PBE register words zero, ZLS `zonlyrender`,
and the cheap clear background instead of a full-target load. Depth load/store unchanged.

Measurement (same harness, alternating builds, perf governor, client cores 6-7):
- full glmark2: old build 692/692 -> new 696/694 (+0.5%), no scene regresses beyond noise;
- shadow scene: 330 -> 335 FPS under the bench harness (single-scene runs swing much more; the 10-29
  handoff's "native 226" was a lower-clock measurement);
- full gltri gate: all 49 mode runs pass against zink; 0 MMU faults; full glmark2 has only the known
  DRI3 warning line.
Exported as patch 0326 (series 326, tree-checked), pushed with the mesa commit. Benchmark caveat: the
first official bench30 run of this session read native 674/683 (zink 685/688) while the alternating A/B
immediately after read 692-696 for both builds, so that bench30 pair was a low-clock/temperature outlier,
not a regression; re-run bench30 when the board is idle for the headline number.

## Round-58 (2026-10-29): patch 0327 - RGB565 sampling was black; convert instead; mip gap closed for it

Handoff open item 5 ("mipmapped textures in non-renderable formats keep level 0 only") turned out to sit
behind a sampler bug: a GL_RGB/UNSIGNED_SHORT_5_6_5 texture sampled **black** through the native driver
(zink returned the expected texels). The TPU decode of 16-bit packed colour (and R8/RG8) is not correct
on this hardware; the driver claimed support for B5G6R5, so the state tracker handed it over.

Fix (0327): refuse B5G6R5 for sampler bindings on top of the existing "formats the draw path can use"
rule, so RGB565 is converted to RGBX8/RGBA8 like RGB8 already was. The converted texture is a
renderable 8888 format and therefore gets the normal twiddled mip chain with GPU-generated levels -
which closes the mipmap-level-0-only gap for this case (a CPU-uploaded RGB565 mip chain now matches zink
exactly). R8/RG8 stay accepted but their decode is still unverified: a known limitation recorded in
`pvr_screen.c`.

Verified: plain RGB565 sampling and a CPU-uploaded RGB565 mip chain (256x256, 9 levels, minified) give
**identical sample checksums to zink**; full gltri gate 49/49 mode runs; full native glmark2 675 with only
the known DRI3 warning; 0 MMU faults. Exported as patch 0327, series 327 tree-checked, PR pushed.
WIP kept locally on branch `wip/linear-mips` (linear chains + pck-info meta) in case R8/RG8 get real
decode support later; not pushed.

## Round-59 (2026-10-29): native GL works under X11 through EGL; drawbench X11 parity with vendor

Two harness findings from the open items:

1. **Live-window deferred clears (handoff #6)**: windowed `glmark2-es2-wayland -b build --size 400x300`
   on headless weston runs through the window-system-buffer + deferred-clear path: native 1666 FPS vs zink
   1342, no errors, 0 faults. The off-screen-only caveat is removed for the Wayland window path (Xwayland
   still needs the EGL/X11 client below).

2. **Native GL under X11 works through EGL**: gltri gained `GLTRI_X11` (EGL_DEFAULT_DISPLAY + pbuffer) and
   loads the native driver under Xwayland (`GL_RENDERER: PowerVR B-Series BXM-4-64`, correct triangle).
   The earlier "native cannot run under X11" was a **GLX** limitation of the system glmark2 binary, not the
   driver: any EGL/X11 client gets the native path.
   - drawbench now has `DB_X11`; same binary/platform under X11: native **1252 / 1203 / 1451** FPS (median
     1252, Xwayland on the open stack) vs vendor **1300 / 1184 / 1271** FPS (median 1271, bare Xorg on
     pvrsrvkm) - **parity within noise** on the closest harness yet to glmark2's X11 conditions.
   - Consequence for the handoff: a glmark2-class native X11 number needs a glmark2 build that uses
     EGL/X11 (the distro binary is GLX), or the vendor run moved to an EGL/X11 harness as done here.

No driver change in this round; tools updated (`tools/verify/{gltri.c,drawbench.c}`).

## Round-60 (2026-10-29): the bench30 "native" numbers were zink - corrected glmark2-class measurement

Found while building a same-binary comparison: the distro `glmark2-es2` X11 binary is **GLX**, and the
native driver does not load through GLX, so `MESA_LOADER_DRIVER_OVERRIDE=pvr` silently fell back to zink.
`native-1.txt`/`native-2.txt` of the last bench30 runs indeed read
`GL_RENDERER: zink Vulkan 1.3(...)` - the "native 746/744" (and this session's 658/698) were **zink**, and
the per-scene "old vs new" A/Bs that used glbench.sh measured zink as well. Only the Wayland glmark2
(system `glmark2-es2-wayland`) and the EGL/surfaceless tests (gltri, drawbench with explicit
`GL_RENDERER` checks) were really native.

Fix: built glmark2 from source with `-Dflavors=x11-glesv2` (meson; libjpeg/libpng/zlib dev headers staged
from .debs because apt dependency resolution is broken). That binary uses EGL/X11, so it loads the native
driver under Xwayland, zink, or the vendor IMG GLES under bare Xorg - the same binary and platform for all
three stacks. Installed durably at `open-perf/glm2x11/` (22 MiB, `glm2x11/bin/glmark2-es2`), driver script
`open-perf/bench31.sh`.

Corrected three-way, same binary `-Dflavors=x11-glesv2`, perf governor, client on cores 6-7, two runs:

| stack | FPS scores | total FPS | GL_RENDERER |
|---|---|---|---|
| native (open stack, Xwayland) | **728 / 728** | 24068 | PowerVR B-Series BXM-4-64 (36.56.104.183) |
| zink (open Vulkan, Xwayland)   | **775 / 775** | 25620 | zink Vulkan 1.3 (IMAGINATION_OPEN_SOURCE_MESA) |
| vendor (pvrsrvkm, bare Xorg)   | **1199 / 1181** | 39607 | PowerVR B-Series BXM-4-64 (IMG GLES 3.2 24.2@6603887) |

So the honest state is: **native is behind zink by ~6% and behind the vendor GLES by ~1.6x on this
glmark2-class X11 harness**. The earlier claim of parity was harness-specific (EGL surfaceless
drawbench: native 1427 vs vendor 1290, where the vendor path was zink-on-vendor-Vulkan, not the IMG GLES
now measured). Per-scene, native's driver-specific deficits vs zink are terrain 0.70, refract 0.79,
ideas 0.85, desktop blur 0.87, loop/conditionals 0.87-0.89, jellyfish 0.88; native wins effect2d and is
level on shadow/build; the vendor gap is dominated by scenes where zink is also far ahead (desktop blur,
ideas, effect=shadow), i.e. stack-wide, not one driver bug.

Consequences: patches 0326/0327 correctness results stand (they were verified with gltri, which reports
the native renderer and its own checksums), but their performance deltas in the commit messages were
measured on the zink fallback and must be re-measured with bench31 or the Wayland binary before they are
quoted. bench30.sh is superseded by bench31.sh.

### Round-60 addendum: 0326/0327 re-measured on the corrected harness - neutral

A/B with the same-binary EGL/X11 glmark2, alternating rebuilds, two full-suite runs each, perf
governor, client pinned to cores 6-7:

- base `e846a194` (0325): **722 / 722**
- HEAD `c03a5859` (0326+0327): **723 / 726**

So on the real native harness both patches are **correctness changes with no measurable full-suite
performance effect** (the earlier "+0.5%" and shadow "+55%" deltas came from the zink fallback or a
different harness/clock state, not the native driver). They stay in the series for the correctness they
fix (depth-only colour traffic, RGB565 decode/mip chains); their commit messages predate the
methodology discovery.

## Round-61 (2026-10-29): native driver-specific deficits - terrain profiled, BO churn located

Terrain is native's worst driver-specific gap on the corrected X11 harness (native 19 vs zink 27 FPS,
0.70). Profiling with env-gated counters in `pvr_bo.c`/`pvr_resource.c` (temporary, reverted):

- terrain run: **3364 BO creates, 114 MB, only 94 destroys**; the histogram is dominated by **1509 eight-byte
  and 1507 forty-byte BOs**. refract (157) and ideas (135) are fine - this is terrain-specific churn.
- backtraces: those tiny BOs come from `pvr_vm_heap_alloc` in the path
  `st_generate_mipmap -> st_finalize_texture -> st_texture_image_copy -> pvr_resource_copy_region ->
  pvr_draw_blit -> pvr_draw_flush`, i.e. the app updates the terrain texture's level 0 each frame and the
  state tracker regenerates the mip chain through our GPU blit path, which allocates small VM-heap/data-set
  objects per blit instead of reusing them. (~37 tiny BOs per frame; ~10 mip levels per frame.)
- first hypothesis tested and rejected: `PVR_DRAW_HWRT_CACHE` 16 -> 64 did **not** help (terrain BO
  creates rose to 4998, FPS unchanged 18), so it is not HWRT data-set eviction; the allocation is on the
  `pvr_vm_heap_alloc` path itself.
- correlation: the same per-frame mip regeneration is likely why refract (native 35-37 vs zink 44-47) and
  terrain lag zink, both being the scenes with per-frame `st_generate_mipmap`.

Next: instrument `pvr_vm_heap_alloc` callers directly (sizes <= 64) and remove the per-blit allocation -
either cache the small heap chunks or avoid the flush/open dance per mip level. All instrumentation and the
cache change were reverted; tree back at the PR head.

## Round-62 (2026-10-29): terrain's per-frame mip cost is EOT/VM-heap churn in the blit path

Follow-up to round-61, done through the safe path (switch-safe to open first; the earlier reboot was
caused by starting weston/gltri while the vendor driver was bound, plus glmark2's data dir being under
/tmp when the reboot wiped it - the durable copy is at open-perf/glm2x11 and /tmp/glm2egl is now a
symlink to it, bench31.sh should create that symlink).

Instrumentation (temporary, reverted):
- heap allocations of the 8/40-byte BOs come from `pvr_vm_heap_alloc` inside `pvr_draw_flush` ->
  `draw_eot_program` -> `draw_upload_usc`/`draw_upload_pds`, i.e. **each EOT (end-of-tile) program
  upload allocates fresh VM-heap BOs and they are never freed**. Same path for PDS/load programs at
  2 dwords + 40 dwords per miss.
- EOT program cache hit rates: terrain **1720 calls / 1464 misses (85%)**, refract 781/15, shadow
  1436/4. So terrain's per-frame `st_generate_mipmap` blits hit a different target address on almost
  every render, rebuilding EOT USC+PDS programs and leaking a little VM heap each time; refract/shadow
  hit the cache.

That is the concrete terrain/refract cost zink does not pay (its mip blits are cheap Vulkan render
passes). Fixing it needs either (a) arena/cache reuse for USC/PDS program uploads instead of a fresh
VM-heap BO per miss, or (b) batching the mip-level blits so one render covers several levels. Both are
allocator/render-path changes larger than one session's safe scope; recorded as the next engine task.
No driver change in this round; tree back at PR head.

## Round-63 (2026-10-29): patch 0328 - program uploads from bump-allocated chunks (leak fix, not a perf win)

Follow-up to round-62's EOT finding. Every USC/PDS program upload (fragment, end-of-tile, load,
vertex PDS) allocated a fresh VM-heap BO and never freed it; terrain re-uploads an EOT program on most
mip blits, so a 2 s terrain run created ~3300 BOs and the VM heap grew without bound (the "heap is
full" abort is what a long enough terrain session would eventually hit).

0328 bump-allocates those bytes from one chunk per heap (256 KiB doubling, old chunk handle dropped
but left mapped so already-returned offsets stay valid). Measured: terrain ioctl count 24022 -> 16896
(-30%), terrain FPS 18 -> 19 (unchanged - the scene is GPU/firmware bound), refract/shadow unchanged
within noise. So this is a robustness/leak fix, not the missing terrain performance; the GPU-side
cost of the per-level mip blits remains the next target. Gate: full gltri 49/49, 0 MMU faults.
Exported as patch 0328, series 328 tree-checked, PR pushed.

## Round-64 (2026-10-29): terrain EOT misses are its animated mip chains, regenerated every frame

Logged the addresses behind the 85% EOT cache misses (round-62): the misses are exactly the
**mip-level addresses of four texture chains** (two RGBA8 + two RGBX8 512x512 chains, levels 0..9,
10 addresses each). So the app re-uploads its animated terrain textures every frame, the state tracker
regenerates the whole mip chain through `resource_copy_region`, and each level is a different render
target -> a different EOT program, one full render submit per level.

That makes terrain ~10-20 render submits per frame, each with a firmware TA/3D turnaround; zink does the
same levels as cheap Vulkan blits inside one submit. The fix is therefore submit-cost, not cache size:
either batch the multi-level mip generation into fewer submits (e.g. one submit with several fragment
jobs, or a dedicated mip-gen path that skips the per-level TA setup), or reduce the firmware turnaround
per blit. Engine task, not a one-liner; EOT cache/bucket changes cannot help because the addresses are
genuinely per-frame.
Instrumentation reverted; tree at PR head; plan is to prototype "N blits, one submit" on the terrain
path before touching the general draw path.

## Round-65 (2026-10-29): patch 0329 - blit renders skip the background load (refract +23%)

The terrain/refract cost from rounds 61-64 was partly self-inflicted: a blit renders a fullscreen quad
covering its whole target, yet the flush built the load background program (a full-target read of the
target's previous content plus its PDS load program) for pixels that are all overwritten. Blit
renders now use the clear background, like the depth-only path already did.

Measured with the same-binary EGL/X11 glmark2, perf governor, cores 6-7, two runs each:
- **refract 36/36 -> 44/45 FPS (+23%), now level with zink (47)**
- terrain 20/20 -> 20/21, shadow unchanged
- full suite **729 -> 745 (+2.2%)**, per-scene worst 0.99 (noise)
- full gltri gate 49/49, 0 MMU faults
Exported as patch 0329, series 329 tree-checked, PR pushed. This is the first native-vs-zink gap closed
by the corrected harness; the remaining terrain cost is the per-level mip-chain render submits
(round-64), next candidate for batching.

## Round-66 (2026-10-29): loop scene weakness is GPU 3D execution, not host overhead

The board reset (13:30 prev boot) had no Oops/panic/watchdog log: it was a hard reset while a GL harness
was started with the vendor driver bound (the known unsafe pattern from round-56's notes) - the
interrupted profiling command was the one running. No data or firmware impact; current boot clean.

Profiling the remaining native-vs-zink deficits with the corrected harness, the shader-heavy `loop`
scene: native 884 vs zink 1049 FPS, one render per frame, firmware trace medians per frame (ticks):
native 4457 (GPU 3D ~2352, gaps ~2105), zink 4021 (GPU 3D ~2009, gaps ~2000). Host gaps are comparable;
the ~17% extra **GPU 3D execution** is the difference - a code-generation/instruction-selection gap in
pco's GLSL->NIR path versus zink's SPIR-V->NIR path, not a draw-path bookkeeping cost. Chasing it means
compiler work on the fragment shader codegen for loop-heavy shaders; recorded, not attempted in this
session. No driver change.

## Round-67 (2026-10-29): native soak - four consecutive full runs, clean

Stability soak on the 0329 build (X11/EGL harness): 4 consecutive full glmark2 runs back to back,
737 / 724 / 723 / 709, **0 error lines and 0 MMU/kernel faults in every run**. The slow downward drift
(~4%) is sustained-load thermal history (no dmesg throttling events, ~60 C idle after restore, and
earlier sessions showed the same ambient dependence); no heap growth or crash pattern. The 0328 program
arenas and the round-56..65 fixes survive repeated full runs.

Board restored to vendor + desktop + original firmware after the soak.


## Round-68 (2026-10-29): 0329 was invalid - blits ARE load-program renders; reverted as 0330

Found while starting the ES3 work: twiddled rendering produced all-zero output on the current build
(`rttest` bad=262140/262144, `gltri` readback zero) while `PVR_NATIVE_DEBUG=no_twiddle` passed. Bisect:
0325, 0326, 0327 and 0328 all PASS; only **0329 fails** - and the mechanism is exact: in this driver a
blit (`pvr_draw_blit`) is implemented **as a load-program render with no geometry** (the background
program does the copy), so "blit renders skip the full-target background load" turned every blit into a
clear-only no-op. Mip generation, twiddled readback and texture uploads through blits silently stopped
copying.

The 0329 "+23% refract, +2.2% suite" figures were therefore **fake speed from broken rendering** (glmark2
does not validate pixels). 0329 is reverted by commit 0330 (exported as patch 0330), and the corrected
same-session numbers are native **680/671** vs zink **763/761** - i.e. native is ~11% behind zink, not
4.7%.

Post-revert verification: `rttest` PASS (262144/262144), `gltri` readback correct, full `tri.sh` gate
49/49, 0 MMU faults. Lesson recorded: every render-path patch needs a pixel-level readback check
(`rttest` + a blit/mip readback test), not just benchmark scores.

## Round-69 (2026-10-29): patch 0331 - R8/RG8 render targets unlock GL_EXT_texture_rg (first parity extension)

Phase 1 of GLES-PARITY-ROADMAP: Mesa gates the ES2 **GL_EXT_texture_rg** advertisement on R8/RG8 being
renderable (the extension-format table probes RENDER_TARGET|SAMPLER_VIEW), so the driver held the
extension back and every GL_R8/GL_RG8 upload failed with GL_INVALID_VALUE - which is why R8/RG8 sampling
"read black" in earlier tests. Allowing the two formats as colour targets needs no new code paths: the
PBE already writes them with the generic U8x4 packmode through the twiddled layout.

Verified against zink: GL_EXT_texture_rg now advertised; R8 and RG8 sampling **identical to zink**
(pixel 60 00 00 ff / 60 9f 00 ff, same checksum); R8/RG8 render-target clear+readback identical to zink
(new `open-perf/rt8test.c`: (255,0) and (255,128)); full gltri gate 49/49, rttest PASS, 0 MMU faults.
Known limitation kept: glReadPixels from an R8/RG8 FBO with a *different* read format needs Mesa's
format-converting blit, which this driver still refuses (read in the native format; blit conversion is a
separate roadmap item). Exported as patch 0331, series 331 tree-checked.

## Round-70 (2026-10-29): second vendor-bound reset; ES2 shader_texture_lod is a Mesa-core gap

Two findings, no driver change:
1. **Board reset cause**: the previous boot ended in a hard reset (watchdog, no Oops/panic in
   `journalctl -b -1`) because a GLES harness was started while the **vendor driver was bound** - the
   same unsafe pattern as 13:30, this time because a cycle left the board on vendor and the next test
   command did not switch. New guard: `tools/verify/gl-open.sh <cmd>` always runs `switch-safe open`
   first; use it for every EGL/GLES test from now on. Board recovered vendor+desktop, firmware intact.
2. **GL_EXT_shader_texture_lod cannot be enabled from the driver**: Mesa 26.3's extension table has only
   the desktop `ARB_shader_texture_lod` entry gated by `caps->fragment_shader_texture_lod`; there is no
   ES2 `EXT_shader_texture_lod` entry at all, so setting the cap changes nothing for GLES (the
   `texture2DLodEXT` test shader is rejected as an unsupported extension). Matching the vendor's ES2
   explicit-LOD extension therefore needs Mesa-core work, not pvr_native work - dropped from the cheap
   Phase-1 list (ponytail: no point shipping a cap that enables nothing).
Verification after revert: plain gltri + rttest PASS, full `tri.sh` gate 49/49, 0 MMU faults.

## Round-72 (2026-10-29): MRT v1 mostly working - three of four behaviours match zink; WIP saved

Implemented the MRT foundation from the design notes and verified each behaviour against zink with
`open-perf/mrt_test` (two RGBA8 attachments, `glDrawBuffersEXT`, `gl_FragData[0]=red [1]=blue`,
sampled back through a second pass):

- **Caps**: `max_render_targets = 4`, so st creates multi-attachment FBOs.
- **Fragment variants**: per-attachment `data.fs.output_formats[DATA0+i]` and explicit
  `data.fs.outputs[DATA0+i] = { .start = i, .count = dwords }` (the reference's
  `usc_mrt_resource.reg.output_reg` equivalent); blend options per attachment.
- **Flush**: PBE register words per attachment and one EOT program with one emit per attachment
  (`pvr_shader_eot_create(nr, state_words)` using `nir_wop_pco` between emits and `freep` on the last);
  `pvr_csb_pbe_state_words` sets `source_pos` from the attachment's output position (BIT0/32/64/96) and
  `mrt_index`, matching the reference's output-register mapping.
- **Clear**: a same-format MRT framebuffer takes the GPU clear path, the background program stores the
  packed colour into every attachment's pixel-output dword, and the clear is submitted immediately
  (deferred clears are single-resource only). CPU fills are skipped afterwards.
- **MRT is only used when the render really is the framebuffer's** (`d->rt == cbufs[0]`, level match):
  materialize/blit renders leave the framebuffer state pointing at another set and must stay
  single-attachment.

Verified: clear into both attachments matches zink ((64,128,191) each); a draw **without** a prior clear
writes red to attachment 0 and blue to attachment 1, matching zink. **Remaining bug**: a draw that
follows a GPU clear of the same MRT framebuffer updates attachment 0 but leaves attachment 1 at the
clear colour (not black from the background, not blue from the shader), i.e. the second render's emit 1
seems not to write. WIP saved on the board's local branch `wip/mrt` (not pushed); PR tree clean at
`b75dcca1`. Next: compare the two renders' submitted EOT/PDS data (the cached program is reused) and the
tile-buffer pixel data on the second render.

## Round-73 (2026-10-29): MRT complete - the missing attachment write was deleted by nir_lower_blend

The round-72 bug (attachment 1 stale after a draw following a GPU clear) was not in the EOT program,
the PBE state words or the stream. The decisive check was a byte comparison of the two drivers' EOT
programs: a temporary dump in pco/native and in the Vulkan driver's `pvr_usc_eot()` showed a 2-emit
EOT of 80 B in both, **byte-identical apart from the baked target addresses** (same instruction
sequence, same 2x2 state-word immediates), and the state words decoded to the same design
(`source_pos` = output dword, `mrt_index` = attachment). So the EOT side, including the second EMITPIX,
was already correct - confirmed separately by the GPU clear, which writes both attachments through the
same 2-emit program.

The write was being lost before the shader reached pco. `PCO_DEBUG_PRINT=fs,nir,passes` showed both
`store_deref`s in the incoming NIR, and a single `@store_output` to `FRAG_RESULT_DATA0` after
`pco_lower_nir` - attachment 1's store was gone, while its (still vec4) var declaration remained.
Root cause: `nir_lower_blend` deletes a colour store whose `colormask` is zero
("don't bother copying the destination for disabled RTs", `nir_lower_blend.c`), and Mesa's state
tracker only fills `pipe_blend_state::rt[]` entries the API configured - for ES2 + EXT_draw_buffers,
rt[1] stays zeroed even though `glColorMask` applies to every draw buffer. The variant therefore
lowered with `colormask = 0` for attachment 1 and `nir_lower_blend` removed its store.

Fix (one line, patch 0332): every attachment uses render target 0's blend state and colour mask, which
is what ES2 means - one blend state and one glColorMask for all draw buffers.

Verification (open stack, `open-perf/mrt_test` with 2x RGBA8, compared against zink):
- draw after clear: attachment0 255 0 0 255, attachment1 0 0 255 255 - zink same;
- clear covers both (64 128 191 255 each); second draw overwrites both (green/yellow);
- draw without clear writes both attachments (the round-72 stale-attachment-1 symptom is gone);
- full gltri gate: 49 modes, all 32 native modes glGetError 0x0; rttest PASS (bad=0/262144);
  rt8test unchanged (255 0 / 255 128); 0 MMU faults.
- Extension surface: `GL_EXT_draw_buffers` and `GL_NV_draw_buffers` now advertised (were absent with
  max_render_targets = 1).
Known v1 limitations: all attachments must share attachment 0's format (mixed sets render to
attachment 0 with a warning); per-render-target blend state is not tracked (ES2 has none); tile
buffers are unused. WIP branch `wip/mrt` squashed into patch 0332 on `pr12-native-ta-fix`.

## Round-74 (2026-10-29): four render targets - the pixel-output width was the gate

ES3 needs four colour attachments, and the cap was already 4, but 3- and 4-attachment framebuffers
rendered nothing (every attachment read back black, including after a clear). The cause was one
hardcoded value: the fragment static stream set `USC_PIXEL_OUTPUT_CTRL` to `0x00007ff8`, i.e. the
2REGISTERS width from the verified single-colour 512x512 capture, and the USC pixel-output path only
carries as many dwords as that field says. The reference derives it from the render
(`pvr_get_pixel_output_width`: total output registers, next power of two, clamped to the core minimum);
patch 0333 does the same: total colour dwords per pixel from the attachment formats, round up to a
power of two, encode 2/4/8REGISTERS in bits [0:1]. Two-attachment renders keep the same value as
before (minimum 2), so nothing regressed.

Verified with `open-perf/mrt_test` extended to N attachments (`MRT_ATT=4`): 4x RGBA8 with
`glDrawBuffersEXT(4)` gives attachment0 red, 1 blue, 2 green, 3 yellow - exactly the
`gl_FragData[]` values; clear covers all four; 2-target mode unchanged. Full gate green: 49 gltri
modes (all native `glGetError 0x0`), 28 mip modes, rttest PASS, rt8test unchanged, 0 MMU faults.
Patch 0333, series head `6b92b768`.

Deferred with evidence, not by assumption:
- **Mixed-format MRT** still falls back to attachment 0 (warning) - the GPU clear program now carries
  one packed colour per attachment, but the per-attachment write path is not validated yet. While
  testing it, a BGRA8 colour target cleared through the PBE and sampled back returned red/blue
  swapped (single-attachment too, so pre-existing): the BGRA8 render-target byte order is wrong
  somewhere between the PBE write and the TPU decode. That is the next format bug to fix; it also
  blocks using BGRA8 as the second mixed attachment.
- Random note for the next session: the board hard-reset at 20:54:47 with no Oops/BUG/panic in
  `journalctl -b -1` and `reboot-tracker` recorded `SOURCE=power-loss`; the last GPU activity was a
  pvrsrvkm probe + HDMI re-mode ~2.5 min earlier, the same hard-reset signature as the earlier
  vendor-bound GL resets. Keep using `switch-safe.sh`/`gl-open.sh` and end sessions on vendor+desktop.

## Round-75 (2026-10-29): mixed-format MRT, and the BGRA8 render-target swap explained

The BGRA8 target bug found in round-74 (clear via PBE, sampled back red/blue swapped) was a packing
mismatch between the two paths that write pixel data. pco packs shader colour outputs in *logical*
channel order and the PBE applies the target format's channel swizzle itself - visible in the
reference, where `to_pbe_format()` maps `B8G8R8A8_UNORM` to `R8G8B8A8_UNORM` before packing the
shader output. The background clear program instead packed the clear colour with
`util_format_pack_rgba(target format)`, i.e. in memory order, and then the PBE swizzled it again:
two reorders, so red and blue swapped. Fix: pack the background colour with the same format pco would
use (`draw_pbe_pack_format()`), and the BGRA8 target is byte-correct.

With that the mixed-format MRT work deferred in round-74 could be turned on: the flush no longer falls
back to attachment 0, the fragment variant is compiled for the actual attachment format set (each
output packed for its own format), and the GPU clear covers mixed formats because the background
program carries one packed colour per attachment. Verified with `mrt_test` (RGBA8 + BGRA8):
clear -> both attachments (64 128 191 255); draw -> attachment0 (128 0 0 255) and attachment1
(255 0 0 255), each packed for its own format; 2- and 4-attachment same-format modes unchanged.
Full gate green: 49 gltri modes (all native glGetError 0x0), 28 mip modes, rttest PASS
(bad=0/262144), rt8test unchanged (255 0 / 255 128), 0 MMU faults. Patch 0334, series head
`5027d7c4`; PR #12 pushed.

Remaining MRT limits (honest): every attachment must be one dword per pixel (PBE `source_pos` has only
four start positions; wider formats need a cumulative output-dword mapping), and per-render-target
blend state is untracked because ES2 has none. Next MRT-adjacent work is `GL_EXT_draw_buffers_indexed`
and then the ES3 core items (UBOs, 2D-array/3D textures, instancing).

## Round-76 (2026-10-29): instancing - plumbing was already there, one PDS path still wrong

MRT is closed; the next ES3 core item is instanced drawing. Reconnaissance showed the plumbing is
mostly written already: `pvr_csb_emit_index_list()` emits the instance-count packet
(`INDEX_LIST3`) and computes the stream length for it, the VDM state carries `vs_max_instances` from
`pvr_vdm_cam_size()`, and the shared PDS vertex-DMA descriptor has
`PVR_PDS_VERTEX_DMA_FLAGS_INSTANCE_RATE` + `divisor` (the Vulkan driver sets exactly those). What was
missing was only the wiring: reject `instance_count != 1` and `instance_divisor != 0` removed; the
instance count passed to the index list; per-instance attributes flagged/divided; and the two gallium
caps the ES2 extension table gates on (`vs_instanceid` for `EXT_draw_instanced`, and
`vertex_element_instance_divisor` for `EXT_instanced_arrays` - without the second,
`glVertexAttribDivisorEXT` is `GL_INVALID_OPERATION` even though the draw call is accepted).

Result of that wiring (open-perf/inst_test.c, 3 instances, per-instance offset vec2 + colour vec4,
divisor 1): one instance-rate attribute works - all three instances render at the right positions -
but with *two* instance-rate attributes the middle instance of a 3-instance draw reads past its
per-instance buffers (position falls back to 0, colour becomes transparent black), while 2 instances
are completely correct. So it is not the index list or the divisor flag; it is the second
instance-rate DMA's instance indexing inside the PDS program / VDM iteration, which the reference
exercises through the same shared PDS library.

WIP saved as `open-perf/instancing-wip-2026-10-29.patch` (64 lines, driver-side wiring only), Mesa
tree reverted to the shipped 0334 state so nothing broken is advertised. Next probe: dump the
generated PDS vertex program + const map (`pvr_pds_disasm.c` exists) for the 2-instance and
3-instance cases and compare the instance-rate DDMAD src1/const entries with what the reference
generates for a Vulkan instanced draw.

## Round-77 (2026-10-29): instancing works - the instance count is encoded count-minus-one

The round-76 "second instance-rate attribute misindexes the middle instance" turned out to be a
one-field bug plus a phantom draw. `INDEX_LIST3.instance_count` is encoded **count-minus-one** (the
reference writes `instance_count - 1`); passing the raw count made the hardware draw one instance too
many, and that extra instance read past the end of the per-instance buffers - with zeroes there
(robust fetch), it rendered a transparent-black triangle at the base offset, i.e. exactly the "broken
middle instance" of a 3-instance draw. A pixel-row scan comparing native against zink made it
obvious: at 2 instances the extra triangle was even visible as a third colour (the in-range third
element of the test's buffer).

Wiring in patch 0335 (the rest was already in the tree): accept `instance_count > 1`; pass the count
to the index list (count-1 encoded); put per-instance vertex elements in PDS instance-rate mode with
their divisor; set `caps->vs_instanceid` and `caps->vertex_element_instance_divisor`, the two caps
the ES2 extension table gates `GL_EXT_draw_instanced` and `GL_EXT_instanced_arrays` on (without the
latter, `glVertexAttribDivisorEXT` is `GL_INVALID_OPERATION` even though the draw is accepted).

Verified against zink with `open-perf/inst_test.c`: 2/3/4 instances (per-instance offset vec2 +
colour vec4, divisor 1) produce identical pixel-row scans, indexed instanced draws
(`glDrawElementsInstancedEXT`) identical too, no phantom instance. Both extensions now advertised.
Full gate green: 49 gltri modes (all native glGetError 0x0), 28 mip modes, rttest PASS
(bad=0/262144), rt8test unchanged, 2/4-attachment and mixed-format MRT unchanged, 0 MMU faults.
Patch 0335, series head `b9b44bb6`; PR #12 pushed. Known limits: no gl_InstanceID (ES3-only language
feature), max 16 instances for this vertex-output size (VDM cam size).

## Round-78 (2026-10-29): sRGB textures and render targets (ES2 EXT_sRGB), multi-dword colour outputs

ES2's `GL_EXT_sRGB` is the small, honest next step that also lays the ES3 `SRGB8_ALPHA8` foundation.
The TPU and PBE both have gamma modes; the two interesting parts were what to set and what happens on
the way out:

- **Sampling (`TEXSTATE_IMAGE_WORD0.gamma`)**: sRGB images get the gamma bit so the TPU decodes to
  linear. The first attempt decoded green/blue but left **red** sRGB-encoded (128 instead of 55 on a
  128 texel): the missing piece was `TEXSTATE_IMAGE_WORD1.alpha_msb`, which tells the TPU where the
  alpha byte sits - without it the first memory channel is treated as alpha and skipped by the sRGB
  decode. Setting it where the format's fourth channel is W (the reference's
  `vk_format_is_alpha_on_msb` rule) fixed it.
- **Rendering (`PBESTATE_REG_WORD0.gamma`)**: the PBE encodes linear source data on write. The PBE's
  source format for an sRGB bitmap is 16-bit float per channel (the same F16 conversion pco applies to
  shader outputs), so the background clear packs the clear colour as linear half floats, and an sRGB
  attachment **occupies two pixel-output dwords**.
- That last part needed the general **multi-dword colour output** plumbing (also what 16F/32F targets
  need): the fragment variant's output ranges, the background clear's per-dword stores, the EOT's
  PBE `source_pos` and the ISP pixel-output width are all built from a shared PBE-output-dwords count
  instead of assuming one dword per attachment.
- A deferred clear now keeps the **float** clear colour on the resource as well as the packed dword -
  the packed value is only right for one format, and replaying it for an sRGB target (16F source)
  gave a black clear until this was fixed.

Verified against zink with `open-perf/srgb_test.c`: sRGB texel bytes (128,64,32) decode to (55,13,4)
- identical; sRGB target cleared to linear 0.5 samples back 128, a 0.25 draw samples back 64 -
identical. `GL_EXT_sRGB` is advertised on ES2 now. Full gate green: 49 gltri modes (all native
glGetError 0x0), 28 mip modes, rttest PASS (bad=0/262144), rt8test unchanged, MRT 2/4/mixed unchanged,
instancing unchanged, 0 MMU faults. Patch 0336, series head `0db1b4fd`; PR #12 pushed.

## Round-79 (2026-10-29): three more ES2 extensions - derivatives, 10-bit packed colour, float textures

Each of these was one missing piece of driver state, verified pixel-identical to zink with
`open-perf/ext_texture_test.c`:

- **`GL_OES_standard_derivatives`**: pco already lowers `nir_intrinsic_ddx/ddy` for the fragment stage
  (the same lowering the Vulkan driver uses), so it only needed
  `caps->fragment_shader_derivatives`. A shader doing `dFdx(v.x)*64, dFdy(v.y)*64` over a 64-pixel
  quad renders (255,255,0,255) on both drivers.
- **`GL_EXT_texture_type_2_10_10_10_REV`**: `R10G10B10A2_UNORM`/`B10G10R10A2_UNORM` as sampler views,
  decoded by the TPU as `A2R10B10G10` (the reference's mapping; the pipe format's swizzle selects the
  channel order). Packed texel (1023,512,256,3) decodes to (255,128,64,255) - identical.
- **`GL_OES_texture_float`**: `R32G32B32A32_FLOAT` as a sampler view, decoded as
  `F32F32F32F32`. Texel (0.25,0.5,0.75,1.0) decodes to (64,128,191,255) - identical.

All three are advertised now. Full gate green: 49 gltri modes (all native glGetError 0x0), 28 mip
modes, rttest PASS (bad=0/262144), rt8test unchanged, sRGB/MRT/instancing unchanged, 0 MMU faults.
Patch 0337, series head `ba12e2e9`; PR #12 pushed. Next ES2 cheap items left: half-float linear
filtering, depth-texture-cube-map, stencil8; then the ES3 core work.

## Round-80 (2026-10-29): half-float linear filtering; and ES2 cube maps are a real, still-missing gap

- **`GL_OES_texture_half_float_linear` DONE (patch 0338)**: half-float textures were sampled with
  nearest only because the cap was never set. This core has `has_pbe_filterable_f16` (the feature the
  reference checks), so `caps->texture_half_float_linear` now follows it and the extension is
  advertised. Verified against zink with `open-perf/f16lin_test.c`: a 4x4 RGBA16F texture with
  columns 0,0,1,1 sampled with `GL_LINEAR` gives (135,135,135,255) on **both** drivers; `GL_NEAREST`
  gives 255 on both. 32-bit float filtering does not exist in this hardware, so
  `GL_OES_texture_float_linear` stays unadvertised (honest).
- **ES2 cube maps are not supported at all** - and that is a core-feature gap, not an extension one.
  `resource_create` refuses `PIPE_TEXTURE_CUBE` and `max_texture_cube_levels = 0`; a probe
  (`/tmp/cube_probe.c`) shows Mesa's `Bad t->Target in _mesa_test_texobj_completeness` +
  `GL_INVALID_VALUE`, while zink returns the six face colours correctly. Fixing this needs: cube
  resource layout (six twiddled faces, contiguous, mip chain per face), the texture descriptor's
  `TEXSTATE_TEXTYPE_CUBE` with `word1.depth = 5` (array layers minus one, the reference's rule) and
  the per-layer stride in the pco image meta (`PCO_IMAGE_META_LAYER_SIZE`), plus cube upload support.
  This is the next hundred-percent-real gap to close; `GL_OES_depth_texture_cube_map` builds on it.

## Round-81 (2026-10-29): cube maps - ES2 core feature restored

Cube maps were ES2 core and completely refused by this driver (round-80's probe): Mesa reported "Bad
t->Target in _mesa_test_texobj_completeness" and the texture stayed incomplete, while the hardware
supports cube maps natively. Patch 0339 adds them:

- **Layout**: six 2D twiddled faces, face-major, each face carrying its own mip chain down to 1x1 when
  mipmapped, faces strided by a page-aligned layer size. This is the reference's
  `pvr_image_plane_setup_mip_levels` layout (its `layer_size` includes the entire padded chain because
  the hardware derives face strides from it).
- **Descriptor**: `TEXTYPE_CUBE` + `IMAGE_WORD1.depth = faces - 1` (5), and the face stride in the pco
  image metadata's `PCO_IMAGE_META_LAYER_SIZE` word - which sits right after the 4 image state words in
  the combined image-sampler binding (the sampler state words then follow the 4 meta words).
- **Transfers / uploads**: the transfer box's z selects the face; the existing "blit through a linear
  staging resource" path now carries a layer for its render target and its background source (target
  address = layer * layer_size past the level, a face read back as a 2D image at that address).
  `glGenerateMipmap` walks each face's chain through the same layered blit.
- caps->max_texture_cube_levels now follows the maximum 2D texture size; cube formats are limited to
  the 8-bit RGBA/BGRA/RGBX/BGRX set the PBE can write.

Verified against zink (`open-perf/cube_probe.c`, `cube_mip.c` - both now in tools/verify):
- six distinctly coloured faces sampled with the six axis directions give exactly the face colours on
  both drivers;
- after `glGenerateMipmap`, sampling with `NEAREST_MIPMAP_NEAREST` at high minification keeps every
  face's colour on both drivers (the per-face mip chain and face stride are right).
Full gate green: 49 gltri modes (all native glGetError 0x0), 28 mip modes, rttest PASS
(bad=0/262144), rt8test unchanged, MRT/sRGB/instancing/extension tests unchanged, 0 MMU faults.
Patch 0339, series head `28c30f2c`.

Note for the next benchmark round: the attempted three-way glmark2 run was invalid because the reboot
had cleared `/tmp` and the glmark2 data symlink `/tmp/glm2egl` was missing (shaders not found,
glmark2 aborted); the symlink is restored and the benchmark is clean to re-run.

## Round-82 (2026-10-29): three-way benchmark after the feature rounds - native now beats zink

Same binary (glmark2 x11-glesv2, EGL/X11), performance governor, system.slice pinned to CPUs 0-5 and
the benchmark to 6-7, two runs each, board switched and restored by `bench31.sh`:

| stack | run 1 | run 2 | vs native |
|---|---|---|---|
| native (this driver) | **810** | **807** | 1.00 |
| zink (open Vulkan) | 776 | 774 | 0.96 |
| vendor IMG GLES | 1216 | 1199 | 1.50 |

Native now beats zink on summed per-scene FPS by 1.10x (9 of 10 scenes) and has moved from 0.57x to
0.71x of vendor since the MRT/instancing/sRGB/cube-map rounds. Per-scene native/vendor ratio and the
obvious next targets:

| scene | native | zink | vendor | n/v | n/z |
|---|---|---|---|---|---|
| build | 1279 | 1183 | 1553 | 0.82 | 1.08 |
| bump | 935 | 870 | 1460 | 0.64 | 1.08 |
| effect2d | 552 | 495 | 826 | 0.67 | 1.11 |
| ideas | 1076 | 878 | 1754 | 0.61 | 1.22 |
| jellyfish | 388 | 347 | 520 | 0.75 | 1.12 |
| refract | 37 | 47 | 76 | 0.49 | 0.79 |
| shading | 732 | 700 | 1267 | 0.58 | 1.05 |
| shadow | 390 | 366 | 686 | 0.57 | 1.07 |
| terrain | 20 | 27 | 39 | 0.51 | 0.74 |
| texture | 1462 | 1331 | 1528 | 0.96 | 1.10 |

The two scenes the native driver still loses to zink are **terrain** (mip/submit batching, the known
round-64 lead) and **refract** (cube map + derivative-heavy shading; cube maps only landed in patch
0339). Next perf steps: bisect the 0332-0339 batch to attribute the 680 -> 810 gain (likely the cube
map/sRGB/format work removing state-tracker conversions or fallback paths), then terrain submit
batching and a refract/shading codegen look. No driver patches this round - measurement only.

## Round-83 (2026-10-30): bisecting the 680 -> 810 gain - it is patch 0336, and specifically EXT_sRGB

The round-82 jump was reproducible under the same lightweight harness (switch-safe open, weston
headless + Xwayland, glmark2-es2 --off-screen, two runs), so it was bisected commit by commit:

| build | score |
|---|---|
| 0331 (pre-features) | 677 / 673 |
| 0335 (instancing) | 673 |
| 0336 with sRGB formats removed from `is_format_supported` | 670 / 675 / 676 |
| 0336 (sRGB + multi-dword outputs) | 809 |
| 0337 (derivatives/1010102/float) | 808 |
| 0339 (HEAD, cube maps) | 810 / 810 |

So the whole gain comes from patch 0336, and within it from **advertising the sRGB formats** (removing
just those three lines from the render-target format list drops 809 -> 676 while the rest of 0336
stays in). The full bench31 run at HEAD: native 810/807, zink 776/774, vendor 1216/1199.

Mechanism is not yet understood: the default framebuffer format is `R8G8B8A8_UNORM` in **both**
builds (temporary PVR_FBDUMP probe), so it is not a different surface format. The likely remaining
explanation is a state-tracker choice that changes when `GL_EXT_sRGB` is advertised (texture internal
format selection or a different draw path), to be pinned down next round by comparing the pipe texture
formats the driver receives (PVR_TEXDBG) on the sRGB and no-sRGB builds for the same scene.
Per-scene gain 0331 -> HEAD: ideas 1.56x, build 1.25x, jellyfish 1.24x, texture 1.21x, bump 1.17x,
shading 1.16x, shadow 1.10x, effect2d 1.09x, terrain 1.05x, refract 1.01x.

### Round-83 addendum (2026-10-30): the EXT_sRGB gain is not the GL extension string

A/B on one sRGB-enabled build with `MESA_EXTENSION_OVERRIDE` (three repeats each, `glmark2 -b build`):
- `GL_EXT_sRGB` present: 1309 / 1312 / 1263
- `GL_EXT_sRGB` removed from the extension string: 1320 / 1317

No difference. So the 809-vs-676 effect is **not** anything the state tracker or glmark2 does with the
GL extension. What the bisect edit actually changed is `pvr_is_format_supported(RENDER_TARGET)` for the
three sRGB formats, which Mesa EGL also consumes when it enumerates configs; the remaining candidates
are (a) the EGL config/visual glmark2 picks (same pipe framebuffer format and Z24X8 depth in both
builds per the FBDUMP probe, so if it differs it is in another config attribute) or (b) a Mesa
capability-driven path that the override cannot toggle. Next probe: enumerate the EGL configs both
builds offer and pinning one with glmark2's `--visual-config id=...`, plus `--results fps:cpu` to see
whether the CPU side or the GPU side gets cheaper. The +19% itself is real and reproducible and is
shipped as part of patch 0336 either way.

### Round-83 second addendum: in-session A/B confirms a GPU-side capability effect

Because separate-session comparisons could hide clock/thermal effects, the bisect edit was turned into a
runtime switch (`PVR_NO_SRGB=1` returning false for sRGB formats in `pvr_is_format_supported`, temporary,
not shipped) and A/B/A/B was run **in one weston session with one binary, interleaved** (`glmark2 -b
build --results fps:cpu`):

| run | fps | frame | user cpu | system |
|---|---|---|---|---|
| sRGB accepted | 1315 | 0.760 ms | 0.085 ms | 0.079 ms |
| sRGB refused | 1108 | 0.902 ms | 0.058 ms | 0.079 ms |
| sRGB accepted | 1337 | 0.748 ms | 0.067 ms | 0.079 ms |
| sRGB refused | 1115 | 0.896 ms | 0.058 ms | 0.078 ms |

So the ~18% is real, in-process, and on the **GPU side** (frame time drops while CPU time, if anything,
is *higher* when fast). Together with the earlier facts (GL extension string irrelevant via
MESA_EXTENSION_OVERRIDE; EGL config lists byte-identical, 60 configs on both builds; pipe framebuffer
and depth formats identical) this points at a Mesa internal choice made from driver *format capability*
rather than from any extension or EGL attribute. The mechanism is still not identified; next probes:
(1) print the pipe formats of every draw target/FBO the scene actually renders to (not only the first
default-framebuffer draw) and the depth/stencil formats, (2) `MESA_DEBUG=1`/state trace to see whether
the fragment shader variant or the number of GPU passes changes, (3) count submitted frag/geom jobs per
frame in both modes (PVR_FRAG_LOG-style counters). The +19% itself is shipped regardless (patch 0336).

### Round-83 third addendum: the EXT_sRGB gain lives in the swap/presentation path, not rendering

The per-flush trace showed identical work in both modes (same target `R8G8B8A8_UNORM`, same
`Z24X8` depth, one draw and one clear per frame, 800x600; only the number of frames differed). So the
frame-end mode was varied in the same A/B/A/B setup:

| frame-end | sRGB accepted | sRGB refused |
|---|---|---|
| default (swap) | 1308 | 1078 |
| `--frame-end=finish` (no swap) | 1113 | 1122 |
| `--frame-end=none` | 1323 | 1319 |

The 20% gap appears **only when eglSwapBuffers is part of the frame**; without the swap the two modes
are identical. Combined with the earlier facts (same GPU draw work, same pipe formats, same EGL config
list, GL extension string irrelevant), this says the *presentation/WSI* path Mesa picks differs when
the driver refuses sRGB render-target formats - most plausibly Mesa's X11/EGL surface path taking a
different (copying or differently synchronised) route. Next probe: `strace -c` the
swap/ioctl (drmSyncobj/DRI3) traffic in both modes, and print the EGL surface type/config glmark2 ends
up with. The +19% is shipped either way (patch 0336); this only matters for understanding which
presentation path is the fast one.

### Round-83 fourth addendum: WSI probe bounded - configs/visuals identical, mechanism parked

Two more probes, both negative, so the EXT_sRGB investigation is parked:
- **Syscall profile** (`strace -c -f` on `glmark2 -b build`, both modes): the ioctl mix per frame is
  the same (~6 ioctls/frame both modes); only the total counts differ with the frame count. No extra
  copying/readback syscall appears in the slow mode.
- **EGL config + native visual IDs**: extending the config enumerator with `EGL_NATIVE_VISUAL_ID` and
  the X visual depth shows the 60 configs and their visuals are **identical** in both modes too.

So the slow mode is not a different config, visual, surface format, draw stream or syscall mix; the
difference is inside Mesa's own presentation logic keyed off driver format capability (the frame-end
A/B proved it is in `eglSwapBuffers`). Parked as a Mesa-internals question rather than something to
keep spending rounds on; the +19% is shipped either way (patch 0336 includes accepting the sRGB
formats). The practical lesson: keep the sRGB render-target formats accepted.

Next work returns to feature gap closure: `GL_OES_depth_texture_cube_map` (cube maps landed in 0339 and
depth formats are already texturable), then `GL_EXT_draw_buffers_indexed`, then the ES3 core block.

## Round-84 (2026-10-30): terrain profiled - per-frame EOT recompiles were eating the CPU

Terrain is the weakest scene against both zink (0.74x) and vendor (0.51x), so it got profiled with
temporary counters/timers in `pvr_draw_flush` (`--results fps:cpu`, `-b terrain`):

- native: 19 fps, frame **55.2 ms**, user CPU **12.1 ms**, system 5.6 ms
- zink: 27 fps, frame 38.3 ms, user CPU 3.5 ms, system 1.8 ms

Per-flush timing showed the flush costing ~266 us with **~198 us in `draw_eot_program`**, and the
reason: terrain renders about 27 distinct target sets per frame (mip levels + bloom passes), the EOT
program cache held 32 entries and missed **83%** of lookups, and each miss recompiles the end-of-tile
USC program and uploads it (measured ~200 us).

Fix: **512 EOT cache entries** (patch 0340). Terrain steady state: hits 1464/9415 -> 9802/9885,
per-flush EOT ~200 us -> ~1 us, **user CPU 12.1 ms -> 1.9 ms per frame** (system 3.2 ms). Terrain
itself only moves 18 -> 19-20 fps because it is now GPU-bound (frame 51.6 ms with ~2 ms CPU), and the
full glmark2 score is unchanged (809) - this is a CPU-overhead fix for multi-target workloads.

Remaining terrain gap is GPU-side: native ~46 ms of GPU time per frame vs zink ~34 ms, pointing at pco
codegen/fragment work rather than driver submission. Next perf steps: pco codegen on the terrain
fragment shader, then the submit-path batching idea (per-flush submit ioctl is ~60-90 us).
Full gate green after the change (49 gltri modes, mip 28, rttest PASS, rt8test, MRT/sRGB/instancing/
extensions/cube tests unchanged, 0 MMU faults). Patch 0340, series head `1e8c39aa`.

## Round-85 (2026-10-30): CPU is no longer the native driver's problem - the remaining gap is GPU-side

Per-scene CPU accounting with `--results fps:cpu`, same binary/session, default frame-end
(so swap is included for both), native vs zink:

| scene | native fps | user+sys ms | zink fps | user+sys ms | n/z | native CPU gap |
|---|---|---|---|---|---|---|
| build | 1282 | 0.28 | 1185 | 0.54 | 1.08 | -0.25 |
| bump | 938 | 0.24 | 869 | 0.53 | 1.08 | -0.29 |
| effect2d | 553 | 0.23 | 492 | 0.48 | 1.12 | -0.26 |
| ideas | 1066 | 0.52 | 606 | 1.67 | 1.76 | -1.16 |
| jellyfish | 388 | 0.30 | 351 | 0.76 | 1.11 | -0.46 |
| refract | 37 | 1.16 | 46 | 1.92 | 0.80 | -0.76 |
| shading | 734 | 0.22 | 705 | 0.46 | 1.04 | -0.24 |
| shadow | 387 | 0.38 | 359 | 1.03 | 1.08 | -0.65 |
| terrain | 20 | 4.55 | 27 | 4.84 | 0.74 | -0.29 |
| texture | 1472 | 0.14 | 1344 | 0.43 | 1.10 | -0.29 |

The native driver is now **cheaper on the CPU than zink in every scene** (the EOT cache fix of round-84
removed the last big per-render compile). Estimating GPU time as frame - cpu: build 0.50 ms native vs
0.30 ms zink, terrain 45.5 vs 32.2 ms, refract 25.8 vs 19.8 ms - so every remaining deficit is GPU
execution, i.e. pco codegen / draw-pipeline work, not driver overhead.

Next step is therefore a shader-codegen comparison: dump the native vertex/fragment ISA for a scene
(`PCO_DEBUG_PRINT=vs,fs,binary`) and the Vulkan driver's ISA for the equivalent zink shader (same pco
compiler, different front-ends), and diff instruction counts/sequences for the biggest GPU gaps
(terrain and refract first). No driver change this round - measurement only.

## Round-86 (2026-10-30): ISP tile configuration derived from features (matches the capture)

Round-85 established that the remaining native gap is GPU execution, so the fragment stream's ISP
configuration was audited. `USC_PIXEL_OUTPUT_CTRL` and `ISP_CTL` were hardcoded to the values of the
verified 512x512 single-colour capture (`0x00007ff8` / `0x80035000`): correct for that one case, but
the ISP pipe-enable count and partition mask stayed single-target values even for 4-attachment MRT.

Ported the reference's `pvr_arch_setup_tiles_in_flight()` (patch 0341): partition size from pixel
width and tile size, partitions available from the kernel's partition-space runtime info, phantom and
paired-tile rules, then the pixel-output width/partition-mask and ISP pipe_enable fields; the other
ISP_CTL bits are set the way the reference's fragment stream sets them (sample positioning, empty
tiles via the background object, skip_init_hdrs where the multicore/simple-parameter combination
allows). **The computed pair for the common one-RGBA8-target case is byte-identical to the old
hardcoded values** (`0x00007ff8` / `0x80035000`), which validates the port; wider pixel outputs (sRGB
2-dword targets, 4-attachment MRT) now get the matching smaller partition counts instead.
No performance change is claimed - the values are identical for the benchmarked scenes - but the
configuration no longer depends on one capture.

Full gate green (gltri 49 modes, mip 28, rttest PASS, rt8test, MRT 2/4, instancing, sRGB, cube,
extensions, 0 MMU faults). Patch 0341, series head `2768cfe0`.

## Round-87 (2026-10-30): shader ISA comparison - terrain codegen is at parity, the gap is elsewhere

Dumped both drivers' ISA for the terrain scene (`PCO_DEBUG_PRINT=vs,fs,binary`; the Vulkan driver
compiles its SPIR-V with the same pco compiler, so the listings are comparable):

| shader | native | zink (Vulkan driver) |
|---|---|---|
| terrain heavy fragment shader | 291 instructions (739 IR ops) | 286 instructions (694 IR ops) |
| terrain vertex shader (heavy variant) | 50 (88 IR) | 200 (671 IR) |
| simple vertex shader | 9-10 | 31 |
| light/simple fragment shaders | 5-20 | 12-40 |

So for the heavy terrain fragment shader the two code generators are **within ~2% of each other**; the
Vulkan driver's vertex shaders are actually *larger*. Shader instruction count therefore does not
explain the terrain GPU gap (~45 ms native vs ~32 ms zink of GPU time). Current best hypotheses, to be
tested with a controlled fragment-bound microbenchmark (drawbench) rather than more scene profiling:
(1) texture sampling configuration for mipmapped/filtered reads (LOD, filtering, twiddled cache
behaviour), (2) the TA/IPF pipeline configuration for many small draws/passes (tile counts, phantoms),
(3) zink's render-pass behaviour (load/store choices) differing from this driver's clear/load model.
The ISA comparison tooling and logs are kept (`/tmp/isa-dump/`), no driver change this round.

## Round-88 (2026-10-30): the GPU gap is per-render-pass overhead, not fill/texture/vertex throughput

Built `open-perf/drawbench2.c` (now tools/verify/drawbench2.c): fixed-work surfaceless-EGL
microbenchmarks that run identically on native and zink. Per-mode fps (120 frames, glFinish/frame):

| mode | native | zink | native/zink |
|---|---|---|---|
| fill (1 fullscreen quad) | 2181 | 1314 | **1.66** |
| overdraw | 2457 | 1607 | 1.53 |
| tex nearest | 1493 | 967 | 1.54 |
| tex linear | 1473 | 926 | 1.59 |
| mipmapped nearest | 1669 | 950 | 1.76 |
| mipmapped linear | 1605 | 938 | 1.71 |
| cube fetch | 1665 | 979 | 1.70 |
| vertex 60k | 1140 | 973 | 1.17 |
| **27 small (64x64) FBO passes + final draw per frame** | **150** | **709** | **0.21** |
| 27 draws, one pass | 521 | 714 | 0.73 |

So the native driver is **faster than zink at every per-primitive GPU operation** (fill, texture
fetch nearest/linear, mip sampling, cube fetch, vertex throughput) but **4.9x slower when the frame is
split across many small render passes**. With CPU accounting from the same tool (passes mode: wall
801 ms vs cpu 275 ms over 120 frames) the per-pass gap is GPU-side: ~4.4 ms/frame of GPU time for 27
tiny passes, i.e. ~0.16 ms per pass of pure pipeline overhead.

That is the real explanation for the glmark2 scenes that still lose to zink: terrain and refract render
about 27 passes per frame (mip levels/bloom), so they pay that overhead 27 times. Next investigation:
what the per-pass GPU overhead is (job dispatch/barriers vs our flush's stream/sync sequence) - measure
passes=1 vs 27 on both drivers, then look at batching multiple renders into fewer submits / barriers
(as the reference does within a command buffer) instead of one submit-per-pass with syncobj waits.

### Round-88 addendum: the per-render cost is a fixed ~0.16 ms, independent of target size

Follow-up measurements with drawbench2 (native, 27 passes/frame unless noted):

| variant | fps | per-frame |
|---|---|---|
| 27 small passes, no clear (draws batch into one render) | 1379 | 0.73 ms |
| 27 clear+draw passes (one render each) | 148 | 6.8 ms |
| 27 passes to **two alternating targets**, no clear (forces one render each) | 150 | 6.7 ms |
| clear-only (clears deferred, no draw) | 2296 | - |
| clear+draw passes at 64 / 256 / 512 px | 149 / 157 / 130 | - |

So the cost is a **fixed ~0.16 ms per render submission**, not tile/fill work (it does not scale with the
target size) and not the clear itself (alternating targets without clears costs the same). zink does
the same 27 passes at 754 fps, i.e. ~0.05 ms per pass, because the Vulkan driver can put all sub_cmds
of a frame into one queue submission while this driver submits each render separately.

Inspection of `pvr_job_submit_render`: every render is **one ioctl with three jobs** (geometry,
partial-render fragment, fragment - the reference's scheme) plus **three `drmSyncobjCreate` calls**
and a later wait/destroy, i.e. a full CPU-kernel-firmware round trip per render with the GPU going
idle in between. That is consistent with the measured fixed cost. The next substantial performance
item is submission batching: queue multiple renders' job sets and submit them in one ioctl (the kernel
API already takes a job array), with dependencies expressed as syncobj waits the kernel can order, as
the reference does inside a command buffer. Scoped as its own multi-step change (hazard tracking for
deferred submissions), not a quick patch.

### Round-88 second addendum: what the three jobs do, and a dead end worth recording

Tried removing the partial-render (PR) job from `pvr_job_submit_render` (the 3-job scheme is
geometry + PR-fragment + fragment). Result: 6.7x apparent speedup but **all rendering disappears**
(gltri reads back black, MRT black) - dropping the PR job skips the actual tile processing, so that
number was measured on skipped work, not a speedup. Putting an empty command stream in the PR job
gives the same black frames. So in this driver's scheme the PR job is the tile-render phase and the
final fragment job does the pixel merge; both are needed.

Reference cross-check (`pvr_arch_cmd_buffer.c`, `pvr_arch_queue.c`, `pvr_drm_job_render.c`): Vulkan
also submits all three jobs per render, with the same PR/fragment split, and its PR job is a copy of
the fragment stream with PR PBE/EOT words patched in. The difference is in the *submission plumbing*:
Vulkan uses a **persistent `geom_to_pr_syncobj`** per context for the geometry->PR dependency, while
this driver creates **three fresh syncobjs per render** (`drmSyncobjCreate` x3) and the kernel has to
signal/wait them every kick; Vulkan also batches all of a frame's render jobs into one
`DRM_IOCTL_PVR_SUBMIT_JOBS` array while this driver does one ioctl per render. Both are candidates for
the fixed ~0.16 ms per-render cost measured above; changing the syncobj scheme to one persistent
geometry->PR syncobj (as the reference) is the smaller, safer first step, then submission batching.

### Round-89 addendum: persistent geom->PR syncobj is neutral; the streams themselves are the next lead

Implemented the reference's persistent geometry->PR syncobj scheme (one syncobj per context instead of
three fresh `drmSyncobjCreate` per render). Measured with drawbench2, same build correctness first
(gltri red count 32768 as expected, MRT attachments correct, 0 faults):

| | old scheme (3 fresh syncobjs) | persistent geom->PR (1 fresh) |
|---|---|---|
| fill | 2181 fps | 2372 fps |
| 27 clear+draw passes | 148 fps | 150 fps |

The per-render fixed cost is unchanged, so syncobj object creation/signalling is not the penalty; the
change was reverted rather than shipped as dead weight (it only mirrors the reference plumbing).

Next lead, from reading the reference geometry stream construction: the reference's geometry stream is
`KMD_STREAM_HDR, CR_VDM_CTRL_STREAM_BASE, CR_TPU_BORDER_COLOUR_TABLE_VDM, CR_PPP_CTRL, CR_TE_PSG,
VDMCTRL_PDS_STATE0, KMD_STREAM_VIEW_IDX`, while this driver's captured geometry stream is
`KMD_STREAM_HDR, CR_VDM_CTRL_STREAM_BASE, CR_TE_PSG, CR_ISP_SCISSOR_BASE, CR_ISP_DBIAS_BASE,
CR_ISP_OCLQRY_BASE, CR_ISP_ZLSCTL, CR_ISP_ZLOAD_BASE, CR_ISP_STENCIL_LOAD_BASE` - i.e. several
ISP-side commands sit in the geometry stream here where the reference has CR_PPP_CTRL,
VDMCTRL_PDS_STATE0 (USC common size), the border colour table and the view index. Whether that
placement (and the missing PPP/TPU/PDS state) is what makes each kick cost more than the reference's is
the next thing to check: compare the two drivers' streams command by command for the same 64x64 render
and try moving the ISP state to the fragment stream / adding the missing geometry state.

### Round-91 correction: the geometry stream is *not* mis-placed - it already matches the reference

The previous addendum's "stream placement" lead was wrong: the `CR_ISP_SCISSOR_BASE / DBIAS /
OCLQRY / ZLSCTL / ZLOAD / STENCIL_LOAD` constants belong to the **fragment** stream builder
(`pvr_frag_static_stream`), while `pvr_geom_static_stream` already emits exactly the reference's
sequence (`KMD_STREAM_HDR, CR_VDM_CTRL_STREAM_BASE, CR_TPU_BORDER_COLOUR_TABLE_VDM, CR_PPP_CTRL,
CR_TE_PSG, VDMCTRL_PDS_STATE0, KMD_STREAM_VIEW_IDX`, plus the BRN49927 ext stream). It was ported
from the reference in an earlier round and verified field by field. So stream content/placement is not
the remaining difference.

Combined with everything measured so far, the fixed ~0.16 ms per render is best explained by the
**one-`SUBMIT_JOBS`-ioctl-per-render submission model**: each render is a kernel kick round trip, and
the GPU goes idle between kicks; the Vulkan driver puts a whole frame's render jobs into one
submission and does not pay the round trip per pass (27 passes at 754 fps vs 148 fps here). The fix is
submission batching (queue several renders' job sets, submit together with the kernel's job array),
which is a multi-step change to the deferred-submission/hazard-tracking path - scoped for its own
round, not a drive-by patch.

## Round-93 (2026-10-30): three-way benchmark refreshed after 0340/0341

First valid run after the 03:08 reboot (the first attempt was invalid - `/tmp` was cleared, so the
glmark2 data symlink `/tmp/glm2egl` was missing and every run failed to load shaders; recreated and
re-run). Same binary, same setup as round-82 (perf governor, CPUs 0-5 system / 6-7 bench, two runs):

| stack | run 1 | run 2 |
|---|---|---|
| native (this driver) | 805 | 806 |
| zink (open Vulkan) | 775 | 772 |
| vendor IMG GLES | 1218 | 1203 |

Within run-to-run noise of round-82 (810/807, 776/774, 1216/1199): the EOT-cache fix (0340) and the
ISP-config derivation (0341) are CPU/correctness changes and do not move the aggregate glmark2 score,
as stated in their commit messages. Native is at 0.67x vendor and 1.04x zink on this aggregate.

## Round-94 (2026-10-30): submission batching implemented and measured - neutral, hypothesis closed

Implemented render-job batching (pending job sets from several flushes submitted in one
`DRM_IOCTL_PVR_SUBMIT_JOBS` array, with early flushes whenever a completion sync is needed or an rt
data slot would be reused) - the change the previous rounds pointed at. Correctness held (gltri red
32768, MRT, sRGB), but perf was **unchanged**: fill 2306 fps vs 2181 baseline, 27 clear+draw passes
150 vs 148 fps.

So the fixed ~0.15 ms per render is **not** the CPU/kernel round trip: queuing the jobs together and
submitting one ioctl does not remove it. It is GPU/firmware-side per-kick cost (two kicks per render:
geometry+PR and fragment), which the reference pays too; zink's advantage in the many-small-pass
pattern therefore comes from something else in its command-buffer/pass handling, not from submission
plumbing. Change reverted (neutral complexity is debt). Together with the persistent-syncobj and
PR-flag experiments, this closes the submission-plumbing line of investigation.

## Round-95 (2026-10-30): twiddle-layout reverse engineering started (blocked on a clean dump)

Every remaining ES2-visible feature gap on this driver needs the same missing piece: a CPU-side
twiddled-layout writer. `GL_OES_depth_texture_cube_map` and compressed textures (ETC1/ETC2) both are
sampled/rendered from twiddled memory that the driver currently only fills through a GPU blit (colour
uploads) or cannot fill at all (depth data). The reference never needs CPU twiddling because its images
are linear, so there is no code to port: the layout has to be derived from hardware.

Started that derivation: temporary `PVR_DUMP_TWIDDLE` hook dumps a 64x64 twiddled RGBA8 render
target's raw BO after the upload blit (waits all slot renders first), and a probe uploads a
permutation-invariant per-pixel pattern (each pixel's bytes carry its index). Result: sampling of the
texture is correct, but the raw dump is **not a simple 32-bit-pixel permutation** - it is mostly zeros
with 16-bit-interleaved-looking words and alternating garbage, so the dump is either not the complete
surface, not coherent, or the layout splits each pixel into 16-bit halves across the image. The dump
hook needs one more iteration (start from an 8x8 single-channel tile, decode with the driver's own
readback path to cross-check, and check whether the mapping BO is CPU-coherent) before a layout formula
can be fitted. Parked here rather than guessing: a wrong twiddle writer corrupts every upload and is
must-have-correct code. Next session: finish the 8x8 derivation, then implement `pvr_twiddle_copy()`
and use it for depth-cube and ETC1 uploads.

### Round-97: /tmp cleanup broke harness runs again - data path made explicit; glmark2 --validate assessed

The board's `/tmp` is cleaned periodically, which deleted the `/tmp/glm2egl` shader-data symlink and
silently produced invalid benchmark/validation runs twice (round-82 first attempt, round-93 first
attempt, and a validation attempt today). `bench31.sh` and a new `tools/verify/validate.sh` now pass
`--data-path /home/radxa/open-perf/glm2x11/share/glmark2` explicitly instead of relying on the
compiled-in `/tmp` path - no symlink needed.

`glmark2-es2 --validate` was then evaluated as a correctness gate: zink passes a small subset
(e.g. `build use-vbo=true`, `texture texture-filter=linear`) while the native driver passes none of
the 27 checks, but the validator is strict about exact pixels and fails most checks on zink too, so it
is **not yet a usable gate** in this offscreen setup. Two options next: (a) use it differentially -
for the scenes zink passes but native fails, do a direct native-vs-zink image diff of the same
offscreen render; (b) rely on the existing targeted gates (gltri/mip/rttest/MRT/cube/sRGB/...) and add
per-feature image comparisons. The differential scene list is the candidate-bug list to start from.

## Round-98 (2026-10-30): hardened switch-safe verified, bench harness fixed, numbers steady

After the second vendor-module hard reset (04:15:10, same `psConnectionPriv NULL` race, this time on
the restore-to-vendor path while display-manager started), `switch-safe.sh` now waits for the vendor
module's "Initialized pvr" and a 3 s settle before starting display-manager. A full open -> vendor
cycle plus the benchmark's own two swap cycles ran clean (0 faults, no PVR_K errors).

`bench31.sh` now passes the glmark2 data path explicitly (the `/tmp` symlink had silently invalidated
three runs), and the resulting three-way numbers are steady:

| stack | run 1 | run 2 |
|---|---|---|
| native | 807 | 805 |
| zink | 775 | 770 |
| vendor | 1220 | 1190 |

Native ~0.67x vendor, ~1.04x zink - in noise of rounds 82/93. The remaining work is feature parity
(ES3 core; twiddle-layout RE parked) and the per-scene GPU gap, which the earlier rounds showed is not
submission plumbing.

## Round-99 (2026-10-30): extension list audited - compressed extensions are honest via Mesa fallback

The native ES2 extension list (70) advertises `GL_OES_compressed_ETC1_RGB8_texture`, the S3TC/DXT
family, ASTC LDR and the ANGLE DXT aliases even though this driver supports no compressed formats.
Not dishonest advertising: Mesa's `st_extensions.c` gates those extensions on a format list with a
fallback (e.g. ETC1's `{PIPE_FORMAT_ETC1_RGB8, PIPE_FORMAT_R8G8B8A8_UNORM}, GL_TRUE`, "at least one
format must be supported"), so uploads arrive as RGBA8 and Mesa decompresses/converts on the CPU. The
driver's `is_format_supported` rejects non-PLAIN layouts explicitly, so nothing compressed reaches it.

Remaining ES2-era extensions that would need real hardware work (not just caps), for the record:
`EXT_occlusion_query_boolean` (OCLQRY program + per-query buffers), `EXT_multisampled_render_to_texture`
(MSAA tile paths), `EXT_disjoint_timer_query` (GPU timestamps), `EXT_clip_control`/`EXT_depth_clamp`
(TA/PPP clip state, undocumented for this core), `EXT_texture_mirror_clamp_to_edge` (no TPU wrap mode),
and depth-cube/compressed *unchanged* (twiddled upload, parked). So the ES2-visible surface is now
either done, hardware-blocked, or Mesa-fallback-honest; the next real increment is the ES3 core block.

## Round-100 (2026-10-30): the twiddle unlock path exists - transfer jobs write twiddled surfaces

Follow-up to the parked twiddle RE: the reference does not CPU-twiddle because its **transfer-frag
jobs** handle memory layouts on the GPU. `pvr_arch_job_transfer.c` resolves each surface through
`pvr_mem_layout_spec()` and explicitly supports `PVR_MEMLAYOUT_TWIDDLED` / `PVR_MEMLAYOUT_3DTWIDDLED`
destinations (and depth/stencil merges: `PVR_TRANSFER_PBE_PIXEL_SRC_DMRG_*`/`SMRG_*`), which is how
`vkCmdCopyBufferToImage`/`vkCmdCopyImage` fill twiddled Vulkan images from linear sources. This
driver's `pvr_probe tq_fill_check` already replays a captured transfer-frag fill stream against a
target address, so the job type, sync path (`pvr_job_submit_transfer_sync`) and PDS/USC program
installation are proven - what is missing is a *generic* transfer stream builder (source surface
descriptor, destination PBE/tiling words, format conversion) instead of the captured single fill.

So the route to the remaining features is porting a minimal transfer copy (linear buffer -> twiddled
image, and twiddled depth -> twiddled depth for depth-cube uploads) rather than deriving twiddle math.
Scoped next steps: (1) build a transfer stream for simple RGBA8 buffer->twiddled-image copy and verify
against the existing GPU-blit upload path; (2) extend to depth formats via the DMRG/SMRG sources;
(3) then compressed formats. This is the one architectural piece that unlocks depth-cube, compressed,
3D/array and depth-texture work at once.

## Round-101 (2026-10-30): honesty pass - two unimplemented extensions withdrawn

Auditing the 70-entry native ES2 extension list against what the driver implements turned up two that
Mesa advertised because its cap defaults are optimistic, not because the driver supports them:

- `GL_EXT_frag_depth` came from `caps.fragment_shader_depth`, which
  `u_init_pipe_screen_caps()` defaults to true. The fragment variant builder has no depth output, so
  `gl_FragDepthEXT` would be silently ignored.
- `GL_EXT_shadow_samplers` came from `caps.texture_shadow_map` (also default true). The sampler
  descriptor has no compare mode, so `sampler2DShadow`/comparison sampling would silently return the
  raw depth value.

Both caps are now cleared (patch 0342): the extension list drops to 68 and matches what the driver can
back. `GL_EXT_texture_border_clamp`/`GL_OES_texture_border_clamp` cannot be withdrawn this way - Mesa
marks them `dummy_true` for ES2 - so they remain advertised and are recorded as a documented
limitation; implementing sampler border colour + `CLAMP_TO_BORDER` (TPU border table + wrap mode) is
scoped as its own item. Gate green after the change (gltri 49, mip 28, rttest PASS, rt8test,
instancing, extensions, half-float linear, 0 faults). Patch 0342, series head `574ba361`.

## Round-102 (2026-10-30): border-clamp sampling implemented (last unbacked ES2 extension)

`GL_EXT_texture_border_clamp`/`GL_OES_texture_border_clamp` are advertised for ES2 by Mesa's extension
table unconditionally (`dummy_true`), but the driver only mapped the wrap mode: the border colour table
was a 4 KB zeroed BO and every sampler used `bordercolor_index = 0`, i.e. the built-in transparent
black. Any app clamping to border sampled black instead of its border colour.

Patch 0343 implements it the reference way (`pvr_arch_border.c`):
- `pvr_sampler_desc.bordercolor_index` emitted into TEXSTATE_SAMPLER_WORD0;
- the table BO is 21 entries x 4096 B (the reference layout: 128 16-byte values indexed by
  ROGUE_TEXSTATE_FORMAT, then 128 compressed values, unused here);
- hardware built-ins 0..4 untouched, custom colours cached from index 5 up (16 entries), written once
  and packed per format with `util_format_pack_rgba` (with width 1 - the missing argument that first
  left the table black);
- flush already passes the table address to both VDM and PDM TPU words.

Verified with `tools/verify/border_probe.c`: 8x8 red texture, CLAMP_TO_BORDER, green border -> centre
red, outside (0,255,0,255), identical to zink (was (0,0,0,0)). Full gate green (gltri 49, mip 28,
rttest PASS, MRT 2/4, instancing, sRGB, 0 faults). Patch 0343, series head `d99908bc`.

With this, every ES2-visible extension the list advertises is backed by working behaviour; the
remaining work is hardware-blocked (occlusion/MSAA/timer/clip) or ES3 (twiddle/transfer jobs).

## Round-103 (2026-10-30): GL_EXT_texture_mirror_clamp_to_edge (TPU FLIP_ONCE_THEN_CLAMP)

One more ES2 extension was sitting behind an unset cap: the TPU's address mode
`FLIP_ONCE_THEN_CLAMP` is precisely GL_MIRROR_CLAMP_TO_EDGE semantics, and the sampler state already
mapped `PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE` to it, but `caps.texture_mirror_clamp_to_edge` was never
set so the state tracker converted to CLAMP_TO_EDGE. Setting the cap (patch 0344) grows the ES2
extension list 68 -> 69.

Verified with `tools/verify/mirror_probe.c`: 4x1 texture sampled at u = -0.4 returns
(200,0,0,255) - mirrored back into the first half - exactly like zink (clamping would give
(64,0,0,255)). Full gate green (gltri 49, mip 28, rttest PASS, MRT 2/4, instancing, sRGB,
border-clamp probe, 0 faults). Patch 0344, series head `14087ce4`.

## Round-104 (2026-10-30): anisotropic filtering (GL_EXT_texture_filter_anisotropic)

The TPU sampler word carries anisoctl x2/x4/x8/x16 but the driver hardcoded DISABLED and never
advertised the extension. Patch 0345 enables it: `caps.anisotropic_filter = true`,
`caps.max_texture_anisotropy = 16` (TPU maximum; what st reports as
GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT), and `pvr_sampler_desc.anisoctl` is emitted into
TEXSTATE_SAMPLER_WORD0 from `pipe_sampler_state.max_anisotropy` (x16/x8/x4/x2), only when the
minification filter is LINEAR as GL specifies.

Verified with `tools/verify/aniso_probe.c`: a 256x256 checker with generated mips sampled through an
8x:0.5x anisotropic footprint returns exactly the zink values at max anisotropy 1 and 16
(128,128,128 / 127,127,127); GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT reports 16. ES2 extension list
69 -> 70. Full gate green (gltri 49, mip 28, rttest PASS, MRT 2/4, instancing, sRGB, border/mirror
clamp probes, 0 faults). Patch 0345, series head `0bc92d43`.

## Round-105 (2026-10-30): audit found glPolygonOffset (ES2 core) is not implemented - scoped

Looking for the next ES2 gap, the depth-bias path turned out to be missing entirely: the driver emits
no depth-bias table, `CR_ISP_DBIAS_BASE` points at whatever the captured stream had (a zero entry),
and the rasterizer state's `offset_units`/`offset_scale` are never read. In the reference this is
`sub_cmd->depth_bias_bo`: an array of `struct pvr_depth_bias_state { float constant_factor;
float slope_factor; float clamp; }` uploaded per job, indexed by
`ppp_state->depthbias_scissor_indices.depthbias_index`, with the constant factor pre-scaled by depth
format (`pvr_calculate_final_depth_bias_contant_factor`, UNORM vs FLOAT equation, plus the
`ROGUE_CR_ISP_CTL.dbias_is_int` mode on enhancement 42307). `VkPhysicalDeviceLimits.depthBiasClamp`
is true for this device, so the hardware supports the full feature including clamp.

So **glPolygonOffset is a missing ES2-core feature** (any app using it renders unoffset), and
implementing it also unlocks `GL_EXT_polygon_offset_clamp`. Scope for the next session: (1) allocate a
per-slot small BO for the bias table (written before submit, slot reuse already waits), (2) derive the
entry from `pipe_rasterizer_state` offset_units/offset_scale/clamp with the reference's constant-factor
scaling per depth format, (3) point `CR_ISP_DBIAS_BASE` at it and emit `TA_STATE_ISPCTL.dbenable` +
`depthbias_index` in the control stream, (4) verify with a two-quad depth-offset probe and zink
comparison. The probe drafted this round was inconclusive (both drivers rejected the quad, so its
setup is wrong); leave a working probe to the implementation round.

## Round-106 (2026-10-30): 3rd vendor restart Oops + first depth-bias attempt hung the GPU

**Reboot cause (06:18:30):** the third instance of the vendor `pvrsrvkm` bring-up crash, again on the
restore-to-vendor path: after `modprobe pvrsrvkm` a client opened the GPU while firmware/shader images
were still loading (`PVR_K: psConnectionPriv is NULL` x3 -> NULL-deref Oops -> hard reset). The
existing wait only covered "Initialized pvr" (drm registration), which happens seconds before the
firmware lines. `switch-safe.sh` now waits for `Initialized pvr` **and** `RGX Firmware image` **and**
`Shader binary image`, then settles 5 s before starting display-manager. Verified: the next
open->vendor cycle restored with 0 faults and 0 PVR_K errors.

**Depth-bias / glPolygonOffset first implementation (WIP, reverted):** the table/ISPCTL plumbing was
wired (per-slot 12-byte entries, `CR_ISP_DBIAS_BASE` patched in the fragment stream,
`TA_STATE_ISPDBSC.dbindex`, constant-factor UNORM pre-scaling, `dbias_is_int`) **but the native probe
aborted on the first polygon-offset draw** - the GPU did not complete that render (board stayed alive,
no kernel fault; process had to be timed out). So at least one of: table entry stride/format, dbindex
semantics, ISP_CTL.dbias_is_int bit, or the base-address patch is wrong. Reverted to keep the tree
clean; the patch is saved outside the repo (`/tmp/dbias-wip.patch`). Note the probe itself also needs a
stronger offset to be meaningful: zink with glPolygonOffset(-2,-2) still rejects the equal-depth quad,
so the expected-value setup must be re-derived before retesting. Next attempt should run the probe
under a short timeout with dmesg watched, and try the table variants one at a time.

## Round-107 (2026-10-30): depth-bias isolated - the dbias_is_int mode hangs the render

Two things were learned about `glPolygonOffset` today, with the probe finally made meaningful:

1. **The probe was wrong before**: a quad at NDC z=0.5 has window depth 0.75 (ES maps NDC [-1,1] to
   depth [0,1]), so "quad at clear depth 0.5" never tested what it claimed. The working probe now
   draws at NDC z=-0.5 (window depth 0.25) over a 0.5 clear and uses a large offset: zink shows
   `C less off +40000 -> black, D less off -40000 -> green`, i.e. polygon offset demonstrably works
   there, while the native driver ignores it (all green). That is the real gap, confirmed.
2. **The implementation's table plumbing does not crash** (DBIAS base patched in the fragment stream +
   entry written), but setting ISP_CTL bit 20 (`dbias_is_int`) makes the GPU stop completing the
   render: with `PVR_DBIAS_NO_INT` the probe runs to completion (offset still not applied, expected
   without int mode), and no kernel fault is logged either way - the job simply never finishes. So
   table/base/index semantics are probably fine and the int bias equation is the piece that needs a
   reference to match (perhaps it also needs the ZLS/TA controls in an int mode, or the firmware
   enable is elsewhere).

WIP saved as `/tmp/dbias-wip2.patch` with env switches (`PVR_DBIAS_NO_ENTRY`, `_NO_BASE`, `_NO_INT`,
`PVR_DBIAS_INDEX`) for the next attempt; tree reverted clean. Next step: find what else the reference
sets for integer depth bias (search `dbias_is_int` producers) or test with a F32 depth target where
the float equation applies and the int bit is unnecessary - that path may work today and would already
fix glPolygonOffset for apps using a depth32 texture.

## Round-108 (2026-10-30): int depth-bias mode hangs regardless of value; float-depth path unreachable

Further `glPolygonOffset` probing:
- The F32-depth route is **not reachable in ES2**: `GL_DEPTH_COMPONENT32F` is rejected by this driver's
  ES2 format set (zink accepts it, native returns GL_INVALID_ENUM and renders nothing), so there is no
  path where the hardware's float bias equation applies without the int bit.
- With D16 depth and the int-mode bit set, even a moderate constant bias (units 16384 -> 0.5 depth) makes
  the GPU stop completing the render - same hang as round-107, no kernel fault. The table/base plumbing
  alone is safe (completes, no visual effect), so `ISP_CTL.dbias_is_int` needs something else enabled
  (or the mode is broken on this firmware).
- Next approach for a working feature: capture a vendor Vulkan draw that actually uses depth bias
  (VkPipelineRasterizationState.depthBias* != 0) and diff its fragment/geometry stream against ours to
  find the missing enable/bits, instead of guessing. WIP saved at `/tmp/dbias-wip3.patch`.

## Round-109 (2026-10-30): reference sets ISP_CTL.dbias_is_int always; our bit alone still hangs

Captured the **reference** (open Vulkan driver, which zink uses and where polygon offset works) ISP_CTL
words during the same probe: every draw - with or without polygon offset - packs **0x80135000**, i.e.
bit 20 (`dbias_is_int`) is set unconditionally (`dbias_is_int=1 zls_int=1 has_ern=1`) and the value is
constant. The native driver's computed value for the same one-RGBA8-target render is 0x80035000, so
adding bit 20 makes the word identical to the reference.

Nevertheless, with the WIP applied the native probe still hangs on the offset draw in three
configurations: bit + table + base, bit + base, and **bit alone with neither table nor entry written**
(and with the TA-side dbenable forced off). No kernel fault; the render simply never completes. That
contradicts "bit 20 is the trigger", so the remaining difference must be elsewhere in the stream: the
native fragment stream's tail layout for a render with a depth buffer, or the TA/ISP depth-compare state
paired with the int equation. Next probe: add a fragment-stream dump to the reference Vulkan driver
(same hook style used for the VDM control stream earlier) and byte-diff it against the native stream for
an identical 64x64 depth-buffered clear+quad draw, with and without depth bias. WIP saved at
`/tmp/dbias-wip-final.patch` (includes the env switches and the reference ISP_CTL print).

## Round-110 (2026-10-30): glPolygonOffset works again - constant term emulated, clamp extension added

glPolygonOffset (ES2 core) was silently ignored. The hardware path is still unusable: with the
depth-bias table wired up, setting ISP_CTL bit 20 (`dbias_is_int`, which the reference packs
unconditionally as part of its constant 0x80135000) makes the firmware never complete the render, and
the hang reproduces even with the table/base untouched. So the feature is implemented where it can be
exact instead:

- the **constant term** `units * r` (r = smallest resolvable depth difference for the depth format) is
  added to the TA viewport depth transform for the draw. The viewport z words are post-projection, so
  this is an exact constant window-depth shift - the same depth the hardware bias would add.
- `rs->offset_clamp` clamps that bias, so `caps.polygon_offset_clamp` and
  **`GL_EXT_polygon_offset_clamp`** are honest; ES2 extension list 70 -> 71.
- the TA `dbenable` bit is off, so the unresolved hardware path cannot add unknown bias.

**Limitation (documented):** the slope-factor term (`factor * max depth slope`) is not applied.
Constant-term offsets - the usual shadow-mapping usage - are exact; sloped geometry gets only the
constant part. The hardware depth-bias path remains parked with the reference capture as evidence
(round-109: reference ISP_CTL always 0x80135000, our identical word hangs).

Verified with `tools/verify/polygon_offset_probe.c` (depth 0.25 quad over 0.5 clear): without offset it
passes, units +40000 rejects it, units -40000 passes - byte-identical to zink; with clamp 0.1 the
+40000 case passes again, also matching zink. Full gate green (gltri 49, mip 28, rttest PASS, rt8test,
MRT 2/4, instancing, sRGB, 0 faults). Patch 0346, series head `483a4890`.

## Round-111 (2026-10-30): one-command verify gate (`tools/verify/run-all.sh`)

The gate had grown into a long hand-run list of scripts and probes; it is now one command that exits
non-zero on failure and prints one line per group. It compiles the probes from source (so it is
self-contained), runs under the open driver and covers: gltri 49 modes, mip 14 levels, rttest, rt8test,
MRT 2 and 4 targets, instancing, sRGB, extended textures, half-float linear, cube faces, cube mips,
border clamp, mirror clamp, anisotropy, and polygon offset. First full run: **ALL GROUPS PASS**, 0
faults.

Current state after the feature rounds: patch series **0346** (head `483a4890`), ES2 extensions **71**
(zink 136, vendor 120 but ES3.2), every advertised ES2 extension backed by a matching zink probe,
three-way glmark2 last measured at native **805/806**, zink 775/772, vendor 1218/1203 (0.67x vendor,
1.04x zink). Known limitations recorded: polygon-offset slope term not applied; hardware depth-bias int
mode parked (hangs the firmware); twiddle/depth-cube/compressed uploads parked (need the transfer-job
port); ES3 core block not started.

## Round-112 (2026-10-30): post-feature benchmark + blend microbenchmark

Three-way glmark2 after the feature rounds (0342-0346: honesty caps, border clamp, mirror clamp,
anisotropy, polygon-offset emulation): native **806/805**, zink **778/771**, vendor **1211/1196** - in
noise of the earlier measurements, as expected for feature/extensions work (0.67x vendor, 1.04x zink).

Added blended-overdraw modes to `drawbench2.c` (27 overlapping quads, SRC_ALPHA/ONE_MINUS_SRC_ALPHA,
with and without depth test): **native 434 / 481 fps vs zink 208 / 236** - the native blending path is
about twice zink's, so blending is not part of the remaining GPU gap either. Together with the earlier
per-feature results (fill, texture, mip, cube, vertex all faster than zink) the only pattern where
zink wins remains many separate render passes, which round-94 attributed to GPU/firmware per-kick cost.

## Round-113 (2026-10-30): reference transfer-frag streams captured from the open Vulkan driver

The twiddle unlock needs the transfer-job port, so the first step taken: a temporary dump hook in the
open Vulkan winsys transfer submit (`PVR_DUMP_TQ`) captured the transfer-frag streams the reference
driver issues for the same texture-upload workload (zink probe creating four textures plus an FBO).
Five streams captured, all **176 bytes** (`captures/tq-ref/tq-*.bin`, length-prefixed):

- word 0 = `0x000000b0` (stream length), followed by the kernel/transfer header and the image/format
  words; the last words carry the pixel-event/TPU/PBE configuration.
- the five streams differ in exactly **three dwords** - word 5, word 39 and word 40 - increasing by
  `0x240` per capture. Those look like the per-job PDS/USC program page addresses, i.e. the reference
  uploads the transfer program alongside each job (which is also what the earlier TQ-fill replay probe
  had to install by hand at captured offsets).

So the next step for a real buffer->twiddled-image copy is: capture the **program buffers** those
streams reference (PDS pixel-event data/code and the USC kernel at the addresses in words 5/39/40,
either by dumping the reference's PDS/USC heaps around the submit or by replaying with our own
program pages), then replay one captured stream natively through `pvr_job_submit_transfer_sync` with
patched source/destination addresses - the same methodology the TQ-fill probe already proved works.
Stream captures are checked in under `tools/verify/captures/tq-ref/`; the dump hook itself was
temporary and is reverted.

## Round-114 (2026-10-30): transfer streams decoded with the kernel's field layout

The kernel source tree (`~/kernel-src/powervr`, `pvr_stream_defs.c`) has the authoritative field order
of the TQ_3D transfer stream, and `pvr_arch_srv_job_transfer.c` in the open Vulkan driver contains the
matching decoder, so the captured streams can be decoded exactly
(`tools/verify/decode_transfer_stream.py`). Layout: KMD stream header (length), then
`pds_bgnd0_base`, `pds_bgnd1_base`, `pds_bgnd3_sizeinfo`, `isp_mtile_base` (each 64-bit), nine 64-bit
`pbe_wordx_mrty` words, then 32-bit `isp_bgobjvals`, `usc_pixel_output_ctrl`,
`usc_clear_register0..3`, `isp_mtile_size`, `isp_render_origin`, `isp_ctl`, `isp_aa`,
`event_pixel_pds_info/code/data`, `isp_render`, `isp_rgn`, `frag_screen`.

Decoded comparisons:
- the reference's image->buffer copy stream (512x512) has `isp_render_origin = 0x20,0x20`,
  `pbe_wordx_mrty[1] = 0x000001ff020003fc`, `pds_bgnd3_sizeinfo` low = the event-program offset;
- the TQ fill stream has origin `0x800001`, every pbe word except `[0]=0x9680000000000000` zero, and
  the fill value in `usc_clear_register1`;
- the new zink texture-upload captures are copy-like (non-zero `pds_bgnd0/1`, `isp_mtile_base`,
  `usc_pixel_output_ctrl = 0x200`, origin `0x1,0x1`), differing from each other only in the three
  event-program offsets - i.e. same program, different heap slots.

Field meanings beyond the structure are still inferred (origin appears to be in tiles for the 512
copy; `pds_bgnd3_sizeinfo` packs two 32-bit regs), but the structure is now known, which is what a
port of the reference's transfer-stream builder needs. Next: reproduce the image->buffer copy stream
natively (patched addresses) as a first replay, then the buffer->image direction needed for uploads.

## Round-115 (2026-10-30): TQ copy bundle captured - streams + PDS programs + USC kernels

Captured everything a native replay needs, with two temporary hooks in the open Vulkan driver
(`PVR_DUMP_PDS` in `pvr_gpu_upload_pds`, `PVR_DUMP_USC` in `pvr_gpu_upload_usc`), running the same
texture-upload workload: five transfer streams (`tq-*.bin`, captured round-113), **74 PDS program
entries** (`pds.dump`) and **32 USC programs** (`usc.dump`). Correlated:

- the transfer streams' program fields point at kickusc PDS programs, e.g. stream `tq-0000` carries
  `isp_render = 0x2a00` / `event_pixel_pds_data = 0x2a20`, which is the PDS entry
  `data_off=0x2a00, data=[0x5d40, 0, 0x79000, 0x81a20, 0x90000002], code_off=0x2a20,
  code=[f0040101, f4000002]` - and its first data word **0x5d40 is the USC offset of the TQ kernel**
  (a 16-byte USC program captured at 0x5d40). The old 512x512 copy stream points at the same kind of
  program at `0x2c40/0x2c60` (USC offset 0x5d40 again), and other variants (0x2e80, 0x30c0, 0x3300)
  reference other TQ kernels (USC 0x5e40, 0x6000, ...) for other format classes.
- so the (name-mismatched) stream fields map as: `isp_render` = kickusc **data** offset,
  `event_pixel_pds_data` = kickusc **code** offset, `pds_bgnd3_sizeinfo` low = pixel-event program
  data offset, `pds_bgnd1_base` = a small PDS code offset (0x1e40, two instructions), matching what the
  TQ-fill replay probe had to install by hand.

Bundle checked in: `tools/verify/captures/tq-ref/{tq-*.bin, pds.dump, usc.dump}`. Next: locate the
destination address the streams use (candidates: `pds_bgnd0_base` = 0x410000000000, `isp_mtile_base`,
or the nine `pbe_wordx_mrty` words) by capturing the same copy with a known buffer address, then write
the native replay probe: install the captured PDS/USC programs at their captured offsets, map a target
buffer, patch the destination, submit via `pvr_job_submit_transfer_sync`, verify the bytes.

## Round-116 (2026-10-30): known-size buffer copy captured; TQ addresses not in the stream

Wrote `captures/tq-ref/vktq_copy.c`: a minimal Vulkan test that fills a source buffer with 0xdeadbeef,
`vkCmdCopyBuffer`s it to a second buffer, waits and verifies every dword (PASS on the open driver).
Captured its two transfer streams (`copystream2.bin`: fill then copy) plus the PDS programs.

Decoded the **copy** stream (176 bytes) against the fill stream:
- `pds_bgnd0_base = 0x410000000000` (same constant as the zink copies), `pds_bgnd1_base = 0x1e40`
  (same two-instruction PDS code), `isp_mtile_base = 0x01000000`, `pbe_wordx_mrty` **identical to the
  fill** (so PBE words carry no addresses), `usc_pixel_output_ctrl = 0x300` (fill: 0x0),
  `isp_rgn = 2` (fill: 3), event program offsets 0x2680/0x26a0.
- the kickusc PDS program for the copy (`data_off=0x2680`, 5 data dwords) is
  `[0x5d40, 0, 0x2ec00, 0x81a20, 0x90000002]`: first word is the USC TQ-kernel offset (as in round-115),
  the others look like USC constant/unified-store descriptors.
- `vkGetBufferDeviceAddressKHR` returns 0 on this driver, so the buffers' device addresses could not be
  read out to correlate directly.

Conclusion: the source/destination addresses for a TQ copy are **not in the job stream** - they are
supplied through the TQ shader's constant data (the kickusc program's 0x2ec00/0x81a20 words point into
the USC constant store). The next probe should write known 64-bit values into those constant slots (or
capture two copies with different buffer allocations and diff the kickusc data), then replay natively.
Captures checked in under `tools/verify/captures/tq-ref/`; the temporary dump hooks were reverted.

## Round-117 (2026-10-30): transfer stream field semantics confirmed from the reference source

Read `pvr_setup_hwbg_object()` in the open Vulkan driver's transfer job builder, which confirms the
decoded stream fields:
- `CR_PDS_BGRND0_BASE` = **shader_addr** (PDS shader task offset) + **texunicode_addr** (low/high 32-bit
  halves), so the copy stream's `pds_bgnd0_base = 0x00041000_00000000` means shader_addr 0 and
  texunicode_addr 0x41000;
- `CR_PDS_BGRND1_BASE` = **texturedata_addr** (the copy's 0x1e40, the two-instruction PDS code);
- `CR_PDS_BGRND3_SIZEINFO` = the four PDS **size fields** (usc_sharedsize / pds_uniformsize /
  pds_texturestatesize / pds_tempsize), which is why its low half tracked the pixel-event program offset
  in earlier decodes (16-bit fields).
- `event_pixel_pds_data`/`event_pixel_pds_code` are the pixel-event PDS data and code offsets,
  `isp_render`/`isp_rgn` the render extent and region settings, and the src/dst addressing is carried in
  the **PDS texture-state data** the job references (the captured PDS entries with DMA-like descriptors
  such as `[0xda, 0x21000, 0x2, 0, 0xda, 0x21030, 0x40002, 0, 0x5e40, 0x18]` - the 0x21000/0x21030
  values look like **offsets into the transfer-frag heap**, which the UAPI exposes as
  `DRM_PVR_HEAP_TRANSFER_FRAG`), not in the job stream itself.

So a native replay needs: a transfer context, the captured PDS/USC programs installed at their captured
offsets, and the transfer-frag heap laid out the way the captured PDS data expects (the 0x21000-class
offsets plus the TQ kernel at USC 0x5d40). That is the next concrete step; the capture bundle and
decoder are already checked in.

## Round-118 (2026-10-30): transfer stream address fields decoded to raw offsets

Resolved the address encoding in the transfer stream: `__pvr_address(addr, 4, 4, 31)` packs
`(addr >> 4) << 4` masked to bits 4..31, i.e. the field holds the address with its low 4 bits dropped.
The captured fresh-process copy stream (44 dwords) therefore decodes to:
`pds_bgnd0_base = { shader_addr = 0, texunicode_addr = 0x41000 }`,
`pds_bgnd1_base = { texturedata_addr = 0x1e40 }`,
`pds_bgnd3_sizeinfo = { 0x2640, 0x10001 }` (size fields in their units),
`isp_mtile_base = 0x01000000`, one PBE word pair non-zero (`0x96800000`, `0x02000ffc`, `0x1`, `0x7ff`),
`usc_pixel_output_ctrl = 0x300`, render origin `0x800001`, `isp_aa = 0x2500`,
pixel-event code/data `0x202`/`0x26a0`, `isp_render = 0x2680`, `isp_rgn = 2`, trailer `0x0107ff`.

Together with the captured PDS entries this pins where a replay would patch:
- **destination** address in the pixel-event data segment (this is exactly what the working TQ-fill
  replay probe patches: words 2/3 = `addr >> 2` low and `addr >> 34` high - the capture's
  `[0x5d40, 0, 0x2ec00, 0x81a20, ...]` has the same shape),
- **source** descriptor in the texture-state/unitex data the job names
  (`texunicode_addr = 0x41000`, `texturedata_addr = 0x1e40`; those offsets were not inside the ranges
  dumped this run, so the next capture should dump the full PDS and USC heaps or add hooks where the
  unitex/texture-state data is uploaded).

Next replay attempt: start from the already-working TQ-fill replay path, then add the copy state
(texture-state data + unitex code) with destination patched as above and source patched at the
texture-descriptor words.

## Round-119 (2026-10-30): texture-state data captured; addresses come from relocated/cached programs

Dumped the per-command texture-state data segment (`PVR_DUMP_TEXSTATE` hook in
`pvr_pds_unitex` after `pvr_pds_generate_pixel_shader_sa_texture_state_data`) for the same buffer copy
at two sizes. The whole segment is **4 dwords**: `[0x0004b080, 0x00000080, 0x90000014, 0]` for the
16 KiB run and `[0x0004b100, 0x00000080, 0x90000014, 0]` for the 64 KiB run - i.e. a tiny PDS DMA
program whose first words are USC task/constant addresses allocated per run, not the source image
descriptor. The actual texture descriptor (and therefore the source address) lives in the **cached TQ
shader data / unitex code** that the reference uploads once per shader variant (pvr_transfer_frag_store)
and references by offset; those offsets (texunicode 0x41000, texturedata 0x1e40) were outside the
ranges the PDS/texstate hooks observed in these runs, and the address inside the descriptor is patched
through the winsys's relocation machinery rather than written by the job builder.

So a native replay needs the cached TQ shader state as well - specifically the unitex/texture-state
programs at the offsets the job names. The capture plan that will get them: dump the whole PDS and USC
heap BO contents (or hook `pvr_bo_suballoc` allocations in pds_heap/usc_heap and dump every allocation
with its offset), for one known copy, and then replay with those bytes installed at their offsets.
Captures from this round are checked in (`tx-*.dump`, `cs-*.bin`).

## Round-120 (2026-10-30): complete PDS/USC/transfer heap dump captured for one copy

Wrapped `pvr_bo_suballoc` with an env-gated dump (`PVR_DUMP_HEAP`) that records every allocation in the
pds, usc and transfer-frag heaps with its heap offset, size and bytes. Running the known buffer copy
once produced **69 allocation records (106 KB)**: every PDS program (offsets 0x1000..0x73c0) and USC
program (0x1000..0x7000-class) the job uses, including the ones the stream names
(`texturedata 0x1e40`, pixel-event data 0x2640/0x2680).

The one value that is *not* a heap offset is the stream's `texunicode_addr` (0x41000): the stream field
packs `(addr >> 4) << 4` into bits 4..31, i.e. **the low 32 bits of a device address** (high bits are
dropped by the packet encoding) - so that field (and `shader_addr`) is a truncated device address, not a
heap offset. That matters for a replay: structures the TQ job addresses through those fields must live
below 2^32 in the replay VM, or the pump needs to keep the reference's address layout.

Bundle checked in: `captures/tq-ref/heap.dump`, `cs-heap.bin` (fill+copy streams). With this, everything
the reference job references is available byte-for-byte; the remaining work for a native copy replay is
the address-layout decision above plus installing the captured programs at their offsets in a transfer
context (the TQ-fill replay already proves that submission path works).

## Round-121 (2026-10-30): the TQ DMA space captured - where the copy's descriptors live

Hooked the reference transfer builder right after `pvr_write_usc_constants(sh_reg_layout, dma_space)` and
dumped that general-heap BO: **20 dwords** with device address `0x800004b080`. Two things fell out:

- the per-command texture-state data captured earlier (`TX`) starts with `[0x0004b080, 0x00000080]` - that
  is exactly **the low/high 32 bits of the DMA space's device address** `0x800004b080`, so the texstate
  segment's first words are the DMA burst source address, confirming the address split and explaining
  why the stream's `texunicode_addr` (0x41000) looked like a truncated address: the values in the stream
  are device-address low bits, not heap offsets.
- the DMA space contents are the TQ shader's descriptor constants:
  `c0002d84 00011ffc ec0007ff 10200002 0 0 0 0 0 00022400 ...` - i.e. packed TEXSTATE/PBE descriptor
  words for the source surface (format, tile/extent fields, and the region to copy), which is where the
  copy's source descriptor lives.

So the full data path is now identified: job stream (fixed constants) + pixel-event/kickusc PDS programs
(captured) + unitex code (pds heap, offset in stream) + this DMA space BO (general heap, address split
into the texstate segment). For a native replay the DMA space can be regenerated/patched directly: its
address must go into the texstate segment words 0/1 and the descriptors can be rewritten for the chosen
source/destination. Captures (`dma.dump`, `cs.dump`) checked in; hook reverted.

## Round-122 (2026-10-30): replay address model solved - fields carry heap-relative offsets

Printed the native VM's heap bases: every heap base ends in **32 zero bits**
(`general 0x8000000000`, `pds? 0xda00000000`, `0xe000000000`, `0xe400000000`, `f200000000`), and the same
is true for the reference VM's heaps (the captured DMA space address is `0x800004b080`, whose low 32
bits are exactly the `0x4b080` value written into the texstate segment). That resolves the apparent
contradiction of 32-bit address fields: the packet fields hold **heap-relative offsets** (the heap base's
low 32 bits are zero, so truncating a device address to its low 32 bits yields the offset), and the
firmware resolves them against the context's heap bases.

Consequences for the replay design (all now unblocked):
- the captured PDS/USC program bytes can be installed at their captured **offsets** in the replay
  context's own heaps - the base address difference does not matter, because the stream carries offsets;
- the TQ DMA space (general heap BO, address split into the texstate segment's first two words) can be
  allocated by the replay itself, with its 64-bit address written where the capture shows;
- the source descriptor inside the DMA space is TEXSTATE/PBE words the replay can regenerate for its
  chosen source buffer (twiddled address fields are heap addresses offset by the same rule).

So the remaining work for the transfer-job port is ordinary driver integration rather than reverse
engineering: build a transfer context, install the captured pixel-event/kickusc/unitex programs, create
the per-command DMA space and stream (with the constants decoded in rounds 114-118), submit and verify.

## Round-124 (2026-10-30): probe builds again; TQ-fill replay re-validated on hardware

`pvr-probe` had not compiled since `pvr_csb_pbe_state_words()` grew its `emptytile` argument; the
end-of-tile check was never updated. Fixed (pass false - a render target), so the probe builds again as
a usable in-repo driver self-test. Running it with the captured fill stream on the current driver:
`tq: fill replay COMPLETED (sync signalled)`, `readback changed=4096/4096 mismatched=0 first=0xdeadbeef`,
`PASS (native TQ fill job writes the target)` - the transfer-job submission path is re-validated today,
which is what the transfer port builds on. Patch 0347, series head `e1f0d2c` (probe fix only, no
driver behaviour change).

Next for the copy replay: hook `pvr_bo_alloc` in the reference Vulkan driver to log general-heap BO
addresses/sizes for the same copy run (so the two 16 KiB staging buffers' device addresses are known),
correlate them against the captured DMA space words to find the source/destination fields, then replay
the copy in the probe with those fields patched.

## Round-125 (2026-10-30): address-shift experiment narrows where the copy addresses live

Allocator/rename of the reference driver's buffers can be controlled from the Vulkan test: with
`PAD=<bytes>` the copy tool allocates a large host-visible buffer before the source/destination, which
moves both by a known delta. Instrumented the reference driver with hooks for all winsys heap
allocations, the per-command DMA space, the IPF control stream and the PDS uploads, then diffed base vs
padded runs (dumps in `captures/tq-ref/addr-shift/`):

- heap allocations confirm the shift: source/destination went from `0x80000b7000 / 0x80000bb000` to
  `0x80008b7000 / 0x80008bb000` (+0x800000) with an 8 MiB pad.
- the **transfer job streams are byte-identical** between the two runs - the copy's addresses are not in
  the job stream.
- the **per-command DMA space** changes in one word only (`0x10200002` -> `0x10200022`) and the **IPF
  control stream** does not change at all.
- the **PDS upload data** changes in exactly two words (`0x0002dc00 -> 0x0022dc00`,
  `0x0002ec00 -> 0x0022ec00`), which match the DMA space BO address shifting by 0x200000 - i.e. those are
  the DMA-space address words, not the copy source/destination.

So the copy's source/destination addresses live somewhere the hooks above do not reach yet. Next
candidates, in order: the **pixel-event PDS data segment** (the working TQ-fill replay patches its
destination there, so the copy almost certainly carries its addresses there too - the current hook only
dumps `pvr_gpu_upload_pds`, while the per-command pixel-event program is generated in
`pvr_pds_generate_pixel_event_data_segment` and uploaded through a different path), and the **TQ
shader's USC constant data**. Next step: hook the pixel-event data generation in the transfer path and
diff base vs padded again.

## Round-126 (2026-10-30): pixel-event data does not carry the copy addresses either

Hooked the per-command pixel-event program generation (`pvr_pds_generate_pixel_event_data_segment`
inside `pvr_pbe_setup_emit`) and diffed base vs padded runs again. The two programs generated per copy
(the filler and the copy) differ only in the DMA-space address words already seen in the PDS dumps
(`0x2dc00`/`0x2ec00` shifting with the DMA-space BO), no source/destination buffer words appear.

Together with round-125 this rules out the job stream, DMA space, IPF control stream, PDS pixel-event
data and pixel-event constants as carriers of the copy's buffer addresses. Reading the builder code
explains why an address shift of +0x800000 (a multiple of 2^23) would *not* show up anyway: the only
address-derived state the builder writes is the **texel-unwind alignment**, which depends on the low
bits of the address (the +0x800000 pad preserves the low 23 bits). The next experiment must use a pad
whose size changes the low address bits of the buffers (e.g. 0x20000 or a set of allocations that
leave a gap), then diff the same hooks: that will expose the unwind/alignment words that do carry the
address. The capture set for this round is small; the hooks were temporary and are reverted.

## Round-127 (2026-10-30): transfer command addresses confirmed; still not found in any dumped structure

Printed the transfer commands' actual addresses per job: base run `dst=0x80000bb000 src0=0x80000b7000`,
8 MiB-pad run `dst=0x80008bb000 src0=0x80008b7000` - so the addresses really do shift as measured.
Dumped the **second** DMA-space branch (the one actually used for source image state, `DMA2` at
`0x800004b100`, 20 dwords): it contains the same kind of TEXSTATE words
(`c0002d84 00011ffc dc0007ff 10200002 ... 00002400`) and again **no raw source address**; the only
change across the address shift is `N[3]` (`0x10200002 -> 0x10200022`, again a low-bit alignment field).

So the copy addresses are consumed by the driver but not written verbatim into the job stream, DMA/texstate
data, pixel-event data, IPF control stream or PDS uploads - they must be encoded in a form not yet decoded
(two remaining candidates: the TQ shader's **USC constant/uniform store** - written through the PDS
"doutu"/DMA mechanisms - or descriptor words whose address is expressed as a **base+delta pair** where the
base comes from a register we have not varied yet). The next step is to vary the *base* register
(e.g. `isp_mtile_base`/texturedata bases) or to dump the USC common store after a kick, and to diff with a
low-bit-changing offset - the straightforward captures all came back clean.

Everything from this investigation is checked in: `captures/tq-ref/` (streams, PDS/USC/heap/texstate/DMA
dumps, address-shift pair, decoder, `vktq_copy.c` with PAD/PADSIZE/DSTOFF). The TQ-fill replay in
`pvr-probe` remains the working proof that the submission path itself is sound.

## Round-128 (2026-10-30): FOUND IT - the copy addresses are (addr>>2) in the pixel-event/PDS program data

Re-ran the address-shift experiment and matched every dumped dword against candidate encodings of the
known transfer addresses (`source 0x80000b7000`, `destination 0x80000bb000`, and the +0x800000 shifted
pair). The two words that changed in the pixel-event data and PDS dumps are exactly:

    word = (addr >> 2) & 0xffffffff

base:        src 0x80000b7000 -> 0x0002dc00,  dst 0x80000bb000 -> 0x0002ec00
8 MiB pad:   src 0x80008b7000 -> 0x0022dc00,  dst 0x80008bb000 -> 0x0022ec00

So those "DMA-space address" words from rounds 121-127 are in fact the **transfer source and
destination addresses**, encoded as `addr >> 2` (a dword index/36-bit address form), with the high bits
carried in the neighbouring word (constant `0x81a20` for this kernel; the general heap range
0x8000000000-0xa000000000 keeps `addr >> 34 == 2`, so it does not change). The captured fill-replay
probe already patches its destination the same way (`addr >> 2` low 32 bits + bits 0..5 = `addr >> 34`),
which closes the loop.

Replay recipe (all pieces now known and checked in):
1. load the captured transfer stream (fill stream 0 and copy stream 1 from `cd -- cs.dump`);
2. install the captured PDS/USC programs at their captured heap offsets (bundles in
   `captures/tq-ref/`: `heap.dump` has every program's bytes and offsets);
3. map the source and destination buffers in the general heap, write the copy stream's
   pixel-event data word 2 with `dst >> 2` and the kickusc PDS data word 2 with `src >> 2` (high-bit words
   keep their captured templates; the general heap keeps their value valid);
4. submit on a transfer context and verify the destination bytes - the probe's TQ-fill self-test already
   proves that submission path works.

This is the last missing piece of the transfer-job reverse engineering; the next round implements the
copy replay in `pvr-probe`.

## Round-129 (2026-10-30): first native TQ copy replay runs - job submits, destination not written yet

Implemented the copy replay in `pvr-probe` (`tq_copy_check`, env `PVR_PROBE_TQ_COPY=<stream>`, patch 0348):
it maps source `0x80000b7000` / destination `0x80000bb000` in the general heap, installs the captured
pixel-event data/code at PDS+0x2680/0x26a0 (destination patched as `addr >> 2` in data word 2, exactly
as the working fill replay does), the captured texturedata block at 0x1e40 and the TQ copy kernel at
USC+0x5d40, then submits the captured 176-byte copy stream on a native transfer context.

Result: the job **submits and its sync signals** (`copy replay COMPLETED`), so stream, programs and
submission are accepted - but the destination stays zero (`readback changed=0/4096`). The copy kernel
therefore also needs the per-command **unitex / texture-state program** that the copy stream names
through `pds_bgnd0.texunicode_addr = 0x41000` (a PDS heap offset in the reference run, not captured in
our dump whose offsets end at 0x26a0 - the reference's PDS suballocator had clearly placed that program
higher up). The next step is to capture it (hook the command-buffer PDS upload path during the reference
copy, or dump the full PDS heap after the copy) and install it at PDS+0x41000, then re-run
`PVR_PROBE_TQ_COPY=.../copy-stream.bin`.

All artifacts checked in: `captures/tq-ref/{copy-stream.bin, copystream2.bin, copypds.dump, heap.dump}`,
probe patch 0348 (series head `1cbcd3b`), and the fill replay still passes.

## Round-130 (2026-10-30): kickusc programs captured; copy replay still does not write

Corrected the stream decode first: `KMD_STREAM_HDR` is **2 dwords**, not 1 (the earlier decoder was off
by one word). With the correct header the copy stream reads: `pds_bgnd0 = {shader_addr 0x41000,
texunicode_addr 0x1e40}`, `pds_bgnd1 = {varying 0, texturedata_addr 0x2640}`, pbe words
`96800000 02000ffc`, `isp_ngobjvals 0x300`, `usc_pixel_output_ctrl 0x7ff8`, `mttile_size 0x800001`,
`event_pixel_pds_info 0x202`, `event pixel code 0x26a0`, `data 0x2680`.

Captured the missing program: the TQ shader's **kickusc PDS program**, created through
`pvr_bo_alloc(pds_heap)` rather than `pvr_gpu_upload_pds`, so earlier hooks missed it. A new hook shows
it at **PDS+0x41000** and **PDS+0x42000** (data `[0x7340, 8, 0...]`, code `f4000002`). The replay
(`tq_copy_check`, patch 0349) now installs those, writes a reconstructed texture-state DMA block at
PDS+0x2640 (`[0xcf000, 0x80, 0x90000020, 0]`) and maps a 4 KiB general-heap DMA space at
`0x80000cf000` with the captured TEXSTATE words.

Result: the job still submits and syncs but writes nothing. The remaining unknown is the **exact
texture-state DMA block** (its address/size encoding and the source descriptor it DMAs) - the captured
copy run recorded neither its content nor the source surface descriptor, so it needs another capture
with a hook on `pvr_write_usc_constants`/`pvr_pds_unitex` in the copy branch (or a full general-heap dump
around the copy) to get it byte-exact. Patch 0349, series head `ebfe662e` (probe-only changes; default
probe results unchanged, fill replay still passes).

## Round-131 (2026-10-30): byte-exact copy job inputs captured in one run

Captured a **single aligned bundle** of everything the copy job uses in one `vktq_copy` run (checked in as
`captures/tq-ref/bundle/`): the two transfer streams (`cs.bin`), all PDS uploads (`pds.dump`), all USC
programs (`usc.dump`, including the 64-byte TQ copy kernel at **USC+0x7340** and its neighbours
0x7180/0x73c0), and the texture-state DMA blocks (`tx.dump`: offset **0x2640** for the copy job =
`[0x0004b080, 0x00000080, 0x90000014, 0]`, i.e. the general-heap DMA space address 0x800004b080 split
into words 0/1, plus 0x2700 for the fill job).

The probe replay (patch 0350) now uses those exact values: DMA space mapped at 0x800004b000 with the
captured TEXSTATE at +0x80, the 64-byte copy kernel at USC+0x7340, and the texstate block at PDS+0x2640.
The job still submits and syncs without writing the destination. The remaining gap is the
kickusc/pixel-event **task binding**: the kickusc program's data word 0 (0x7340 in the earlier capture)
names the USC task offset, and the aligned bundle now provides the matching kernel bytes and the exact
pixel-event data for the same run - wiring those three together is the next step.

## Round-132 (2026-10-30): 4th vendor reset (probe run on the vendor driver) + copy replay updated

**Reboot cause:** the probe experiment from the previous step ran while the board was already restored to
the vendor driver (the standing rule is that no harness may run against `pvrsrvkm`). Opening the vendor
stack from a normal client reproduced the known vendor bug - `PVR_K: psConnectionPriv is NULL` x3, then
`Unable to handle kernel NULL pointer dereference` and a hard reset at 13:15:35. This is the fourth
occurrence; the hardening added earlier covers the driver *swap* races, but nothing prevents a stray
harness from being started on the vendor driver - the rule to check `readlink
/sys/bus/platform/devices/1800000.gpu/driver` before any probe run is now explicit in the safety list.

Post-reboot state: clean boot, vendor+desktop, 0 faults. The interrupted build left a corrupt
`libpowervr_uscgen.a` (archive without index); `ninja -t clean` + relink fixed it and `pvr-probe` builds
again.

**Copy replay:** the aligned bundle shows the copy job's pixel-event data names USC task 0x5d40 while its
kickusc program names 0x7340, so the replay now maps a 16 KiB USC page at USC+0x5000 with **both
captured kernels** (16-byte fill-style at 0x5d40, 64-byte copy kernel at 0x7340). The job still submits,
syncs, and writes nothing. The remaining gap is the kickusc task-control binding (captured data word 0/1
= `0x7340/8`): either the kernel is entered with the wrong task/sample-rate control or the DMA space
descriptor still needs the source surface words. Patch 0351, series head `189d18a7` (probe only; default
probe unchanged, fill replay still passes).

## Round-133 (2026-10-30): replay iterations after the 4th reset - still no destination write

Post-reset continuation (the reset itself was round-132: a probe run against the vendor driver). Health
after reboot: clean, vendor+desktop, 0 faults.

Replay progress this round:
- re-captured the reference's DMA space for the **fill** branch (`dev 0x800004b100`, 20 dwords); the
  write hook sits in the fill path, so the **copy** job's space (0x800004b080) still comes from the
  round-121 capture, where its content differs from the fill variant exactly in word 2
  (`0xdc0007ff` fill vs `0xec0007ff` copy) and word 9 (`0x00002400` fill vs `0x00022400` copy) - the
  copy values are now used by the replay;
- retried the copy replay with both captured USC kernels (16-byte at 0x5d40 for the pixel-event
  program, 64-byte at 0x7340 for the kickusc program), the exact texstate block
  (`0x800004b080` split + `0x90000014`), and the copy-variant DMA content: the job still submits and
  syncs and writes nothing;
- found a second issue while iterating: with the copy replay path active the probe's later self-checks
  hit a host-heap corruption (`malloc(): unaligned tcache chunk detected`), so the replay helper needs
  its mappings/sizes audited before it can be trusted in the same process. The experimental edits were
  reverted to keep the series at patch 0351.

Remaining for the copy replay: bind the kickusc task correctly (the captured task-control words
`0x7340/8` may need the USС shared/temps fields regenerated) and make the replay process-safe. The
aligned bundle and all captures remain checked in.

## Round-134 (2026-10-30): task binding ruled out for the copy replay

Added copy-only mode (`PVR_PROBE_TQ_COPY_ONLY`) so the replay can run without disturbing the probe's other
self-checks (this also avoids the host-heap corruption seen when it ran inside the full probe), and made
both USC task offsets runtime-overridable. Tested all four combinations of pixel-event and kickusc task
offsets (0x5d40 / 0x7340):

- every combination **submits and syncs**, and none writes the destination.

So the task binding is not the remaining blocker. Since the stream's PBE/pixel-event state, both USC
kernels, the kickusc programs, the exact texstate block and the copy-variant DMA state are all installed
and correct, the captured copy job must depend on further firmware-visible state the replay does not
reproduce (e.g. the kickusc program expects the TQ shader store's USC shared/register initialisation, or
the DMA space descriptor's surface words need to be regenerated against our buffers rather than copied
from the reference run). This needs the reference's own value for those words or a firmware trace; the
aligned capture bundle is the basis for both.

## Round-137 (2026-10-30): regenerated kickusc programs also do not write - copy replay parked

Added a generator mode to the probe replay (`PVR_PROBE_TQ_COPY_GENKICK`): the kickusc background program is
rebuilt with `pvr_pds_kick_usc()` / `pvr_pds_setup_doutu()` for a selectable USC temps count instead of
using the captured bytes. Generated programs (temps 8 and 4, task offset 0x7340) still submit, still sync
and still write nothing.

Positive/negative matrix for the copy replay is now: captured stream + pixel-event data/code + both USC
kernels + kickusc programs (captured and regenerated) + exact texstate block + copy-variant DMA state +
all task-offset combinations -> always submits+syncs, never writes. The remaining dependency is therefore
the **DMA-space source-surface state** the reference produces (or some other firmware-visible state not
captured by any hook used). Decoding that requires either the reference's exact values for those words
(hook `pvr_write_usc_constants` in `pvr_3d_copy_blit_core` specifically for this job) or a firmware trace
of the reference copy job - both are follow-up work, not quick experiments.

The transfer-job port stays parked with the full capture bundle and harness checked in
(`tools/verify/captures/tq-ref/`, probe patches 0348-0353); the shipped ES2 driver state is unaffected
(patch 0353 changes pvr-probe only, default probe behaviour unchanged).

## Round-138 (2026-10-30): float render-target extensions need the R16/R16G16 formats first

Checked what ES2-era extensions zink has that the native driver lacks (zink 136 vs native 71): almost all
of the difference is ES3-only, and the only ES2-valid candidates left were float colour-buffer/float-blend
(`EXT_color_buffer_half_float`, `EXT_float_blend`) and MSAA-related ones (blocked). Allowed
`R16G16B16A16_FLOAT` and `R32G32B32A32_FLOAT` as render targets and rebuilt: the extensions still did not
appear, because Mesa's state tracker requires **all** of `R16_FLOAT`, `R16G16_FLOAT` and
`R16G16B16A16_FLOAT` to be renderable+sampleable before it advertises
`EXT_color_buffer_half_float` - and the driver has no descriptor/layout mapping for the single- and
dual-channel half-float formats yet. The change was reverted (no behaviour shipped).

Next step for these extensions: add `PIPE_FORMAT_R16_FLOAT` / `PIPE_FORMAT_R16G16_FLOAT` to
`pvr_format_layout_from_pipe`, the TPU format table, PBE source-format/pack-mode mapping and the
texture-format support list, then re-check the cap exposure and gate a render/blend/readback probe.
`EXT_float_blend` similarly needs the 32F render target path exercised end-to-end before it can be
advertised honestly.

## Round-139 (2026-10-30): half-float render targets expose the extension but fail FBO validation

Implemented the format plumbing for half-float colour targets: added `PIPE_FORMAT_R16_FLOAT` /
`PIPE_FORMAT_R16G16_FLOAT` to `pvr_format_layout_from_pipe` (TEXSTATE `F16` / `F16F16`), accepted
`R16_FLOAT` / `R16G16_FLOAT` / `R16G16B16A16_FLOAT` as render targets in `pvr_is_format_supported`, and
made the PBE pack mode channel-count aware (`F16` / `F16F16` / `F16F16F16F16`). With that,
**`GL_EXT_color_buffer_half_float` did appear** (ES2 extensions 71 -> 72).

But the honesty gate failed: rendering into all three formats reports
`GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT (0x8cd6)` in a probe that creates the float texture, attaches it,
clears + blends a quad and samples it back to RGBA8. So the extension was **reverted** rather than
advertised; the tree is clean at patch 0353.

Next step for the feature: find what Mesa's attachment validation still rejects (the driver now claims
`PIPE_BIND_RENDER_TARGET` for the formats, so it is likely a validation/cap condition rather than
is_format_supported: e.g. the resource's bind flags from the texture-creation path, a missing
`PIPE_CAP_*` or the single/dual-channel clear/readback path). Debug with `MESA_DEBUG=1` on the probe and
read `st_validate_attachment`, then re-test render + blend + sample before exposing the extension.
`EXT_float_blend` needs the same treatment for `R32G32B32A32_FLOAT` afterwards.

## Round-140 (2026-10-30): half-float targets - clear works, draws do not; R16F/RG16F rejected

Follow-up debug of the reverted half-float colour-target work, using the correct ES2 API usage
(unsized `GL_RGBA` + `GL_HALF_FLOAT_OES` becomes RGBA16F under OES_texture_half_float):

- **RGBA16F:** the texture creates, the FBO is **complete**, and a clear works - but a drawn/blended quad
  leaves only the clear colour in the target (sampled back it reads exactly the clear colour, no error
  raised). So the 3D draw path does not actually emit fragments for a 16F target even though the PBE
  state accepts the format; the clear path is separate.
- **R16F / RG16F:** `glTexImage2D` fails outright with `GL_INVALID_VALUE`, and the driver log shows the
  compute-fallback path rejecting the non-RGBA8 target ("target must be a non-empty RGBA8 2D texture"),
  i.e. the driver has no texture path for single/dual-channel half-float in ES2 yet.

So `EXT_color_buffer_half_float` cannot be advertised honestly: one format renders only clears, two do
not even create. The plumbing attempt is reverted again (tree clean at patch 0353). To land the extension
later: (1) make the draw path emit into a 16F target (PBE source format/packmode and EOT/pixel program for
float outputs), (2) give R16F/RG16F a real texture/upload path (compute fallback currently only handles
RGBA8), then re-run the render/blend/sample probe before advertising.

## Round-141 (2026-10-30): EXT_color_buffer_half_float landed - RGBA16F/RGB16F render, blend and sample

The earlier two failures had separate causes: the draw path's `draw_format_ok()` refused 16F targets
(so the PBE/draw was never submitted), and the probe itself used the wrong ES2 API usage. Fixing both:

- added `R16_FLOAT` / `R16G16_FLOAT` to `pvr_format_layout_from_pipe` (TEXSTATE `F16` / `F16F16`),
  accepted the three half-float formats as render targets, made the PBE pack mode channel-count aware
  (`F16` / `F16F16` / `F16F16F16F16` - it was hard-coded to four channels), and allowed RGBA16F in
  `draw_format_ok()`;
- the probe now uses the correct ES2 path: **unsized** `GL_RGBA`/`GL_RGB` + `GL_HALF_FLOAT_OES`
  (effective RGBA16F / RGB16F; the sized R16F/RG16F internal formats are GLES3-only in Mesa's ES
  validation, which matches the reference stack).

Verification (`tools/verify/f16rt_probe.c`): RGBA16F and RGB16F FBOs are complete, clear, blend a quad
and sample back through an RGBA8 pass - sampled values match the expected 159/64/96 for every case
(delta 0, no GL errors). Full verify gate (16 groups) still passes. `GL_EXT_color_buffer_half_float`
is now advertised: **ES2 extensions 72**. Patch 0354, series head = this commit.

## Round-142 (2026-10-30): ES2 extension surface complete except hardware-blocked items

With `EXT_color_buffer_half_float` landed (ES2 extensions 72), listed every ES2-valid extension that zink
has and the native driver lacks (19): `AMD_performance_monitor`, `EXT_clip_control`,
`EXT_demote_to_helper_invocation`, `EXT_depth_clamp`, `EXT_frag_depth`, `EXT_memory_object(_fd)`,
`EXT_multisampled_render_to_texture(2)`, `EXT_robustness`, `EXT_semaphore(_fd)`, `EXT_shadow_samplers`,
`KHR_robust_buffer_access_behavior`, `KHR_robustness`, `NV_conditional_render`, `NV_texture_barrier`,
`OES_depth_texture_cube_map`, `OES_texture_3D`.

Every one is blocked by a known hardware/feature gap rather than missing GL plumbing: MSAA
(multisampled_render_to_texture), twiddled 3D/array + depth-cube uploads (OES_texture_3D,
OES_depth_texture_cube_map - both need the transfer-job port), occlusion queries
(NV_conditional_render), external memory/semaphores, robustness (not wired through the UAPI), fragment
depth and shadow samplers (withdrawn earlier: no honest backend), clip control/depth clamp and demote
(no backend path). So ES2 extension work is effectively finished: **72 extensions, all backed by
verification probes**; the next capability gains require the transfer-job/twiddle work or the ES3 core
block, not more ES2 extensions.

The RGBA32F render-target experiment (for `EXT_float_blend`) was reverted: that extension is **ES3-only**
in Mesa's table, so there is nothing to expose for it in an ES2 context.

## Round-143 (2026-10-30): where the remaining 1.5x vendor gap really is - FP16 ALU

Broke the latest glmark2 run down per scene (native/vendor ratio, from `bench31-results`):

| scene | native | zink | vendor | n/v |
|---|---|---|---|---|
| shading cel | 530 | 546 | 1171 | 0.45 |
| refract | 38 | 47 | 76 | 0.50 |
| terrain | 20 | 27 | 39 | 0.51 |
| shading phong | 653 | 624 | 1184 | 0.55 |
| shadow | 390 | 365 | 690 | 0.57 |
| bump height | 1011 | 967 | 1715 | 0.59 |
| shading blinn-phong-inf | 797 | 740 | 1340 | 0.59 |
| bump high-poly | 407 | 367 | 668 | 0.61 |
| texture nearest/linear/mipmap | 1452/1433/1480 | 1299/1302/1398 | 1597/1559/1554 | 0.91-0.95 |
| build use-vbo | 1213/1313 | 1157/1212 | 1327/1731 | 0.76-0.91 |

Two facts matter: (1) the native driver **beats zink in most scenes** (cel, phong, shadow, bump, ideas,
effect2d), so submission/CPU/state work is not the gap; (2) the scenes where vendor wins by ~1.7-2.2x are
exactly the **ALU-heavy fragment-shader** scenes (shading, refract, terrain, bump, shadow), while
texture/build scenes are within 5-10%.

The likely cause is precision: on PVR, `mediump` fragment arithmetic can run on the FP16 pipe (roughly 2x
FP32 throughput), and the vendor GLES compiler exploits it. `pco` has **no 16-bit arithmetic at all**
(the ISA layer has no FP16 op encodings; the only 16-bit support is I/O widening and PBE/F16 packing), so
native falls back to FP32 ALU exactly like the Vulkan path - which is why zink (IMG Vulkan compiler) and
native are neck and neck. Confirmed by grep: no f16 encodings in `pco_isa.h.py`/`pco_common.h.py`.

So beating the vendor GLES stack on these scenes requires **FP16 ALU support in pco** (ISA encodings,
NIR fp16 lowering of mediump arithmetic, precision selection), a compiler feature far beyond a GL patch;
with it the shading/terrain/refract scenes are the ones that would roughly double. Without it the current
ratio (~0.67x aggregate, native ahead of zink everywhere CPU-bound and on texture/build scenes) is the
FP32 ceiling. Recorded as the top performance item for a future session; no driver change this round.

## Round-145 (2026-10-30): FP16 ALU feasibility - lp_fmad alone is worth only ~2%

Tested the cheapest form of the FP16 idea: pco already models the fmad **low-precision bit**
(`OM_LP` -> `lp` in I_FMAD_EXT), so an env-gated experiment forced `PCO_OP_MOD_LP` on every `nir_op_ffma`
and ran the shading benchmark through the same weston/Xwayland harness:

    shading default: 946 FPS (LP off) -> 966 FPS (LP forced)   (+2%)

So the lp fmad bit alone is not the vendor's win. The shading scenes are dominated by the **single-op
transcendentals** (frcp/frsq/flog/fexp/fsinc/...), and the hardware capability that matters is
`has_usc_f16sop_u8` (FP16 single-op mode). pco has **no F16SOP support at all** - `pco_isa.py` has a
literal `# F16SOP` / `# TODO` placeholder where those encodings should be. Reproducing the vendor's 2x on
ALU-heavy scenes therefore needs the F16SOP instruction encodings added to the ISA model and the
corresponding low-precision lowering, which is compiler work beyond this session; recorded as the precise
blocker for the performance goal. The experiment was reverted (tree clean at patch 0354).

## Round-146 (2026-10-30): depth-only rendering is not a gap either (native 2.4x zink)

Added a `depthonly` mode to `drawbench2` (27 depth-tested draws with the colour mask off - the pass
shape shadow mapping uses) and measured on the same board state: **native 455 fps vs zink 186 fps**
(2.4x). So the shadow scene's vendor advantage is not a depth-only-path inefficiency either; like the
other shading scenes it lands on the FP16-vs-FP32 fragment ALU difference (round-145). The microbench
mode is kept in `tools/verify/drawbench2.c` with the other modes.

## Round-147 (2026-10-30): verify gate covers half-float colour targets (17 groups)

Extended `tools/verify/run-all.sh` with the `f16rt_probe` (RGBA16F/RGB16F render+blend+sample check) so
the newly advertised `EXT_color_buffer_half_float` stays covered by the one-command gate. Full run on the
current driver: **ALL GROUPS PASS** (17 groups: gltri 49, mip 14, rttest, rt8test, MRT 2/4, instancing,
sRGB, extended textures, half-float linear, cube faces, cube mips, border clamp, mirror clamp,
anisotropy, polygon offset, half-float colour targets), 0 faults.

## Round-148 (2026-10-30): final three-way benchmark at patch 0354

Full glmark2 run on the current driver (same x11-glesv2 binary, governor performance, off-screen):
**native 806 / 806**, **zink 775 / 777**, **vendor 1226 / 1196**.

So with the session's work (patches 0332-0354: cube maps, EOT cache, ISP tile config, honesty withdrawals,
border/mirror clamp, anisotropy, emulated polygon offset, half-float colour targets, extension surface
72) the driver still holds **~0.66x vendor / ~1.04x zink**, i.e. the ES2 feature work did not move the
glmark2 aggregate (as expected - those scenes are dominated by the FP16-vs-FP32 fragment ALU difference,
round-143/145) while adding the capability surface. Logs archived under `bench/round-148`.

## Round-149 (2026-10-30): bench31 restore hardened (desktop session check)

After the last benchmark's vendor restore the Plasma session did not come up on the first
`display-manager` start (kwin/Xorg missing), even though the driver had restored correctly. `bench31.sh`
now checks `pgrep -x kwin_x11` after starting the display manager and restarts it once if the session is
missing; the copy in `tools/verify/bench31.sh` is kept in sync. This is the only way the harness has
failed to leave the board in a usable state since the switch-safety work, so it is recorded and fixed
rather than left as a manual step.

## Round-150 (2026-10-30): copy stream does not write even when made fill-like

Experiment: take the captured copy stream in the replay and zero its background-program fields (bgnd0,
bgnd1, bgnd3, mtile base, isp_bgobjvals) while writing a fill pattern into the USC clear register - i.e.
turn the copy job into a fill-like job - then check whether the destination gets written. Result: **still
no write** in every mask variant. So the blocker is not the background-program state either; the copy
stream itself does not reach the "write destination" behaviour the working fill stream does.

The next comparison should be a byte-level diff of everything the two replays set up for a job that
writes (the fill stream) versus the copy stream: which of the remaining u32 fields differ (`isp_render`
2 vs 3, `isp_aa` 0 vs 0x2500, render origin/mtile size, the pbe words), which program offsets are
installed for each, and whether the copy stream expects additional context state that the reference sets
up at first transfer-context use. The working fill replay is the control: it proves the context,
mapping and screenshot path are fine, so the difference is inside the job description.

Experiment reverted (probe-only code unchanged; tree clean at patch 0354).

## Round-151 (2026-10-30): stream-selection for bisecting the copy replay

The replay can now select one stream out of a multi-stream capture (`PVR_PROBE_TQ_COPY_STREAM`, 0-based,
default 1 = the copy job in the bundle) and keeps the fill-like hybrid switch. Verified index selection
against `captures/tq-ref/bundle/cs.bin` (stream 1 = the copy job, still no destination write). This makes
the next bisection practical: replay stream 0 (the fill job, known to write) through the same code path
and then move fields/offsets one at a time toward the copy job to see exactly which description kills the
write. Probe-only patch 0355, default behaviour unchanged.

## Round-152 (2026-10-30): background-tag path identified as the copy-replay blocker

Built the bisection harness the previous round set up (probe patch 0356): stream selection, the fill
job's programs installed alongside the copy job's, a "morph" switch that moves fill-stream fields toward
the copy job's values, and the reverse "hybrid" switch on the copy stream. Results:

- **Control:** replaying the captured **fill** stream through the copy-replay path writes the destination
  (4096/4096 pattern) - the context, mapping, program install and submission are all good.
- **Morph:** starting from the fill stream and applying the copy job's pixel-event offsets, bgnd0, bgnd1,
  bgnd3 and isp_bgobjvals keeps the write working. Applying **isp_render = 2** (the copy job's value)
  stops it.
- **Hybrid on the copy stream:** zeroing bgnd0/1/3 + mtile, setting isp_render = 3, writing a pattern to
  the clear register and **zeroing isp_bgobjvals** makes the copy stream write (hybrid masks 55/63
  pass); every mask that leaves **isp_bgobjvals = 0x300** fails, even with all background words zeroed.

So the copy job's write path is gated by the **background-tag object** (`enablebgtag` in
isp_bgobjvals): with the tag enabled the hardware needs a correctly initialised background shader
context, and the replay's captured kickusc/texturedata/sizes are not sufficient. That is the piece to
reproduce next - compare with the native driver's own working background-object setup
(`pvr_csb_pds_bgnd_words` + the draw path's ISP_CTL/AA, shared-register and USC-temps state) and mirror
the missing state in the replay.

## Round-153 (2026-10-30): bg-tag path needs the background data programs byte-exact

Follow-up analysis of the round-152 finding (copy write gated by `isp_bgobjvals` enablebgtag+mask):

- The native driver's own working background-object setup writes `isp_bgobjdepth = 1.0f`,
  `isp_bgobjvals = 0x3ff` (enablebgtag=1, mask=1, stencil=0xff) and the PDS BGRND words from
  `pvr_csb_pds_bgnd_words()`. The transfer stream instead carries `isp_bgobjvals = 0x300` (enablebgtag +
  mask, **no** stencil) and - as far as the kernel's transfer stream definition goes - **no bgobjdepth
  field at all**.
- Combinatorial re-tests: setting only `isp_bgobjvals` to 0/0x100/0x200/0x300/0x3ff on the copy stream
  never makes it write; the configurations that write are exactly the ones that **also** zero bgnd1 and
  bgnd3 (round-152 masks 55/63). So it is not one bit: with the tag enabled the hardware needs the
  background **data programs** (bgnd1 texturedata at PDS+0x2640 and the bgnd3 size/UNIFORM state) to be
  valid and consistent with the kickusc program at 0x41000; our reconstructed values are not.

Next step: capture those pieces exactly from one reference copy run - hook
`pvr_pds_generate_pixel_shader_sa_texture_state_data` (the texturedata/varying program content and its
PDS offset) plus `pvr_pds_unitex` sizes in the same run as the stream, and install them byte-for-byte in
the replay. That is the last missing input; the fill control proves the rest of the replay path.

## Round-154 (2026-10-30): copy DMA-state restored + its address encoding verified

Found that the replay had been installing the **fill** job's DMA space words (0xdc0007ff / 0x2400) since
an earlier revert; the copy job's captured content is 0xec0007ff / 0x22400. The captured address
encodings check out numerically:

- word 3 low 22 bits = **source address >> 18** (base run: 0x80000b7000 -> 0x200002; +0x800000 shifted
  run: 0x80008b7000 -> 0x200022, both matching the captured words); bit 28 is a flag.
- word 9 moves by **destination delta << 3** (copy vs fill differ by 0x20000 for a 0x4000 address
  difference).

The replay now uses the copy variant matching its mapped source (0x80000b7000) and destination
(0x80000bb000). The job still submits and syncs without writing, so the remaining piece is still the
background-tag execution context (rounds 152/153) - this round just removed a wrong input from the
experiment. Patch 0357, probe-only.

## Round-155 (2026-10-30): same-run capture of the copy job's supporting data

Captured stream + texture-state data + DMA spaces + kickusc programs in **one** `vktq_copy` run
(`captures/tq-ref/bundle2/`): the copy job's texturedata block at PDS+0x2640 is exactly
`[0x0004b080, 0x00000080, 0x90000014, 0]` (DMA space address 0x800004b080 split into words 0/1), the
kickusc programs are at PDS+0x41000/0x42000 with data `[0x7340, 8, ...]`/`[0x73c0, 8, ...]` and code
`f4000002`, and the second DMA space (0x800004b100) carries the fill-variant TEXSTATE. Those are
byte-identical to what the replay installs (rounds 152-154), so no new input difference was found.

A useful clue for the next attempt: the copy job's DMA space (0x800004b080) is written by the branch
that our `PVR_DUMP_DMA1` hook in `pvr_3d_copy_blit_core` did **not** reach, while the fill-side branch
(0x800004b100) is the one that runs - i.e. the transfer path's pass structure differs from what the hook
placement assumed. Instrumenting both passes explicitly (or dumping every general-heap BO written in the
transfer submit) will show whether the copy job's TEXSTATE is regenerated per pass; that plus the
background-tag context is the remaining work. All capture hooks reverted; tree clean at patch 0357.

## Round-157 (2026-10-30): the command-buffer BO list mixes two struct types (replay instrumentation note)

Tried to instrument every general/PDS BO written during transfer submit. The hook produced no entries
because `pvr_cmd_buffer::bo_list` is **not homogeneous**: some sites link a `struct pvr_suballoc_bo`
(`pds_upload_out->pvr_bo->link`, clear/descriptor BOs) and others link a plain `struct pvr_bo`
(`pvr_bo->link`). Both have `link` at offset 0, so a single `list_for_each_entry` cannot safely classify
entries, and the earlier "no BOs found" result was a type-cast artefact, not an empty list. The dump was
reverted.

This explains why the copy job's DMA space (0x800004b080) is written by neither of the two
`pvr_write_usc_constants` sites the hooks were placed at: the observed writes come from code paths whose
buffer was allocated through the mixed list, and the exact pass structure still needs a per-site hook
rather than a list walk. Next clean approach: hook `pvr_arch_cmd_buffer_alloc_mem()` to record
(heap, offset, size) of every transfer-path allocation, then dump each recorded buffer at submit time by
address (no reliance on the mixed list).

## Round-158 (2026-10-30): every copy-job buffer captured in one run - and the replay already matches

Added a small registry to `pvr_arch_cmd_buffer_alloc_mem()` (records heap address, size, CPU map for each
transfer-path allocation) and dumped it at submit time. One `vktq_copy` run then yields **all** the
copy job's buffers with their content (checked in as `captures/tq-ref/bundle3/`):

- copy DMA space `0x800004b080` (20 dwords) = the copy-variant TEXSTATE (`ec0007ff`, `0x22400`);
- copy texturedata at PDS `0xda00002640` = `[0x0004b080, 0x00000080, 0x90000014, 0]`;
- transfer-frag IPF control stream at `0xe400000000` (80 dwords);
- a second PDS block at `0xda000026c0` (`03040000 80000000 0 0 f4000004 ...`);
- the fill job's DMA space `0x800004b100` and texturedata for comparison.

Comparing these with what the probe replay installs: **the DMA space content, texturedata block, stream
and program bytes are already byte-identical**. Since the job still submits+syncs without writing, the
remaining difference is not in any buffer or stream the reference writes - it must be firmware-visible
state that is not in these buffers (the TQ shader-store/USC cache binding of the kickusc kernel, or the
background-tag depth/tag context). That narrows the search to the only remaining category; all buffer
hooks are reverted and the dumps are checked in.

## Round-159 (2026-10-30): crash guards, the bg-tag answer, and fragment temp counts

### 1. Crash guard (all four resets were the same vendor NULL-deref)
`pvr_screen_create()` and `pvr-probe` now call `drmGetVersion()` and **refuse to run unless the DRM
driver name is `powervr`**; a vendor-bound board gets a clear error instead of crashing the kernel
(verified: probe on pvrsrvkm prints the refusal, 0 faults). `bench31.sh` vendor arm fixed as well: it no
longer passes the Mesa-only `MESA_LOADER_DRIVER_OVERRIDE=pvr` / `LIBGL_DRIVERS_PATH` variables (they could
load the Mesa pvr_dri while pvrsrvkm was bound - the same crash), the script aborts if
`/usr/local/lib/dri/pvr_dri.so` is a Mesa driver, and both arms verify the stack from
`GL_VERSION` (`Mesa` required for native/zink, forbidden for vendor).

### 2. What does the tag-off copy write - src or constant?  CONSTANT ZEROS
Copy replay with the background tag disabled (`isp_bgobjvals = 0`, clear register left at the stream's
0x0) writes **0x00000000** to every destination dword (first 16: all zero). With the clear register
patched to 0xdeadbeef the same configuration writes 0xdeadbeef - i.e. the write is the pixel-event/fill
path clearing with the clear value, **not the TQ shader copying the source**. Per the decision tree: the
TQ shader never produces output, so the next thing to examine is the **kickusc program (PDS+0x41000) ->
USC task 0x7340 binding**, not the bg-tag depth context.

### 3. Fragment shader temp counts (`PCO_DEBUG_PRINT=fs,stats`, cache disabled, same board state)
glmark2 off-screen, temps per fragment IR (max over the scene's shaders):

| scene | FPS | FS temps |
|---|---|---|
| shading | 948 | 0, 5, 0 (max 5) |
| terrain | 20 | 47, 34, 19, 16, 16, 14, 14, 11, 11, 9, 8, 8, 8, 4, 4, 0 |
| refract | 38 | 23, 19, 5, 0 (max 23) |
| shadow | 389 | 11, 6, 5, 0 (max 11) |

terrain and refract are exactly the scenes with large FS temp counts and the largest vendor gap (0.51x /
0.50x), which matches the "temp count is the lever" finding; logs checked in under
`captures/pco-temps/`. Next measurement after any temp-reduction work: same four scenes.

### 4. Baselines
Per discussion: `bench30-results/` on disk is from the Oct 29 10:10/11:10 reruns (zink) and is not a
native baseline; **bench31 is the baseline** (native 806/806 vs zink 775/777 vs vendor 1226/1196).

## Round-160 (2026-10-31): bench31 vendor arm fixed, unambiguous TQ diagnosis, desktop temps

### 1. bench31.sh vendor arm (fixed)
`/usr/local/lib/dri/pvr_dri.so` is **IMG's own DRI driver (Mesa 24.0.1 fork)**, so the "is a Mesa
driver" pre-check was wrong and always fired - and because it fired after display-manager stop/Xorg
start it skipped the restore. Fixes:
- removed the strings pre-check and its (incorrect) reset-attribution comment;
- restored the original line-31 env for the vendor arm (`MESA_LOADER_DRIVER_OVERRIDE=pvr`,
  `LIBGL_DRIVERS_PATH=/usr/local/lib/dri`) - that is what selects the IMG DRI driver;
- **restore moved into a trap on EXIT** (kill Xorg :1, `$SW vendor`, AllowedCPUs reset, gov schedutil,
  start display-manager, kwin check/restart) so any exit - including stack errors - restores the board;
- stack checks by identity, not by "Mesa" presence: native `GL_RENDERER` must contain
  `(36.56.104.183)`, zink `GL_RENDERER` must contain `zink`, vendor `GL_VERSION` must contain
  `build 24.2`; the vendor no-Mesa check stays.

Vendor smoke (one scene, `build:use-vbo=false`, same env as the vendor arm): score **1298**
(baseline runs: vendor-1 **1464**, vendor-2 **1282** - within noise), `GL_RENDERER: PowerVR B-Series
BXM-4-64`, `GL_VERSION: OpenGL ES 3.2 build 24.2@6603887`, 0 "Mesa" hits, 0 faults. Restore verified
(dm active, kwin alive). So the vendor arm is usable again.

### 2. TQ diagnosis with distinguishable values
Probe now uses source `0xdeadbeef`, clear register `0x0badf00d`, destination prefilled `0x55555555`,
and counts each value; the kernel on the USC launch address can be swapped (`PVR_PROBE_TQ_COPY_KERNEL`).
Four cases (tag ON = raw stream `isp_bgobjvals=0x300`; tag OFF = zeroed backgrounds + tag,
`isp_render=3`), faults checked after each:

| case | dst content | meaning |
|---|---|---|
| tag ON + fill kernel | all `0x55555555` | no store |
| tag ON + copy kernel | all `0x55555555` | no store |
| tag OFF + fill kernel | all `0x0badf00d` | store works; fill kernel launched |
| tag OFF + copy kernel | all `0x55555555` | copy kernel never stores |

Per the decision tree: **the tag-on launch (or bg-object state) is broken** - the destination
address and the PBE store work (tag-off fill proves it), but nothing runs under the tag. The
**kickusc program (PDS+0x41000) -> USC task 0x7340 binding is a real lead**; CPU-twiddle fallback
decision stays on hold until that is investigated.

### 3. Desktop blur/shadow temps (the two biggest gaps)
Same env/options as the default glmark2 runs (`windows=4`, cache disabled, off-screen):

| scene | native fps | vendor fps | FS temps |
|---|---|---|---|
| desktop blur (radius=5, 1 pass, separable, 4 windows) | 120 | 328 | 4, 16, 6, 6, 15, 15, 16, ... (max 16) |
| desktop shadow (4 windows) | 456 | 1102 | 4, 16, 16 (max 16) |

CPU is ~10% (user 0.51 s + sys 0.50 s over 10.2 s wall) - **GPU-bound**, and the fragment temp counts
are low. So temps do **not** explain the two largest gaps; the perf plan needs another lead. The
pattern (4 windows x multi-pass desktop effects) points at per-small-render-pass GPU cost again
(load/store passes, framebuffer/target switches), not ALU.

## Round-161 (2026-10-31): the copy replay *hard-faults* in firmware - the missing state is the DMA/texture address base

### 1. The replay was launching the wrong TQ shader (fixed in the probe)
PDS/USC dumps from one reference `vktq_copy` run (temporary hooks in `src/imagination/vulkan`, reverted)
show the reference compiles **two** TQ fragment shaders per run:

| USC offset | size | temps | launched by |
|---|---|---|---|
| `0x7340` | 16 words | 2 | background-object kickusc program at PDS `0x41000` (stream `pds_bgnd0_base`) |
| `0x73c0` | 20 words | 4 | IPF-control-stream kickusc program at PDS `0x42000` (`state->pds_shader_task_offset`) |

The captured stream's `pds_bgnd0_base` is `[0x41000, 0x1e40]` but the IPF control stream's primitive
block names `0x42000`, whose captured data word is `0x73c0` - and the probe had been forcing **both**
kickusc programs to `0x7340`, i.e. it always launched the 16-word background variant with the wrong
temps. Also captured byte-exact and now installed:

* the **IPF control stream** (region array + primitive block, 78 dwords) at the transfer-fragment heap
  base `0xe400000000` (the stream's `isp_mtile_base` is its heap offset, 0); the replay never mapped it;
* the reference's texture-unicode program at PDS `0x1e40`: two **zero** words (the probe had guessed a
  DMA program there);
* the texture-state DMA program at PDS `0x2700` (`state->tex_state_data_offset`, named by the control
  stream's TEXTUREDATABASE field) - `[0x0004b080, 0x00000080, 0x90000014, 0]`;
* the PDS block named at `0x26c0` (`pvr_isp_prim_block_pds_state`'s varying base) and both kickusc
  program buffers (`data = [task, 8]` + `f4000002` code, 0x24-byte BOs).

Probe patch 0361: `PVR_PROBE_TQ_COPY_CS` installs the control stream + `0x26c0`, the 20-word shader is
installed at `0x73c0`, kickusc `0x42000` names `0x73c0`, and `PVR_PROBE_TQ_COPY_PRE`/`PRECTL` replay a
second captured stream through the same context with a distinguishable clear value to test whether an
earlier kick establishes firmware context state.

### 2. Result: still no store, but now we know *why* - the job faults
With every captured buffer, the stream and all programs byte-identical the copy still writes nothing
(tag ON: destination stays poison). A second experiment ruled out firmware per-context state: replaying
the fill stream first (which does write its clear value, verified distinguishable via `PRECTL`) and the
copy stream second still leaves the destination at the pre-job's value.

What changed: the kernel logged `Received unknown FWCCB command 2abc0069` for **copy** replays only
(never for the fill replay, never for reference runs). `0x2abc0069` is `ROGUE_CMD_MAGIC_DWORD (0x2ABC)
<< 16 | 105` = `ROGUE_FWIF_FWCCB_CMD_CONTEXT_RESET_NOTIFICATION`: the firmware was reporting a context
reset that this kernel version does not decode (it only handles freelist commands), so the failure was
silent. A temporary kernel patch (`tools/verify/kernel-ctxreset-decode.patch`, decode only - no
behaviour change) turns it into:

```
CTXRESET ctx=0 reason=1 dm=3 job=1 flags=0x1 pc=0x112310000 fault=0xe00004b080
```

* `reason=1` = `ROGUE_CONTEXT_RESET_REASON_GUILTY_LOCKUP` (the ISP data master locked up),
* `dm=3` = the ISP/3D data master, `flags=0x1` = page-fault flag,
* **`fault=0xe00004b080` = USC heap base (`0xe000000000`) + `0x4b080`** - and `0x4b080` is the low
  dword of the shader's texture-state DMA source address `0x800004b080` (general heap).

So under the tag-enabled/shader path the ISP's PDS DMA resolves the captured DMA source address with
the **USC-heap base as its upper part** instead of the captured address-high word (`0x80` for
`0x8000000000`). The fill/clear path never executes that DMA, which is why it works.

### 3. Next step (precise)
Find the state that selects the PDS-DMA address base: the transfer job's DMA burst program is generated
by `pvr_pds_encode_dma_burst()` and encodes the full 64-bit address as two dwords, so either the
program is entered at a different offset, or the ISP/PDS DMA uses a base register that the reference
sets up and the replay does not (the native 3D draw path never has to DMA from the general heap into USC
shared registers, so this is untested state in our driver too). Practical order: (1) dump the fw-visible
`rogue_fwif_cmd_transfer` from both paths with a temporary kernel print and diff; (2) if identical,
inspect how the transfer context's PDS/DMA base is programmed (fw trace mask is available on the module);
(3) only then decide TQ vs the CPU-twiddle upload fallback.

Evidence: `captures/tq-fault/{fw-context-reset.txt,ref-alloc-dump.log}`, `tools/verify/kernel-ctxreset-decode.patch`.
Board restored to vendor+desktop after the session; 0 faults, 0 PVR_K errors.

## Round-162 (2026-10-31): the replay's firmware command is byte-identical - the fault is in how a PDS program's first word is consumed

### 1. The last job-description difference was the single-core flag (fixed)
A temporary kernel print of the firmware-visible `rogue_fwif_cmd_transfer` (patch
`tools/verify/kernel-tqcmd-dump.patch`, decode only) for the reference `vktq_copy` run and the native
replay shows both commands are **byte-identical except two dwords**: the clear register (deliberately
patched by the probe) and `cmd->flags`. The reference submits `cmd->flags = 2` =
`ROGUE_TRANSFER_FLAGS_SINGLE_CORE` (userspace sets `use_single_core` because the device has the
multicore feature - BXM-4-64 reports multicore support and the transfer stream's last dword is the
`frag_screen` field, not `isp_xtp_pipe_enable`), while the probe passed 0. After the probe started
passing `DRM_PVR_SUBMIT_JOB_TRANSFER_CMD_SINGLE_CORE` (probe 0362) the commands are identical modulo
the clear value - and the replay still faults in the ISP data master. So the job *description* is not
the problem; something the firmware/hardware reads from memory or from per-context state is.

### 2. The fault address tracks one PDS program's first dword
Varying the content of the two texture-DMA programs (new probe envs `PVR_PROBE_TQ_COPY_TEXDMA1/2/2H`
for PDS `0x2640`/`0x2700`, and `PVR_PROBE_TQ_COPY_CSDB/CSVAR` for the IPF control stream's
texture-database / varying fields) gives:

| experiment | firmware fault address |
|---|---|
| baseline (0x2700 word0 = `0x4b080`) | `0xe00004b080` |
| 0x2700 word0 = `0x4b100` | `0xe00004b100` |
| 0x2700 word0 = `0x0` | `0xe000000000` |
| 0x2640 word0 = `0x4b300`, 0x2700 = `0x4b200` | `0xe00004b200` |
| control-stream texture-database field = `0` | `0xda00000000` |
| control-stream texture-database field = `0x73c0` | `0xda000073c0` |

(`0xe000000000` = USC heap base, `0xda00000000` = PDS heap base.)

So the PDS program named by the control stream's texture-database field is consumed with its **first
dword taken as an address in the USC (or, for the control stream field, PDS) heap**. The reference's
control stream names PDS `0x2700`, whose captured content is a DMA-burst program
`[0x4b100, 0x80, 0x90000014, 0]` (address `0x800004b100` + control); if the reference consumed it the
same way it would fault at `0xe00004b100` too, which it does not.

### 3. What that means for the next attempt
Either (a) the reference's per-context state makes the hardware consume that program as a **DMA
descriptor** rather than as a kick/address word - the state that selects it is not in the job command -
or (b) the captured control stream/content pair is not the exact one the reference job executes (the
copy blit builds two texture-data programs in one prep, `0x2640` and `0x2700`, and only one control
stream dump was seen; a same-run capture of *both* control streams with per-pass tags would settle
it). Cheapest decisive next probe: make the userspace driver log the exact (pass index, control-stream
device address, texture-data offset) tuple at submit for the reference, and replay the *matching*
pass's control stream; if the replay then still faults, the remaining difference is per-context
firmware state and the TQ path needs the vendor-style "transfer context init" job.

Evidence: `captures/tq-fault/tqcmd-{ref,probe3}.txt` (firmware commands, dword-indexed) and
`tools/verify/kernel-tqcmd-dump.patch`. Mesa probe 0362, board restored to vendor+desktop.

Note on the local module tree: the context-reset decode from round-161 (`tools/verify/kernel-ctxreset-decode.patch`)
is **kept applied** in `/home/radxa/kernel-src/powervr/pvr_ccb.c` (it only prints a decoded
`CTXRESET ...` line for firmware reset notifications the mainline handler would otherwise ignore);
the round-162 `pvr_job.c` firmware-command dump was reverted and the module rebuilt from clean sources
plus that decode.

## Round-163 (2026-10-31): coherent single-run snapshot - the replay's inputs are now provably identical, the fault is per-context firmware state

### 1. Root cause of the contradictory bisection results: mixed runs
The PDS sub-allocator hands out offsets in allocation order, so a stream captured in one process run
does not necessarily name the same blocks as an allocation dump from another run. Every earlier
"replay installs byte-identical bytes" comparison mixed at least two runs; the lag in the firmware's
context-reset notification (it is delivered when a *later* submission processes the CCB, not with the
faulting job) then made the "what does the fault address track" bisection look consistent when it was
not.

### 2. One-run capture of the whole transfer path
A new temporary capture hook in the reference driver (registry of every mapped GPU block: PDS/USC
uploads, sub-allocator blocks, per-heap `pvr_bo_alloc` buffers, keyed by device address and dumped with
the firmware streams at transfer submit; `PVR_DUMP_ARENA`) gives, for a single `vktq_copy` run, both
firmware streams and all 97 mapped blocks. Checked in as
`captures/tq-fault/single-run-{streams.bin,blocks.txt}`. Facts from that snapshot:

* the copy stream is **byte-identical** to the bundle capture used by the replay;
* the control stream at `0xe400000000`, both TQ shaders (USC `0x7340` 16 words, `0x73c0` 20 words), both
  kickusc programs (`PDS 0x41000 -> 0x7340`, `0x42000 -> 0x73c0`), the pixel-event data/code at
  `0x2680`/`0x26a0`, the texture-unicode code at `0x1e40`, the varying base program at `0x26c0`, and the
  first texture-state DMA program at `0x2640` (`0x800004b080`) are **byte-identical** to the replay;
* the replay was wrong in exactly two inputs, now fixed in probe 0363:
  1. PDS `0x2700` (the control stream's `TEXTUREDATABASE`) holds a **second texture-state DMA program**
     whose source is `0x800004b100` (not `0x4b080`);
  2. that second texture-state DMA **space at `0x800004b100`** was never mapped by the replay;
* the replay's fill job pixel-event program was also wrong (task is the EOT shader at USC `0x5d40`, not
  `0x5c00`, and it names the source buffer as its write destination) - fixed.

### 3. Result: still a firmware context reset, now with all inputs identical
With both fixes the replay maps and installs every block of the coherent snapshot, and the firmware
command is byte-identical (round-162), yet the copy still ends in
`CTXRESET reason=1 (GUILTY_LOCKUP) dm=3 flags=0x1`: the ISP data master page-faults at
`USC heap base + <first dword of a texture-state DMA program>` - i.e. those programs' first word is
consumed as a USC-space address. Varying the first word moves the fault address, and pointing it into
the mapped USC page (e.g. `0x73c0`) moves the fault to the *other* texture-state program, so both are
consumed the same way. The reference consumes identical bytes without a fault.

The only remaining difference is therefore **firmware-visible per-context state** (the job command and
all job memory are now proven identical), or the kernel-side `rogue_fwif_fwtransfercontext`. Next probe:
dump that firmware context object from `pvr_context.c` for the reference and the replay and diff it;
if identical, the difference is in how the kernel's transfer queue object is set up for the first kick
and the next step is a per-context initialisation job in the same style as the 3D static context state.

Evidence: `captures/tq-fault/single-run-{streams.bin,blocks.txt}`; Mesa probe 0363.

## Round-164 (2026-10-31): the firmware static programs were only placed by the draw path - fixed; TQ fault unchanged

### 1. Real bug found while chasing the TQ fault
The driver reads the VDM sync launcher, the pixel-event (EOT) program and the YUV CSC table from
PDS/USC/general heap **carveouts**. `pvr_static_memory_init()` was only called from `draw_init()`, so
any job that never creates a draw context - e.g. the probe's transfer replay, and any future
transfer-only upload path - ran with those areas **empty**. The probe's own VM check had already
documented the failure mode: the firmware then faults with the fault address exactly at the PDS heap
base, and the isolation tests in round-163 (zeroing a background-object pointer produced
`fault=0xda00000000`) match that signature.

Fixed (patch 0364): `pvr_screen_create()` places the static programs once and keeps them for the life
of the screen; `draw_init()` and the probe's VM check reuse that placement instead of allocating a
second carveout. The 17-group gate passes (`ALL GROUPS PASS`) - draws are unchanged (same programs, same
addresses).

Reference static-area contents were also captured for comparison (temporary hook in
`pvr_winsys_helper_fill_static_memory`): PDS EOT program = a single `f4000002` instruction at the
kernel's `eot` offset, PDS VDM-sync launcher = `f4000002` in its code slot with the task/temps data in
front, USC VDM sync = `0580a044 ff000000`, YUV CSC table = `1ffc0000 ...`. The native driver generates
the same shape for all four, so the static programs are not where the remaining TQ difference is.

### 2. TQ copy replay: unchanged by the fix
With the static programs now present the replay still ends in
`CTXRESET reason=1 (GUILTY_LOCKUP) dm=3 flags=0x1 fault=0xe00004b100` - the ISP data master faults on
the first word of the PDS `0x2700` texture-state DMA program resolved against the USC heap base, with
all job inputs (stream, control stream, shaders, DMA spaces) verified byte-identical in round-163. So
the remaining difference is firmware-visible per-context state or the hardware's DMA address-base
selection for the transfer path, not the static programs.

Next (unchanged from round-163, now with the static areas ruled out): dump the kernel's
`rogue_fwif_fwtransfercontext` for both paths and diff; if identical, compare the two processes'
first-kick timing (the reference submits its clear pass first on the same context).

## Round-165 (2026-10-31): the firmware transfer-context object is identical, and the texture-data slot is consumed as a *kick* in the replay

### 1. Kernel-side context comparison: no difference
A temporary kernel print of the firmware context object for `DRM_PVR_CTX_TYPE_TRANSFER_FRAG`
(`pvr_context.c`, `FWCTX` lines, reverted) shows both paths use the same 152-byte
`rogue_fwif_fwtransfercontext` with identical values except the per-process fields: the two internal
device addresses (CCCB control/CCCB, context state) and the PID (`0xe442f` vs `0xe4513`). Every
firmware-visible field the job depends on (DM, priority, ccb addresses, mem-context address,
context-state address, server context id) is structurally identical. So the kernel-side context is not
the difference.

### 2. The pre-job really runs - the copy still faults
With the corrected fill pixel-event program a pre-job (the reference's clear pass, stream 0) now
verifiably executes: its clear value lands in the source buffer (4096/4096 dwords, made
distinguishable with `PVR_PROBE_TQ_COPY_PRECTL`). Submitting the copy stream after it still ends in a
firmware context reset. So job ordering/preceding-kick state is not the difference either.

### 3. The texture-data slots are consumed as USC kicks in the replay
Setting both texture-state DMA programs' first word to a valid USC task address
(`PVR_PROBE_TQ_COPY_TEXDMA1/2 = 0x73c0`, the 20-word copy shader) **removes the page fault** and turns
it into a lockup reset (`flags=0x0 pc=0x0 fault=0x0`) - i.e. the firmware does launch the USC task at
that word, the shader starts and hangs because the texture-state DMA never happened. With the captured
DMA-burst content the same word is instead used as an address (`fault = USC heap base + word0`).

So the hardware *can* consume those slots as kicks, while the reference - with byte-identical program
bytes - consumes them as DMA descriptors. The selector is therefore in a register whose value we have
not varied: the candidates are the background-object size info (`pds_bgnd3_sizeinfo`:
usc_sharedsize / pds_texturestatesize / pds_tempsize), `isp_ctl`/`isp_aa`, and the control stream's
`TA_STATE_PDS_SIZEINFO1/2`. Next experiment: sweep those three fields (they are already overridable for
the stream; the control-stream words 56 and 60 need small additions), using the *3D draw path's*
values as the reference for "DOUTD mode works" - the draws set the same kind of texturedata program and
the hardware does treat it as a DMA descriptor there.

Note: the round-163/164 fault-address bisection is partly confounded - the probe's static firmware
areas were unmapped then (fixed in round-164), so some of those faults were "firmware executed from
unmapped static memory" rather than the transfer job itself.

## Round-166 (2026-10-31): transfer-state selector sweep (negative), performance re-baseline and a ruled-out perf lever

### 1. TQ selector sweep: no selector among the remaining registers
With all job data byte-identical (round-163) and the static firmware programs now in place
(round-164), the candidate state that could make the firmware consume the texture-state program as a
DMA descriptor rather than a kick was swept through new probe overrides: stream `isp_ctl`, `isp_aa`,
background size info (`pds_bgnd3_sizeinfo`, both halves), and the control stream's primitive-block
`SIZEINFO1` (word 56) and `SIZEINFO2` (word 60). Result:

| override | firmware fault |
|---|---|
| `isp_ctl` = 0x1f000 / `isp_aa` = 0x2500 | unchanged `0xe00004b080` |
| control stream SIZEINFO1 = 0 / SIZEINFO2 = 0 / both | unchanged `0xe00004b080` |
| background size info = 0 | `0x10000` (decode changes, still a fault) |

So the size info participates in address decoding but is not the DMA-vs-kick selector. The TQ path
stays blocked; the honest conclusion after rounds 161-166 is that the remaining difference is outside
the userspace-visible job state (command, all mapped memory, static programs and kernel context object
all verified identical or structurally identical), i.e. firmware behaviour we cannot reach from the
userspace side with these probes.

### 2. Performance re-baseline (bench31, native + zink)
`bench31.sh` on the current driver (patches 0361-0365): **native 794 / 805** (previous baseline
806/806), **zink 759** (previous 775/777). The native delta is within run-to-run noise (~1.5%); the
static-memory move from draw-context creation to screen creation is perf-neutral for glmark2 (one
context per process). The run hit the 20-minute cap during the vendor arm, so only the two open-stack
arms are recorded; future runs need a longer cap or per-arm invocation.

### 3. Ruled out: fixed-function blending
The largest scene gaps are blend-heavy (desktop blur/shadow 0.37x/0.41x, cel/refract/terrain ~0.5x)
while opaque scenes are near vendor (0.76-0.95x), which suggested our shader-lowered blending
(`nir_lower_blend` + destination load) was leaving a fixed-function PBE blend unit unused. The
generated csbgen headers for this GPU contain **no blend-function registers at all** (no `blend`
symbol in any rogue packet header) - on this hardware blending is a shader operation, which is what the
driver already does. That lever does not exist; the remaining perf levers are the known FP16 ALU gap
(F16SOP encodings missing in pco) and per-job fixed cost for many-small-draw scenes (to be measured).

## Round-167 (2026-10-31): per-scene three-way table; terrain/refract are the only scenes where zink beats native

### 1. Per-scene comparison (bench31-results, patches 0361-0365, native-1 / zink-1 / vendor-1)
FPS per glmark2 scene, and the native ratios:

| scene | native | zink | vendor | native/zink | native/vendor |
|---|---|---|---|---|---|
| build use-vbo=false | 1151 | 1030 | 1464 | 1.12 | 0.79 |
| build use-vbo=true | 1260 | 1192 | 1746 | 1.06 | 0.72 |
| texture nearest | 1412 | 1252 | 1557 | 1.13 | 0.91 |
| texture linear | 1367 | 1198 | 1534 | 1.14 | 0.89 |
| texture mipmap | 1414 | 1371 | 1523 | 1.03 | 0.93 |
| shading gouraud | 938 | 875 | 1417 | 1.07 | 0.66 |
| shading blinn-phong-inf | 770 | 717 | 1326 | 1.07 | 0.58 |
| shading phong | 652 | 624 | 1189 | 1.04 | 0.55 |
| shading cel | 529 | 533 | 1177 | 0.99 | 0.45 |
| bump high-poly | 409 | 367 | 661 | 1.11 | 0.62 |
| bump normals | 1368 | 1283 | 2024 | 1.07 | 0.68 |
| bump height | 1005 | 950 | 1736 | 1.06 | 0.58 |
| effect2d kernel laplacian | 761 | 652 | 1210 | 1.17 | 0.63 |
| effect2d kernel box | 339 | 317 | 490 | 1.07 | 0.69 |
| ideas speed=duration | 1073 | 810 | 1808 | 1.32 | 0.59 |
| jellyfish | 388 | 354 | 522 | 1.10 | 0.74 |
| terrain | 21 | 27 | 40 | **0.78** | 0.53 |
| shadow | 385 | 365 | 692 | 1.05 | 0.56 |
| refract | 38 | 47 | 75 | **0.81** | 0.51 |

So the native driver beats zink nearly everywhere (1.03-1.32x) - the exceptions are **terrain and
refract**, both on the *same* GPU driver underneath zink. That makes them the most promising perf
targets: a win there is a native-driver win, not a stack artefact.

### 2. Desktop scene scaling and shader cost
`desktop:windows=1` = 289 fps, `windows=4` = 121 fps on native (zink at windows=4: 137 fps): the
per-window cost is ~1.6 ms and the frame-fixed cost ~1.9 ms, so the default 4-window scene is
dominated by the per-window multi-pass work, not by one-off frame setup.

Fragment shader temp counts (`PCO_DEBUG_PRINT=fs,stats`, cache disabled):

| scene | native FS temps (max) | zink FS temps (max) |
|---|---|---|
| terrain | 47 | 40 |
| refract | 23 | 23 |

Refract has identical temps in both stacks yet native is 24% slower, so its gap is not fragment
register pressure (look at the native RTT/pass structure: load/store of the render target, FBO format
handling). Terrain's native shader carries 7 more temps than zink's for the same GLSL, which fits its
0.78x: find the extra ops in the native fragment lowering (blend/format path) and cut them.

### 3. FP16 ALU: where the vendor gap actually is
The shading/effect2d/cel/bump scenes (~0.45-0.69x vendor) are fragment-ALU-bound and show the FP16
gap. `pco_isa.py` already has the instruction group (`main_op` sngl, `sngl_op` = f16sop = 0b0100) but
the F16SOP operand bit-struct is a `# TODO`; encoding it needs a vendor-compiled USC sample, which is
the one artefact not captured yet (a vendor-stack LD_PRELOAD shim on the IMG DRI driver that dumps USC
uploads is the plausible next step).

## Round-168 (2026-10-31): the driver CPU-waits behind every render - measured cost up to +34%

### 1. Where the time goes in RTT/multi-pass scenes
`draw_slot_idle()` is called whenever a render slot must be reused or a resource that a slot rendered is
read again, and it blocks the CPU on that render's syncobj. Instrumenting it (temporary, reverted)
shows how much wall time that costs in glmark2 (each scene runs ~10 s):

| scene | fps | CPU time blocked waiting | waits |
|---|---|---|---|
| refract | 37 | 9786 ms | 5926 |
| terrain | 20 | 9678 ms | 13917 |
| shadow | 388 | 9118 ms | 15518 |
| effect2d | 1610 | 9076 ms | 32203 |

i.e. the CPU is blocked for ~90-98% of the scene duration - it never runs ahead, so the next render's
geometry (TA) submission cannot overlap the current render's fragment (3D) execution across passes.

### 2. Ceiling measurement
Temporarily skipping the waits (`PVR_NO_WAIT`, unsafe, reverted) gives: **shadow 388 -> 519 fps
(+34%)**, **desktop 121 -> 139 (+15%)**, effect2d unchanged, while refract/terrain collapse (8/3 fps)
because ignoring the hazards makes the GPU stall on overlapping writes. So the serialisation is worth
roughly 15-35% on multi-render scenes, and a *correct* fix (GPU-side dependency waits in the next
submission instead of CPU waits, with enough slots in flight) is the highest-value driver optimisation
found so far.

### 3. Terrain shader (native vs zink, same GPU driver under zink)
pco binary stats for the terrain scene's main fragment shader:
native **10512 bytes, 47 temps, 739 instruction groups**; zink **10328 bytes, 33 temps, 694 groups**.
Same code size but 42% more temps on native, which is a register-allocation outcome (no spills in
either). `PCO_DEBUG=NO_DUAL_ISSUE` / `RA_SKIP_OPT` do not change terrain's 20 fps. Refract is the
opposite: native 1104 bytes/23 temps vs zink 2592/23, i.e. native code is *smaller* and equally
register-pressured yet 24% slower - so refract's gap is structural (pass/sync), consistent with §1.

## Round-169 (2026-10-31): CPU-wait theory disproven - the driver is GPU-work-bound

Round-168 measured the CPU blocked ~9.1-9.8 s of every ~10 s scene on render syncobjs and a
skip-all-waits ceiling of +34% on shadow / +15% on desktop. Three follow-up experiments show that
number was an artefact and the waits are not on the critical path:

1. **Wait-site accounting** (temporary, reverted): the dominant site is the HWRT data-set wait - "wait
   for the render that previously used this RTA data index" (desktop 9326 ms/12057 calls, shadow
   9336/7784, terrain 9682/5198, refract 6839/1119); slot-reuse waits are ~1.5 ms in desktop/shadow
   but 2977 ms in refract (transient memory reuse, genuinely required).
2. **Skipping only the HWRT wait changes nothing**: desktop 120, shadow 389, refract 38, terrain 20,
   effect2d 1610 - identical to the baseline. The kernel/firmware already recycles the two RTA data
   sets through the handoff, and the user-space wait returns only when the GPU work would have been
   finished anyway, so it costs no GPU time.
3. **8 draw slots instead of 4** (reverted): no change on any scene - queue depth is not the limiter.
4. **Render-target load**: forcing the clear-background path (no target reload) changes nothing on
   opaque scenes (texture 1544->1533 fps, build 1335->1337), so the load is either already skipped
   for shaders that do not read the destination or is negligible.

So the earlier +34% came from skipping *all* waits (rendering became invalid and scenes drew much
less work); with correct rendering the driver is **GPU-work-bound**. The remaining levers are the ones
already identified: FP16 ALU (F16SOP encodings, the vendor's ~2x on shading/terrain/refract), the
native terrain fragment shader's 47 vs 33 temps, and the refract pass structure. Job/dependency
overhead is not worth further work.

## Round-170 (2026-10-31): F16SOP is publicly documented (semantics only); terrain gap is not the shader code

### 1. F16SOP: public ISA documentation found
IMG publishes the **PowerVR Instruction Set Reference** at
<https://docs.imgtec.com/reference-manuals/powervr-instruction-set-reference/html/> - the F16SOP page
(<https://docs.imgtec.com/reference-manuals/powervr-instruction-set-reference/html/topics/floating-point-instructions/F16SOP.html>)
documents the full instruction family:

* `F16SOP W0{.E0|.E1}.jOut, (W0|W1){.E0|.E1}.kOut, (W1|_), Sn{...}{.NEG}, (Sn|0){...}{.ONEMINUS}, jOp,
  Sn... , kOp, Sn..., (S3|_)` with `jOp` in {add, sub, min, max, rsub, mad}, `kOp` in
  {add, sub, min, max, rsub}: two half-precision sums of products packed into W0[31:16]/[15:0]
  (or converted to F32 when rfmt(1)=1), plus the `F16SOP.MAD`, `F16SOP.U8`, `F16SOP.U8MAD` variants and
  `SOPMOV`.
* The `sngl_op` code 0b0100 for F16SOP matches `pco_isa.py`/upstream `rogue_isa.h`, so the main-op and
  sub-op bits are already known; what is still missing is the **bit-level operand layout**, which the
  HTML reference does not include (the PDF at `cdn.imgtec.com/sdk-documentation/PowerVR+Instruction+
  Set+Reference.pdf` is access-denied from this host).
* Next step for F16SOP: get one vendor-compiled shader that uses it (IMG DRI driver on the vendor
  kernel stack - an `LD_PRELOAD` shim dumping its USC uploads) and diff the instruction words against
  a known `sop` sequence from the ISA reference; the main-op (3 bits), sngl_op (4 bits), destination
  and source maps are all known from `pco_isa.py`, so the search space is only the F16SOP-specific
  modifier/op-select fields.

### 2. Terrain: the shader code is the same in both stacks
NIR for the biggest terrain fragment shader (`PCO_DEBUG_PRINT=fs,nir`, cache disabled):
native 1904 SSA instructions, zink 1902 - nearly identical opcode mix (native 608 load_const /
397 ffma / 284 fmul / 195 fadd / 136 ffloor; zink 597 / 401 / 280 / 190 / 152). Yet native runs the
scene at 20 fps and zink (same pco compiler, same GPU, our Vulkan driver underneath) at 27 fps. So the
terrain gap is **not** the fragment shader translation; it is driver pipeline state that differs between
the two paths - the next thing to compare is the vertex shader (terrain is vertex-heavy) and the
per-draw pipeline registers (ISP tile config / `usc_pixel_output_ctrl` / depth setup).

## Round-171 (2026-10-31): the twiddled render-target path is the terrain/refract gap

### 1. Shader stages are not it
`PCO_DEBUG_PRINT=all,stats` for terrain: native's biggest VS is **1192 bytes / 22 temps / 88
instruction groups**; zink's is **8008 / 35 / 671** - seven times bigger, yet zink is faster (27 vs
20 fps). Same GPU, same pco. So neither the VS nor (round-170) the FS explains the native/zink gap.

### 2. Twiddled layout: mixed across scenes, and it is exactly the two gap scenes where linear wins
`PVR_NATIVE_DEBUG=no_twiddle` (linear render targets/textures) versus the default (twiddled):

| scene | default (twiddled) | no_twiddle | change |
|---|---|---|---|
| texture | 1466 | 1074 | -27% |
| build | 1330 | 1012 | -24% |
| shadow | 390 | 343 | -12% |
| desktop | 121 | 108 | -11% |
| effect2d | 1604 | 926 | -42% |
| jellyfish | 384 | 393 | +2% |
| **refract** | **37** | **45** | **+22%** |
| **terrain** | **20** | **24** | **+20%** |

Twiddled is the right layout for most scenes (as expected - it is the hardware's native layout), but
**refract and terrain - the only two scenes where zink beats native - are exactly the two where linear
beats twiddled in our driver**. Both scenes are render-to-texture-then-sample workloads (the scene
renders into a twiddled target and reads it back as a texture in the next pass), unlike the static-
texture scenes. So the native driver's *sampled twiddled render target* path is where the gap is, not
the shaders or the job/sync structure.

### 3. Next step
Compare, for a sampled twiddled render target, the native driver's texture descriptor and layout
arguments against the reference Vulkan driver's (`pvr_arch_tex_state.c` / `pvr_tex_state.c`):
twiddled/physical-width-stride handling, the TFBC compression-mode bits in the tex descriptor
(`ROGUE_TEXSTATE_COMPRESSION_MODE_FB_DIRECT_8X8` and its siblings), and the sampler state - plus the
render-target physical extents the driver pads to. `no_fbc` alone changes nothing (terrain 20, refract
38), so it is the twiddle layout/descriptor rather than the compression format itself.

## Round-172 (2026-10-31): the linear-mip speedup is invalid; terrain/refract gap still needs a pass-structure comparison

### 1. Splitting the twiddle knob: the gain comes from (linear) mip chains - but that path is broken
Round-171 showed `PVR_NATIVE_DEBUG=no_twiddle` helps exactly refract (+22%) and terrain (+20%). A split
knob (`no_twiddle_rt` = render targets only, `no_twiddle_mips` = GPU-uploaded mip chains only; exact
comma-token parsing, temporary, reverted) gives:

| configuration | terrain | refract |
|---|---|---|
| default (twiddled) | 20 | 38 |
| `no_twiddle_rt` | 20 | 37 |
| `no_twiddle_mips` | **26** | **48** |

So render targets are irrelevant and the whole gain is the **mip chain layout**. But
`pvr_resource_layout()` for the linear path allocates **level 0 only** (`stride * height`) and never
fills `level_offset[]`, so with linear mip chains every level aliases level 0 - the 26/48 fps numbers
are invalid rendering doing less work, not a real lever (same class of artefact as the round-168/169
wait experiment). Reverted.

### 2. glmark2 validation is not an oracle for this hardware
`glmark2 --validate`: native fails 27 of 27 scenes, zink fails 21 - both stacks fail, so the comparison
is not a correctness test across GPUs. (Worth knowing before using it to chase rendering bugs.)

### 3. What is left for terrain/refract
Both scenes have identical shader translations in native and zink (round-170/171) and the difference
must be in the *pass structure*: how many render passes/jobs each path generates per frame and how the
render target is set up per pass. Next measurement: count the render submissions per frame in the
native path (a counter in `pvr_job_submit_render`) and compare with what the Vulkan driver produces
for the same zink frame at the same draw count; also re-check the non-POT twiddled texture descriptor
(`draw_level_desc` passes the *logical* width/height for level 0 while the PBE words use the physical
ones - if the TPU needs the physical extents for twiddled addressing, non-POT twiddled sampling is both
wrong and slow, which is exactly the terrain/refract shape).

## Round-173 (2026-10-31): the native path renders terrain without a depth buffer - zink uses one

### 1. Kernel-level fragment-command comparison (temporary `pr_err` dump in `pvr_job.c`, reverted)
Dumping the firmware-visible configuration of the first 40 fragment jobs of the terrain scene
(`rogue_fwif_frag_regs`: flags, zlsctl/zload store bases, PBE words, ISP ctl, bgnd) shows:

* **zink** alternates two configurations: `flags=0x8000`, `zlsctl=0`, no depth load, and
  **`flags=0x8080`** (DEPTHBUFFER) with `depth=0xffffff`/`0xffff`, `zlsctl=0x1fa8a21fc`,
  `isp_zload_store_base=0x80013f6000`, `zstride=4194304` - i.e. terrain renders **with depth testing**
  (two different depth formats appear: 24-bit and 16-bit, likely scene depth plus a shadow pass).
* **native**: all 40 dumped fragment jobs are `flags=0x8000`, `zlsctl=0`, `load=0`, `zstride=0` -
  **no depth buffer at all**. Every other ISP field that is comparable (outctl 0x7ff8, bgobjvals 0x3ff,
  isp_ctl 0x80035000, aa 0, event-pixel info) is identical between the two paths.

So the native driver is drawing the terrain scene (and likely refract) **without depth testing**: it
shades every fragment, while zink's depth-tested render lets the ISP reject hidden fragments early.
That fits the two scenes where zink beats native (terrain 20 vs 27, refract 38 vs 47) - and it is a
correctness gap too, since those scenes are depth-tested by design.

### 2. Not the compute fallback
A temporary counter in `pvr_draw_vbo` shows **zero** compute-fallback draws in terrain, refract or
texture: every draw takes the TA/3D path. So the depth-less fragment jobs are the 3D path itself,
not a fallback.

### 3. Next step
Find why `ctx->fb.zsbuf` is NULL for the off-screen/default framebuffer in the native DRI path: the
EGL surface config reports `depth=32` (bench31 logs), and FBO-rendered depth works in the gate, so the
missing piece is the **surface/drawable depth buffer being exposed to the state tracker** (the
DRI/EGL winsys integration). Fix that, then re-measure terrain and refract; a depth-tested terrain
should both render correctly and recover the early-Z win.

## Round-174 (2026-10-31): the depth formats are supported - round-173's window mixed in the shadow pass

Follow-up to round-173's "native renders terrain without depth":

1. **The surface depth formats are supported.** A temporary `PVR_FB_LOG` print in
   `pvr_set_framebuffer_state` (reverted) shows the terrain scene binds
   `FBSET 800x600 cbufs=1 zs=z16_unorm` and `zs=z24x8_unorm` (392 times each), alongside
   `FBSET 256x256 cbufs=1 zs=NULL` (784 times - the shadow pass) and 196 depth-less 800x600 binds.
   Both z16_unorm and z24x8_unorm are handled by `draw_zls_format()` (16BITINT / 24BITINT), so the
   earlier "unknown depth format dropped" hypothesis is wrong.
2. **The kernel dump window was not the main pass.** Re-dumping the fragment configuration in a
   mid-scene window (jobs 600-639, still temporary, reverted) shows the same depth-less
   `flags=0x8000 zlsctl=0` jobs - but that window is dominated by the 256x256 shadow-pass renders
   (which legitimately have no depth). To settle it the dump must be correlated with the render
   target size; the fw command does not carry width/height, so the next dump should print the HWRT
   data-set geometry (`job->hwrt`) alongside the fragment registers.
3. Useful side finding for future kernel instrumentation: **`drm_info()` output did not appear in
   `dmesg` on this kernel** (whatever the loglevel), while `pr_err()` did - and the module rebuild
   only takes effect after the GL clients (weston/Xwayland) release the device, otherwise `insmod`
   silently leaves the old module in place.

## Round-175 (2026-10-31): round-173 retracted - native *does* depth-test, and terrain submits ~52 jobs/frame

A temporary `PVR_SUBMIT_LOG` print in the native driver right before each render submission
(`pvr_job_submit_render`, reverted) settles both open questions at once. For the terrain scene at
20 fps (10 s run, ~200 frames):

| render size | zs | draws | submissions | what it is |
|---|---|---|---|---|
| 800x600 | **yes** | 1 | 579 | the main terrain pass, `fragflags=0x2` (DEPTHBUFFER) |
| 800x600 | no | 1 | 193 | colour-only composite/clear passes |
| 256x256 | no | 1 | 771 | shadow pass |
| 400x300, 200x150, 100x75, 50x37, 25x18, 12x9, 6x4, 3x2 | no | 0 | 581 each | **mip generation blits** |
| 128x128 … 2x2 | no | 0 | 393 each | **mip generation blits** |
| 1x1 | no | 0 | 974 | mip generation blits |

So:

1. **Round-173's "native renders terrain without depth" is wrong** - the main pass does bind a depth
   buffer and submits with the depth flag; the kernel dump window (jobs 600-639) happened to sample the
   shadow and mip-generation passes, which legitimately have no depth.
2. **The terrain scene submits ~52 render jobs per frame**, most of them tiny **per-level mip
   generation** passes (~29/frame) plus the shadow pass. At 20 fps that is ~1 ms per job for the GPU
   side, whereas the vendor stack runs the same scene at 328 fps - so either per-job cost or the
   number of jobs is where the terrain gap lives. Zink gets 27 fps with the same app workload, so the
   *count* of passes is the same for both Mesa paths; the difference must be our per-job GPU/CPU cost
   or how each job is set up (clip/region, hwrt data-set selection, state reloads).

Next: (a) count submissions/frame for zink for the same scene (Vulkan submissions via a kernel-side
counter or by instrumenting the Vulkan driver), and (b) make a synthetic microbenchmark of N small
render passes (e.g. a mip-gen chain) to measure our per-job cost against zink/vendor - if our per-job
fixed cost is the problem, batch the mip-generation levels into fewer jobs.

## Round-176 (2026-10-31): per-render-pass fixed cost is 5.6x zink's - batching is the fix

`drawbench2` extended with a `mipgen` mode (regenerate a 512x512 mip chain every frame) and run with
the existing `passes` mode (27 small 64x64 FBO render passes per frame, `glFinish` per frame), native
(open pvr) versus zink on the same GPU:

| microbenchmark | native | zink | ratio |
|---|---|---|---|
| `passes` 27 x 64x64 FBO passes | **145 fps** (6.9 ms/frame, ~0.26 ms/pass) | **820 fps** (1.2 ms/frame, ~0.045 ms/pass) | **5.6x** |
| `mipgen` 512x512 chain + draw | 292 fps (3.4 ms/frame) | 526 fps (1.9 ms/frame) | 1.8x |

This is the terrain/refract gap: terrain issues ~52 render jobs per frame (~29 of them small mip-gen
passes), so at ~0.26 ms of fixed cost per pass the driver spends ~13 ms/frame on job overhead alone.

Why: the native Gallium path flushes **one render at a time** - `pvr_draw_flush` submits a geometry +
partial-render + fragment job triple for each pass, and the slot/HWRT reuse rules block the CPU on the
previous renders' syncobjs between passes. Zink passes the same workload to the Vulkan driver as a
single command buffer, so the driver submits once per frame and the GPU runs the passes back-to-back.
CPU time is similar in both (117 ms native vs 125 ms zink for the passes run), so the cost is the
CPU-GPU round trip / GPU idle bubble per pass, not CPU work.

Next (the optimisation this session should implement): **batch render submissions** - accumulate the
job descriptors for several renders and submit them in one `DRM_IOCTL_PVR_SUBMIT_JOBS` call, with the
inter-render dependencies expressed as GPU-side sync-op waits (the kernel already supports this for
geom->PR) instead of blocking slot/HWRT waits. Target: the `passes` microbenchmark from 145 towards
zink's 820 fps, then re-measure terrain/refract.

## Round 177 - batched render submission lands (patch 0367); the cost is per GPU kick, not per ioctl

Implemented the round-176 plan: renders are recorded in a pending queue and up to eight are
submitted per `DRM_IOCTL_PVR_SUBMIT_JOBS` (eight slots, two HWRT data sets per size so the CPU can
run ahead).  Two bugs surfaced while getting it to work, both worth recording:

1. The kernel copies each job's command stream through a **user pointer** (`memdup_user` in
   `pvr_fw_cmd_init`), not a GPU address.  The old driver built the streams on `pvr_draw_flush`'s
   stack and submitted immediately, which is fine; a deferred batch submitted the dead stack bytes:
   the ioctl returned EINVAL for one render, and with two or more renders the kernel oopsed in
   `pvr_submit_jobs` (NULL deref in the job setup).  A pending render now copies its stream bytes
   into the pending entry.
2. `kernel.panic_on_oops=1` and `kernel.panic=10` on this image turn every such oops into a board
   reboot 10 s later, and journald loses the tail of the log, so five mid-turn reboots looked like
   power failures (`reboot-tracker` reports ABNORMAL/power-loss).  Capturing `/dev/kmsg` with an
   fsync per record in userspace gave the full oops and the faulting call trace.  Keep this in mind
   for any future driver-hang debugging on this board.

Result on `passes` (27 x 64x64 FBO passes per frame, glFinish/frame, same GPU driver underneath):

| run | fps | wall/frame | cpu/frame |
|---|---|---|---|
| native, one ioctl per render | 145 | 6.9 ms | ~2.0 ms |
| native, batched (0367) | 135 | 7.4 ms | ~2.2 ms |
| zink (Vulkan driver) | 795 | 1.25 ms | - |

Per-pass scaling pins the cost down: native frame time is 0.76 ms + **0.256 ms/pass**, zink is
1.03 ms + **0.0065 ms/pass**; `NOFINISH` changes nothing.  A two-job variant (drop the no-op
partial-render job, the fragment job waiting on the geometry fence directly) is unchanged at
135 fps.  So the cost is not the ioctl round trip and not the extra job: it is one GPU kick per
render pass - the firmware processes each geometry/PR/fragment job set with a fixed per-kick
overhead, and the Vulkan driver pays it once per command buffer, not once per pass.

**Next lever**: encode more than one render pass per kick, i.e. append each pass's control stream to
one stream and submit one geometry + PR + fragment job set per batch of passes, as the Vulkan
command buffer does.  The batching/stream-ownership change above is the infrastructure for that.
17-group gate on 0367: ALL GROUPS PASS.

### Round 177 addendum - the reference submits ONE render job set per frame; we submit one per pass

Measured from userspace with a temporary counter in the Vulkan driver's submit path (zink, `passes`
27x64x64): **every `DRM_IOCTL_PVR_SUBMIT_JOBS` carries exactly 3 jobs (geometry + PR + fragment), two
submits per frame** - the whole frame's 27 FBO passes and the final draw are encoded in ONE
geometry/PR/fragment job set.  Our Gallium driver submits one such job set per render pass (about
28 per frame), which is exactly the 0.256 ms vs 0.0065 ms per-pass difference: the reference amortises
the per-kick cost, we pay it per pass.

Sub-split of the native cost (temporary timers, `passes` 27x64x64, 60 frames): submit ioctl totals
only **0.015 ms/render** (batching already amortises it), while the slot-reuse `drmSyncobjWait`
blocks **0.189 ms/render** - i.e. the GPU/firmware completion of each render is the serial cost, not
the host submission path.  So making the CPU submit even deeper cannot help; the pass count per kick
has to go down.

Next: find how the reference encodes the per-pass render-target state in that single job set (the
fragment static stream carries PBE/EOT per target in our driver, so the reference must carry it in the
command stream or re-program PBE between passes) and mirror it for our flush path.

### Round 178 - where the reference's multi-pass job lives (read, no code yet)

The reference's render job granularity is a `pvr_sub_cmd`: `pvr_arch_render_job_submit()` takes one
`pvr_render_job` (embedded in `sub_cmd->gfx.job`) with its own `sub_cmd->gfx.control_stream` and
issues exactly one geometry+PR+fragment job set for it (`pvr_arch_render_job_submit()` ->
`device->ws->ops->render_submit()`).  A command buffer is a list of sub-commands, and a sub-command
can hold several hardware passes (`pvr_arch_hw_pass.c`, `pvr_hw_pass.h`): the render passes of one
frame are recorded as hw passes inside one sub-command's control stream, then submitted as one job.
The zink measurement (3 jobs per submit, one submit per frame for 27 FBO passes) is consistent with
exactly that.

Next reading points: `pvr_arch_cmd_buffer.c` around the sub-command/hw-pass setup (lines ~1423,
1872, 2659) and `pvr_arch_hw_pass.c` - specifically how a second hw pass re-emits the per-render
target state (PBE/EOT/RT address) into the control stream instead of building a new geometry/fragment
static stream.  That is the piece our `pvr_draw_flush()` has to learn: build the static streams once
per batch, append one hw pass per render, submit one job set.

### Round 179 addendum - corrected submission count: zink really does submit 2 job sets per frame

The round-177 addendum's count was taken from truncated output.  Re-measured with the submit counter
and the whole log: zink `passes 27x64x64`, 5 frames -> **10 submits, every one `jobs=3`, i.e. 2
geometry+PR+fragment job sets per frame for all 27 FBO passes plus the final draw**.  (Native: about
28 job sets per frame.)  So the reference genuinely packs many render passes into one render job -
the merge plan in the roadmap is the right target, and our 0.189 ms/render serial completion is what
it removes.  Counter reverted, Vulkan .so rebuilt from the clean tree.

### Round 180 - the reference's pass count does not reach the submit path at all

Decisive control experiment (temporary submit counter in the Vulkan render submit path): zink
`passes`, 5 frames, **NPASSES=1 and NPASSES=27 both issue exactly 10 DRM submits, each jobs=3**.
zink fps 363.9 (1 pass) vs 346.4 (27 passes) - the 26 extra FBO passes cost ~0.14 ms total.  So the
Vulkan driver packs every render pass of the frame into the same two render-job submissions; the
pass count never scales the job count.  This is the hw-pass machinery (`pvr_arch_hw_pass.c` +
per-hw-pass state re-emitted into the sub-command's control stream) seen from the KMD side, and it
is exactly what `pvr_draw_flush()` must adopt: per-pass state must live in the control stream, not
in a new fragment static stream per pass.

### Round 181 - correction: the `passes` microbench is not a job-granularity proxy

Counting the reference's render-pass entry points with a temporary print: for zink `passes` with
NPASSES=1 and with NPASSES=27, the frame issues only **2 `vkCmdBeginRendering` calls** (dynamic
rendering path counted; the traditional `CmdBeginRenderPass2` path was not instrumented in that run).
So the 27 GL passes do not reach Vulkan as 27 render passes - zink collapses them at the GL level
(each iteration only changes the texture attached to the same FBO and fully overwrites it, so
consecutive clear+draw sequences can be merged).

Therefore round-180's conclusion needs qualifying: the reference's low submit count on `passes`
reflects GL-level pass collapsing plus the driver's own granularity, not a pure proof that the
Vulkan driver packs 27 hardware render passes into one job.  The `passes` microbench cannot be used
to size the per-render-pass gap; use workloads that force real render passes on both stacks
(glmark2 terrain/refract: native 20/38 fps vs zink 27/47).

Next measurement: count BOTH render-pass entries (`CmdBeginRenderPass2` and `CmdBeginRendering`) plus
render submits for zink on the terrain scene (and the same for native), so the terrain gap can be
attributed to pass count vs per-pass cost before changing the driver.

### Round 182 - attribution done: the terrain gap is mip generation, not per-pass cost

Counted the reference on the real scene (zink glmark2 terrain, 11.06 s, FPS 26 -> ~287 frames):
**2573 render passes and 2315 render submits, every submit jobs=3** = ~9.0 render passes and ~8.1
render-job submissions per frame.  Native terrain (round 175) submits ~52 render jobs per frame of
which ~8 are real passes (main/shadow/colour-only) and ~29 are mip-generation passes.

So for the same scene the real render-pass counts match (~8-9/frame); native's extra ~29 mip-pass
submissions per frame *are* the gap (29 x ~0.19-0.26 ms ~= 5.5-7.5 ms of a 50 ms frame, i.e. most
of the 20 vs 26-27 fps difference).  The Vulkan path does not render mips as render passes - zink's
mip generation goes through the transfer/blit path.

Top lever for terrain/refract is therefore: generate mip chains through the transfer/blit path (or
otherwise stop paying a full render-pass job set per mip level).  The pass-merge work remains valid
for the many-render scenes but is not what the terrain gap is made of.

## Round 183 - mip-generation cache lands (patch 0368): terrain +15%, refract +16%

Round-182 attributed the terrain gap to mip generation (native ~29 mip-pass render job sets per
frame vs ~9 real passes, while zink generates mips through the transfer path).  Ceiling test: with
mip generation skipped entirely, native terrain went 19-20 -> 27 fps, exactly the zink number.

First fix, patch 0368: track a per-resource content version (bumped by every GPU render into the
resource and by every CPU write through a transfer map) and remember the version each mip chain was
generated for.  `pvr_generate_mipmap()` returns immediately when the base contents are unchanged -
glmark2 calls `glGenerateMipmap` every frame for the static textures too.  Results (glmark2 offscreen
under weston/headless, native pvr driver): terrain **20 -> 23 fps**, refract **37 -> 43 fps**
(initial baselines 20/38; zink 26-27/47).  17-group gate: ALL GROUPS PASS (mip 14 levels included).

Remaining terrain/refract gap: the chains that are re-uploaded every frame still pay a render pass
per level; the transfer/blit mip generator (one job per level instead of a full geometry+PR+fragment
job set) is the next step for those.

### Round 184 - mip-level CPU wait elision is neutral; the per-level cost is GPU-side

Tried eliding the CPU syncobj waits between the levels of one mip chain (the level renders are
fragment jobs of the same context, and the kernel runs one fragment job per context at a time, so
the consumer is ordered after the producer without a host wait).  Result: terrain 23 -> 22 fps,
refract 43 -> 43 fps - no gain.  Consistent with the round-177 split: mip-generation cost is the
GPU-side completion of each level's render job set, not the host wait.  Change reverted (neutral).

=> The only remaining lever for the per-frame regenerated chains is fewer/cheaper GPU kicks per
level: a transfer/blit or compute mip generator (one job per level instead of a geometry+PR+fragment
job set).  Transfer path is blocked on the TQ replay fault (rounds 154-166); compute (CDM) is the
lower-risk route, one dispatch per level with no cross-job dependency.

### Round 185 - the reference's per-pass encoding, decoded from its own control-stream dump

Enabling the Vulkan driver's control-stream dump (`PVR_DEBUG=cs`) for zink `passes` gives the answer
directly.  NPASSES=1 produces a 3-command control stream (PPP_STATE_UPDATE, VDM_STATE_UPDATE,
PPP_STATE_UPDATE); NPASSES=27 produces ONE control stream with 78 top-level commands:

    27 x VDM_STATE_UPDATE
    26 x INDEX_LIST
    25 x PPP_STATE_UPDATE

i.e. one geometry job's control stream carries every render pass of the frame: each pass emits a
PPP_STATE_UPDATE whose state block holds the per-target ISP/PPP state (pres_ispctl, pds_state_ptr0/3,
region_clip, viewport, outselects, varying words, ppp_ctrl, stream_out_size), then the draw's
INDEX_LIST + VDM_STATE_UPDATE.  The fragment side is one job per frame, so its static stream must be
batch-invariant, with the per-target state living in those PPP state PDS blocks.

Dumps kept in `open-driver-perf/captures/vk-multipass/` (`cs-cmds-1.txt`, `cs-cmds-27.txt`,
`ppp-state-sample.txt`).  This is the concrete model for the merge: our `pvr_draw_flush()` must move
the per-render state currently baked into the fragment static stream (PBE/EOT/ZLS/scissor/ISP) into
per-pass PPP state blocks referenced by PPP_STATE_UPDATE commands in one batch control stream, with
one geometry + one fragment job for the batch.

### Round 186 - external data point on the same GPU: F16SOP is not the missing shader lever

An independent Mesa developer working on the same PowerVR BXM-4-64 (BVNC 36.56.104.183) reports on
Mesa 26.3-devel: the F16SOP path is still unimplemented upstream ("still a bunch of TODOs"), and
their FP16/INT16/INT8 support reuses the 32-bit ALU paths instead
(https://blog.mxpkx.com/index.php/archives/346/, MR
https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44801).  Their vkpeak numbers on this GPU:
FP16 scalar 8.85 GFLOPS vs vendor 10.07 (-12%), FP16 vec4 6.49 vs vendor 5.40 (+20% faster than
vendor), and FP32 vec4 6.04 vs vendor 2.34 (+158%).  So this silicon's FP16 rate is ~the FP32 rate -
F16SOP would not halve ALU time - and Mesa's ALU throughput already beats the vendor driver on
vector workloads.

Consequence for the roadmap: the glmark2 gap is not raw shader throughput; deprioritise F16SOP.  The
remaining gap is render-pipeline/job overhead per pass (the hw-pass merge) and mip generation, i.e.
where the previous rounds already point.

### Round 187 - the reference fragment job stream is batch-invariant (384 bytes, 1 or 27 passes)

Dumped the fragment job's static stream at submit time (`frag_state->fw_stream`) for zink `passes`:
it is **384 bytes for both NPASSES=1 and NPASSES=27**, and the words are identical apart from the
addresses of the per-frame state blocks (e.g. 0x000cf900/0x000cf8c0 vs 0x000d2680/0x000d2640).  So
the fragment job carries no per-render information at all; every per-pass render-target state
(ISP/PBE/EOT/ZLS/scissor) must be encoded in the control stream - the PPP_STATE_UPDATE blocks plus
the PDS state programs they point at (round 185).

That is the exact bar for the merge port: build one batch control stream whose per-pass PPP state
blocks (and the PDS programs they reference) carry the state that our `pvr_draw_flush()` bakes into
a fresh fragment static stream today, keep one fixed fragment stream per batch, and submit one
geometry + one fragment job for the batch.

### Round 188 - two more hypotheses measured and closed

1. **GPU clock is not the gap.** The DT exposes the GPU clock mux `gpu0` (parents clk_800/600/400/300/200
   plus pll-gpu); `clk_summary`/debugfs show it running at **1,104,000,000 Hz (pll-gpu, 1.104 GHz)**
   with the vendor driver loaded. The open kernel module only `clk_prepare_enable()`s its "core"/"clk"
   (pvr_power.c) and never sets a rate, but it inherits this high rate, so ALU throughput is not
   clock-starved.

2. **The per-scene gap is shared by BOTH our stacks, not a Gallium-vs-Vulkan issue.** From the
   bench31 result files (native / zink / vendor fps): shading 961 / 890 / 1375, cel 528 / 546 / 1153,
   effect2d 763 / 676 / 1169, bump 413 / 368 / 672, shadow 454 / 454 / 1097, terrain 20 / - / 40,
   refract 37 / - / 75, while build 1251 / 1155 / 1282 and texture 1391 / 1313 / 1546 are near parity.
   Native and zink are within ~10% of each other everywhere, so the 1.5-2.2x deficits on the
   ALU/pixel-pipeline-heavy scenes come from what native and zink share: the shader compiler (pco is
   used by both; the vendor GL driver uses IMG's compiler) and/or the fragment/pixel-pipeline state.

Conclusion: the next lever is comparing pco's generated code against the vendor compiler's for the
same GLSL - the vendor-compiled USC capture is now the highest-value investigation (it also yields
the F16SOP encoding).  Mip generation/merge work stays valid but the ALU-heavy scene gap is larger.

## Round 189 - per-stage profile: the gaps are per-draw cost and the blend path, not ALU

Added an ALU mode to drawbench2 (128-step non-foldable fract chain per pixel) and ran the full mode
matrix on vendor (bare Xorg :1), native and zink (weston/headless). fps:

| mode | vendor | native | zink | native/vendor |
|---|---|---|---|---|
| alu (128 FMA/frag) | 129.4 | 130.8 | 128.5 | **1.01** |
| fill | 2233.6 | 1753.9 | 2039.5 | 0.79 |
| texture | 1411.6 | 1428.6 | 1152.5 | 1.01 |
| vert (60k verts) | 1247.3 | 1418.7 | 1054.0 | 1.14 |
| blend | 849.3 | 520.9 | 251.8 | **0.61** |
| blenddepth | 848.2 | 523.1 | 252.1 | 0.62 |
| overdraw | 2065.5 | 2473.5 | 2003.6 | 1.20 |
| manydraws (27/frame) | 956.0 | 514.3 | 903.1 | **0.54** |
| cube | 1971.5 | 1710.3 | 1167.4 | 0.87 |
| mip | 1452.7 | 1584.5 | 1148.9 | 1.09 |
| mipgen | 986.1 | 1577.5 | 549.3 | 1.60 |

Conclusions:
1. **Fragment ALU is identical on all three stacks** (130.8 / 128.5 / 129.4) - the glmark2
   shading/cel/bump gaps are NOT shader ALU throughput.
2. **Per-draw cost is the largest Gallium-specific gap**: manydraws native 514 vs zink 903 (same GPU
   driver underneath) = 53 us/draw vs 24 us/draw (vendor 23 us). A per-draw state/stream cost in the
   native driver.
3. **Blend path**: native 0.61x vendor, zink 0.30x - the shader-blend path emits much more work than
   the vendor driver (fixed-function blend does not exist on this silicon, round 166).
4. Per-frame fixed cost shows in fill (0.79) - one render job set per frame ~0.12 ms.
5. mipgen is now 1.6x FASTER than vendor (mip cache, patch 0368).

## Round 190 - per-draw TA state dedup (patch 0369): manydraws 1.9x, blend 1.9x, fill 1.4x

Round 189 localised the largest Gallium-specific gap to the per-draw cost (manydraws native 514 vs
zink 903 / vendor 956).  Cause: `pvr_draw_3d()` rebuilt and re-pointed the entire TA state block and
emitted a PPP_STATE_UPDATE for it on every draw, even when consecutive draws use the same shaders,
rasterizer, viewport and PDS data.  Patch 0369 compares the freshly built TA block with the previous
one and emits the PPP state update only when it differs (the block is memcmp'd; PDS data/shader
offsets that change per draw make it differ, so uniforms still work).

drawbench2, native pvr under weston/headless (3-run medians):

| mode | before | after | vendor |
|---|---|---|---|
| manydraws (27/frame) | 514 | **986** | 956 |
| blend | 521 | **985** | 849 |
| fill | 1754 | **2465** | 2234 |
| texture | 1429 | **1515** | 1412 |

glmark2 per-scene: cel **528 -> 1243** (vendor 1153), effect2d **763 -> 1573** (vendor 1169),
bump **413 -> 1518** (vendor 672), shading 961 -> 946 (unchanged).  17-group gate: ALL GROUPS PASS.

### Round 190 addendum - correction: the per-scene gains were single-scene-run artefacts

The per-scene cel/effect2d/bump numbers quoted in round-190 came from `glmark2 --benchmark <scene>`
single-scene runs whose first FPS line is not the same benchmark as the one in the full run; they
did not reproduce.  The authoritative measurement is a **full glmark2 run** (900 s cap, 33
benchmarks / 16 scenes): **Score 795** (previous native 794/805), and per-benchmark comparison
against the previous native run shows everything within a few percent (build 1238/1253 vs
1251/1309, shading 931/781/642/517 vs 961/794/646/528, bump 411/1353/999 vs 413/1377/1013,
effect2d 750/336 vs 763/340, terrain 23 vs 20, shadow 389 vs 386, refract 44 vs 37).

So patch 0369 does what it says on repeated-state workloads (manydraws 514->986, blend 521->985,
fill 1754->2465 in drawbench2) but glmark2 scenes change per-draw state (uniforms/PDS data), so the
full score does not move.  It stays as a correct, gated microbenchmark win.

Full-run per-benchmark gaps vs vendor (native/vendor): shading 0.45-0.68, shadow 0.55, terrain 0.57,
ideas 0.59, refract 0.59, bump 0.61-0.64, effect2d 0.64-0.69, texture 0.90, build 0.97.  Since native
and zink are within ~10% of each other on every scene and the ALU/vertex/texture/fill microbenchmarks
are at or above vendor parity, the next thing to isolate is per-draw cost with *changing* state
(a "manydrawsflex" microbenchmark: 27 draws/frame with a uniform update per draw) - if that is at
vendor parity too, the remaining gap is in the sampling/fragment-state path these scenes share.

### Round 191 - per-draw cost with changing state is at vendor parity too

Added a `manydrawsflex` mode (27 draws/frame, one uniform change per draw, so the TA state block
differs every draw and cannot be deduplicated) and measured all three stacks:

| mode | vendor | native | zink |
|---|---|---|---|
| manydraws (same state) | 989.3 | 863.5 | 795.0 |
| manydrawsflex (changing state) | 468.7 | **516.8** | 442.4 |
| fill | 2426.7 | 2385.9 | 1841.0 |

Native is *faster* than the vendor driver with per-draw state changes, so per-draw cost is not the
glmark2 gap either. Microbenchmarks now at/above vendor parity: ALU, fill, texture, vertex,
overdraw, blend, manydraws, manydrawsflex, mipgen. The remaining full-run gaps (shading 0.45-0.68,
shadow 0.55, ideas/refract 0.59, bump 0.61, effect2d 0.64) must come from something those scenes use
that the microbenchmarks do not: depth testing/early-Z, mipmapped texture sampling in shaders,
program switches, or the off-screen FBO path. Next: add a depth-tested fill/draw microbenchmark (FBO
with a depth attachment) and split a shading-scene run into CPU vs wall with /usr/bin/time.

### Round 192 - depth-attached FBO exposes a real native-driver inefficiency

Added depth-attached FBO modes to drawbench2 (`depthfill`: 1 quad with depth test; `depthover`: the
same quad at the same depth 27x, so early-Z must reject 26 of them).  Measurements:

| mode | vendor | native | note |
|---|---|---|---|
| fill (no depth attachment) | 2341-2427 | ~2386-2465 | parity |
| fill, FBO now has a depth attachment, depth test OFF | 2341.7 | **1087.5** | native 2.2x slower |
| depthfill (depth test ON, depth cleared) | 1432.2 | 2210.9 | native faster |
| depthover (27 same-depth draws) | 956.8 | 750.9 | native 0.78x |

The 2.2x regression with a depth buffer *bound but unused* points at `pvr_draw_flush()`: it
configures the ZLS load/store whenever `d->zs != NULL` (pvr_draw.c:2238) regardless of whether the
render uses depth, while the TA state only enables depth when `dsa->depth_enabled` (pvr_draw.c:1706).
So a depth-attached target with depth test off pays a full depth load + store per render, and the
ZLS stream may also be disabling the fast path.

Next: gate the ZLS configuration on depth/stencil actually being used (and depth_writemask for the
store), keeping a pending zclear for a later depth-using render - then re-measure `fill` with the
depth attachment and the glmark2 2D scenes (effect2d), plus the depthover early-Z case (0.78x).

## Round 193 - ZLS gating (patch 0370): depth-attached-unused renders 2.3x faster

Fixed the round-192 finding: `pvr_draw_flush()` now configures the ZLS load/store (and the
DEPTHBUFFER fragment flag) only when the render uses depth - `dsa->depth_enabled`, a depth-only
render, or a materialized clear - stores only when `depth_writemask` allows writes, and leaves a
deferred depth clear pending until a render that actually touches depth consumes it.

drawbench2, depth-attached FBO, 3-run medians (before -> after, vendor):

| mode | before | after | vendor |
|---|---|---|---|
| fill (depth attached, test off) | 1087.5 | **2500** | 2341.7 |
| depthfill (test on, cleared) | 2210.9 | 2230 | 1432.2 |
| depthover (27 same-depth draws) | 750.9 | **990** | 956.8 |

17-group gate: ALL GROUPS PASS.  Full glmark2 run: **score 799** (was 795/794) - the fix removes a
pathological case rather than the glmark2 gap, which remains unexplained by every microbenchmark
tried so far (ALU, fill, texture, vertex, blend, manydraws, changing state, depth).

### Round 194 - a realistic math shader is 25% slower than the vendor compiler

Added a `math` mode to drawbench2: a Phong-style fragment shader using normalize, dot, max, pow,
exp2, log2, sqrt and inversesqrt on highp values (the patterns glmark2's shading/bump/effect2d
shaders use, unlike the pure-FMA `alu` chain which is at parity).

| mode | vendor | native | zink | native/vendor |
|---|---|---|---|---|
| alu (128-step FMA chain) | 133.2 | 130.8 | 128.5 | 0.98 |
| math (normalize/dot/pow/exp2/log2/sqrt/rsqrt) | 1292.0 | 1032.6 | 1017.0 | **0.80** |
| fill | 2387.5 | ~2470 | 1841.0 | 1.03 |

So the shader gap is real for realistic math even though a raw FMA chain is at parity: pco's code for
the transcendental/normalisation patterns is ~25% behind IMG's compiler. That accounts for part of
the glmark2 shading/bump/effect2d/e2d deficits (0.45-0.68); the rest must come from the same shaders'
other work. Next: run the `math` and glmark2 shaders with PCO_DEBUG=fs,stats and inspect the
generated instruction sequence for the normalize/pow/exp2/log2 patterns (e.g. whether pow(x,16) is
exp2(log2(x)*16) or a longer sequence, whether normalize collapses to rsqrt+3 muls, and temps).

### Round 195 - pco code for the math shader is compact; the remaining shader gap is issue efficiency

Dumped pco's final IR for the `math` shader (`PCO_DEBUG_PRINT=stats,fs`; note the print env is
PCO_DEBUG_PRINT, the behaviour flags are PCO_DEBUG): 280 bytes of code, 8 temps, no spills, 26
instruction groups (17 main, 5 bitwise, 4 control).  The sequence is reasonable: pow(x,16) becomes
flog -> fmul(16) -> fexp (3 instructions), sqrt via frsq+frcp+mul, hardware frsq/fexp/flog/frcp are
used, some groups already dual-issue (p0/p1 pairs), and the output is packed with pck/u8888.

So it is not instruction-count bloat.  The 0.80x on this shader is issue efficiency (dual-issue
pairing density, bank conflicts, occupancy from temps) rather than missing optimisations.  Shader
code size for a glmark2 shading FS is the next thing to compare directly (the scene's own shader via
PCO_DEBUG_PRINT=fs) against the measured 0.45-0.68 scene ratios.

### Round 196 - glmark2's own shaders resist PCO_DEBUG_PRINT capture (next: dedicated replayer)

Tried to get pco stats for the actual glmark2 `shading` fragment shaders by running the benchmark with
`PCO_DEBUG_PRINT=stats MESA_SHADER_CACHE_DISABLE=1` under the native driver: no stats lines are
emitted (FPS unchanged at 924-953, so the scene used the driver normally).  The same env works for
db2's own shaders (`math` mode printed IR+stats), so glmark2's shaders go through a path that does
not reach the pco dump (cache/async compile or a different compile entry).  Capturing real-scene
shader stats needs a dedicated replayer that feeds the scene's GLSL to our driver directly, or
instrumenting pco_finalise to log unconditionally with a marker.

### Round 197 - shader replayer built; glmark2 shaders are preprocessed templates

Added `open-perf/shaderstat.c`: an EGL pbuffer replayer that compiles a VS/FS pair through our driver,
links and draws one triangle, so `PCO_DEBUG_PRINT` stats can be collected per shader.  glmark2's
shader files are *templates*: they have no precision header (composed by glmark2's ShaderSource) and
use scene-defined names (`LightSourcePosition`, ...), so direct replay mostly fails to compile.
Prepending `precision mediump float/int` plus the light uniforms lets `light-cel` and `desktop`
compile, while light-advanced/bump-normals need the correct uniform types (vec3) and light-phong /
bump-height / effect-2d-convolution use template directives with a different syntax.
Next: replicate glmark2's ShaderSource composition for the shading shaders (or extract the composed
strings by hooking glShaderSource in a preload) and then collect their pco stats.

### Round 198 - dual-issue matters for the math shader (5-25%), not for the FMA chain

Ran the math/alu benchmarks with pco's `no_dual_issue` flag (two repeats):

| shader | default | no_dual_issue |
|---|---|---|
| math | 969.5 / 1005.8 | 726.4 / 952.4 |
| alu (serial FMA chain) | 127.7 / 131.5 | 130.2 / 131.0 |

Dual-issue pairing is contributing on the realistic math shader (the first repeat shows 25%, the
second 5% - noisy because the flag changes the compiled shader and the machine load varies), and it
is (as expected) irrelevant to a serial dependency chain. So pco's dual-issue scheduler is active and
useful; the 0.80x vs vendor is likely pairing *density*/bank-conflict quality rather than absence.

Next candidates for the shader gap: inspect `pco_group_instrs.c`'s `try_dual_issue`/`dual_dest_ok`
heuristics for missed pairs (e.g. pairs only formed within a block, dest bank restrictions, predicate
handling), and instrument the math shader's pairing count. On the scene side, count draws/renders per
frame for the glmark2 shading benchmark to make sure the remaining 0.45-0.68 ratio is not extra passes.

### Round 199 - the bare-Xorg control for the native stack is not possible, and the gap is scene-specific

Tried to rule out the bench31 harness asymmetry (native/zink run under Xwayland, vendor under bare
Xorg) by running native glmark2 under bare Xorg :1.  Xorg cannot initialise GL with our driver
("(EE)" during Xorg startup, the GLES binary then hangs: rc=124 after 200 s), which is exactly why
bench31 runs native under Xwayland - so the control is not available this way.

The asymmetry is nevertheless not the explanation: under the same Xwayland path the native stack is
at vendor parity on build (1238 vs 1282), texture (1389 vs 1546), mip (1585 vs 1453) and the
microbenchmarks, while the deficits are concentrated in shading (0.45-0.68), shadow (0.55), ideas
(0.59), refract (0.59), bump (0.61), effect2d (0.64) - a uniform per-frame harness cost would show up
everywhere.  So the remaining gap is scene-specific GPU work plus the measured 0.80x realistic-math
shader codegen gap.

Next candidates, in order of expected value: (1) count draws/renders per frame for the shading and
shadow glmark2 scenes to look for extra passes; (2) pco dual-issue density; (3) the per-render job-set
cost for scenes with several passes (drawn from the Vulkan-driver hw-pass model).

### Round 200 - dual-issue window tuning: widening hurts, narrowing is neutral on real scenes

Tuned pco's `DUAL_ISSUE_WINDOW` (the look-ahead distance for phase-1 pairing in pco_group_instrs.c):

| window | math | alu | glmark2 shading / bump / effect2d (single-scene) |
|---|---|---|---|
| 8 (default) | 969-1006 | ~130 | 946-953 / 1518 / 1556 |
| 16 | 758-760 | ~124-128 | not measured (math clearly worse) |
| 4 | 1017-1022 | ~131 | 957 / 1517 / 1569 |

Widening to 16 loses ~25% on the math shader (more pairing opportunities but worse register
pressure/scheduling quality), narrowing to 4 is within noise on real scenes (+0.4-1%).  Reverted to
the default 8: the dual-issue scheduler's default window is fine, and its remaining 0.80x vs vendor
is not tunable by this knob.

### Round 201 - shading does 1 draw/render per frame; mediump vs highp is not the difference

Per-frame counters (temporary PVR_COUNT instrumentation: draws in pvr_draw_3d, renders in
pvr_draw_flush, printed per frame): glmark2 `shading` = **1 draw / 1 render per frame** (9288 frames,
929 fps); `shadow` = **3 draws / 3 renders per frame** (368 fps).  So the shading deficit is a single
fragment-heavy pass, not extra passes - it is fragment-shader execution, consistent with the math
shader's 0.80x.

Mediump vs highp on the same math shader (2 repeats):

| | highp | mediump |
|---|---|---|
| native | 1030-1032 | 1033-1037 |
| vendor | 1219-1236 | 1293-1354 |

Mediump is neutral for us and ~6% faster for the vendor, so precision/FP16 lowering is not the
remaining 20-25%: it is fragment-shader issue efficiency (scheduling/dual-issue density/bank
conflicts) versus IMG's compiler.  That is a compiler-quality gap, not a driver-state one.

### Round 202 - the Xwayland/present hypothesis is ruled out by the build scene

Reminder of the harness asymmetry: bench31 runs native/zink under Xwayland but vendor under bare
Xorg, while drawbench2 runs surfaceless, so present/Xwayland cost could in principle inflate the
glmark2 gap.  It cannot be the explanation: a constant per-frame present cost would depress every
scene, yet under the same Xwayland path native matches the vendor on build (1238 vs 1282, 0.97) and
texture (1389 vs 1546, 0.90) - scenes with plenty of frames.  The deficits stay concentrated in
shading (0.68), bump (0.61), effect2d (0.64), shadow (0.55), ideas/refract (0.59).

Combined with round-195/198/201: shading is one fragment-heavy pass per frame, pco's code is compact
with dual-issue active, mediump is neutral, and the realistic-math shader is 0.80x vendor.  So the
residual glmark2 gap is fragment-shader execution efficiency in pco versus IMG's compiler (plus the
scene-specific extra work in shadow/ideas/refract, which are multi-render scenes).

Next: this is now a compiler task.  Concrete pco starting points: the scheduling-after-RA TODO
(pco_ir.c:52 - scheduling currently runs before RA, so register-bank/dual-issue decisions are made
without final register assignments), instruction-selection gaps in pco_nir_alu/pco_opt, and the
recorded `pco_scissor`/bank-conflict heuristics in pco_group_instrs.c.  A useful first milestone is a
microbenchmark that isolates bank conflicts (e.g. many independent FMULs with different register
pairs) to see if pco schedules them into conflicts.

### Round 203 - pco scheduling-pass skips are noise-controlled negatives

Tried pco's PCO_SKIP_PASSES knob on the math shader (names are the pass function names):
`pco_dual_issue_sched`, `pco_schedule`, `pco_shrink_vecs`, `pco_bool`, and combinations.  Single runs
looked like some skips helped (e.g. skip pco_schedule 1032 vs default 893), but an interleaved
5-repeat comparison of default vs skip-pco_schedule gives default 1020.9-1032.7 and skip
1013.5-1031.2 - **identical within noise**.  The earlier differences were run-order/load noise.

So none of pco's pre-RA scheduling passes is hurting or helping the realistic-math shader measurably;
the remaining 0.80x vs the vendor compiler is in the code itself (instruction selection / bank
assignment / issue grouping), not in these toggles.

### Round 204 - BIG FINDING: pco's ILP code is ~4.8x slower than the vendor compiler

Added two more drawbench2 modes: `ilp` (64 independent FMULs in 4 accumulators, summed) and `trans`
(special-function chains: sqrt/rsqrt/exp2/log2 only). fps, 2 repeats:

| mode | native | vendor | ratio |
|---|---|---|---|
| ilp (independent ALU chains) | 210-218 | **1013-1030** | **0.21** |
| trans (sqrt/rsqrt/exp2/log2 only) | 641-687 | 638-663 | 1.03 |
| alu (serial FMA chain, earlier) | ~130 | ~133 | 0.98 |

So transcendentals are at parity and a *latency-bound serial* chain is at parity, but **independent
ALU work (ILP) is 4.8x slower in our driver than in the vendor's**.  That means pco emits code that
does not expose/exploit instruction-level parallelism - the opposite of the earlier "issue efficiency
is only 20% off" reading of the math shader.  Real glmark2 shaders have lots of independent ALU
(lights, interpolants) so this is very likely the main remaining glmark2 gap.

Next: dump the `ilp` shader IR (PCO_DEBUG_PRINT=fs,stats) and inspect the unrolled/scheduled code -
the suspicion is the pre-RA scheduler serialises independent ops (or reduces ILP via register reuse),
and that the post-RA issue grouping does not reorder for ILP.  This is a concrete, high-value pco
target now.

### Round 204 addendum - CORRECTION: the 4.8x ILP gap was a shader artefact

The original `ilp` shader multiplied by a *different literal constant every unrolled iteration*
(1.0001, 0.9999, ...).  pco unrolls the loop and materialises each constant with its own
`bbyp0bm_imm32` instruction group - the dumped IR shows 148 groups for ~130 ALU ops, with the
immediate-bypass instructions as a large fraction.  The vendor driver's compiler evidently handles
that shape differently.  With the multipliers changed to varying factors (`a0 = a0*v.x`, ... - no
per-iteration immediates) the comparison is fair:

| ilp shader | native | vendor | ratio |
|---|---|---|---|
| literals per iteration (original) | 210-218 | 1013-1030 | 0.21 (artefact) |
| varying multipliers (refined) | 380.6-382.9 | 434.0-439.2 | **0.87** |

So the real ILP gap is ~13%, in line with the ~20% realistic-math shader gap, not 4.8x.  The refined
shader is the one kept in drawbench2.  (Note: the literal-heavy shape is still a real pco weakness for
unrolled loops with distinct constants - just not representative of glmark2 shaders.)

## Round 205 - official bench31 after patches 0367-0370: native 794/802, zink 776/775

Ran the full bench31 on the tree with all of this session's patches (batched submission + owned
streams 0367, mip-chain cache 0368, TA-state dedup 0369, ZLS gating 0370):

| arm | score run 1 | score run 2 | previous baseline |
|---|---|---|---|
| native (open driver, Mesa pvr_native) | 794 | 802 | 794 / 805 |
| zink (open driver, Vulkan underneath) | 776 | 775 | 775 / 777 |
| vendor (IMG driver 24.2) | arm failed identity check | (1196, older run) | 1226 / 1196 |

The vendor arm's first run failed its stack-identity check ("did not use the IMG driver
(GL_VERSION)") and bench31 restored the board without continuing - so the vendor baseline in this
run is not usable and needs a re-run.  Native/zink are unchanged within noise: the session's patches
are real microbenchmark wins (manydraws 514->986, blend 521->985, fill 1754->2465, depth-attached
fill 1087->2500, mipgen 986->1578) and improved terrain (20->23) and refract (37->44), but those
benchmarks are too small a share of the glmark2 score to move it past the ~800 plateau.  Beating the
vendor's 1226 needs the shader-codegen gap (~13-20%) closed, i.e. real pco work.

### Round 206 - the failed vendor arms were caused by a stale Xorg :1, not by the driver

The vendor bench31 arm failed twice with "did not use the IMG driver (GL_VERSION)" and empty
output.  Root cause found: a **stale `Xorg :1` process from a much earlier session** (uptime ~5.5 h)
was still holding display :1, so every new `Xorg :1` exited with "Server terminated with error (1)"
while the script's `pgrep -x Xorg` check passed (it saw the stale server).  glmark2 then talked to a
dead/wrong X server: empty output, failed identity check, and in the second attempt a 0 %-CPU hang.

Fix for the next run: kill the stale Xorg, remove `/tmp/.X1-lock` and `/tmp/.X11-unix/X1`, and make
the vendor arm verify a *fresh* Xorg (record its PID before starting) before running glmark2.  The
last trusted vendor baseline therefore remains 1226/1196 (2026-10-30); native/zink for this session
are 794/802 and 776/775.

### Round 207 - vendor baseline re-measured after fixing the stale X server: 1170 / 1150

With the stale `Xorg :1` gone and the arm verifying a *fresh* X (process name on this image is `X`,
not `Xorg`; the socket `/tmp/.X11-unix/X1` is the reliable signal), the vendor arm completed:
**vendor 1 = 1170, vendor 2 = 1150** (previous trusted run 1226/1196 on 2026-10-30; the difference is
thermal/load variance across the session, not a driver change - the IMG driver is untouched).

Complete round-205/207 record on this tree (patches 0367-0370):

| arm | run 1 | run 2 | previous |
|---|---|---|---|
| native (Mesa pvr_native) | 794 | 802 | 794 / 805 |
| zink (Mesa zink on our Vulkan driver) | 776 | 775 | 775 / 777 |
| vendor (IMG 24.2) | 1170 | 1150 | 1226 / 1196 |

native/vendor = 798 / 1160 = 0.69.  The gap is the pco shader-codegen efficiency (~13-20% measured on
realistic shaders) plus the scene-specific multi-render work - no further measurement-only wins are
expected; next work is codegen.

### Round 208 - the pre-RA dual-issue scheduler is net positive (~12%), not a liability

Interleaved 5-repeat controlled run of the math shader, default vs `PCO_SKIP_PASSES=pco_dual_issue_sched`:
default median ~979 fps (978.5, 780.1, 1009.8, 952.0, 988.5), skip median ~865 fps (761.4, 854.7,
864.1, 955.7, 886.8) - skipping the pass is ~12% worse.  Together with round-198 (no_dual_issue is
5-25% worse on math) the dual-issue machinery is doing real work; the residual ~20% gap vs the
vendor compiler is quality/tuning (bank conflicts, pairing density) inside that machinery, not a
missing or harmful pass.  Further work here is genuine compiler engineering (the scheduling-after-RA
TODO), not a knob.

## Round 209 - session consolidation: state of the native driver vs vendor

Patches landed this session (all gate-green, pushed): 0367 batched render submission with owned
streams; 0368 mip-chain content cache; 0369 per-draw TA state dedup; 0370 ZLS gating.

Wins (drawbench2, native pvr, 3-run medians): manydraws 514 -> 986 (vendor 956), blend 521 -> 985
(vendor 849), fill 1754 -> 2465 (vendor 2234), depth-attached fill 1087 -> 2500 (vendor 2342),
mipgen 986 -> 1578 (vendor 986), depthover 751 -> 990 (vendor 957). Terrain 20 -> 23 fps, refract
37 -> 44 (zink 26-27 / 47; vendor 40 / 75).

Official full runs on this tree: native 794/802, zink 776/775, vendor 1170/1150 (Oct 30: 1226/1196).
The glmark2 score plateau is therefore not from the paths fixed above.

Ruled out as explanations (each measured, several with controlled interleaved repeats): fragment ALU
throughput, fill rate, texture sampling, vertex throughput, blending, per-draw cost (same state and
changing state), depth-attached-unused renders, harness/Xwayland present cost, FP16/mediump lowering,
the dual-issue window, and pco pre-RA scheduling-pass toggles.   What remains, and is now narrowed:
~13-20% realistic-shader execution efficiency in pco versus IMG's compiler (shading = 1 fragment
pass/frame, ALU/trans/vertex microbenchmarks at parity, code size compact, dual-issue active) plus the
multi-render scenes' per-render job-set cost.

Remaining work is genuine compiler engineering on pco: the documented scheduling-after-RA TODO
(pco_ir.c:52 - scheduling currently runs before RA, so bank/dual-issue decisions are made without
final physical register assignments), bank-conflict-aware grouping/scheduling (the public ISA
reference explicitly defers bank/conflict details to the NDA ISR document, so the model must be
derived empirically or from the hardware behaviour), and instruction-selection gaps.  A worthwhile
first milestone is an empirical bank-conflict probe: a shader whose ops read registers with controlled
bank relationships and compare timing against the vendor driver to confirm/deny the conflict model.

Measurement methodology notes (learned this session): the vendor bench arm needs a *fresh* X server -
record the new X PID and require the /tmp/.X11-unix/X1 socket (a stale session's Xorg :1 silently
hijacks it; the desktop X process name is "X", not "Xorg").  drawbench2 is surfaceless EGL, so it
cannot measure Xwayland present overhead.  `glmark2 --benchmark <scene>` single-scene runs are not
comparable to full-run per-benchmark numbers; always compare full runs.

### Round 210 - static pco analysis: ranked candidate improvements (report in pco-analysis/REPORT.md)

A static analysis pass (no board runs) over the saved IR dumps and the pco sources produced a ranked
list of candidate improvements for the measured ~13-20% realistic-shader gap:

1. **Enable pco's existing literal pool for the native Gallium FS/VS** (driver-side plumbing, ~50-100
   lines). The literal pool exists in pco (pco_const_imms.c:264-331, enabled for Vulkan via
   pvr_arch_pipeline.c:2943 and for native compute) but not for native graphics. The math FS has 4
   literal groups and 8 temps; estimate 26 -> 22 groups and temps 8 -> ~5, plus pairing windows no
   longer killed by literal materialisation.
2. **Let the dual-issue scan cross pure single-instruction bitwise groups** (small pco change in
   pco_group_instrs.c): literal materialisation is a bitwise-class group and is a hard barrier in
   try_dual_issue (pco_group_instrs.c:210-234, 425-427), so every literal kills a pairing window; the
   literal-heavy ILP shader has 59 literal groups and ZERO pairs. depends_on() is complete post-RA.
3. **Post-RA pairing placement pass** (the pco_ir.c:52 TODO applied to grouping) - recovers pairs
   broken by RA/legalize MOVs and can use final p1 encodability.
4. Move pco_schedule (WDF/IDF) after RA (one-line move, same TODO).
5/6. clamp->sat folding, pow(const int)->squaring (small).
Rejected with reasons: RA bank-bias for p1 (temps are bank TEMP=1 so p1 limits never reject them),
dual-issue window tuning (measured neutral/harmful), min/max re-encode (already single-group), F16SOP.

### Round 211 - candidate #4 (schedule after RA) measured: 25% regression, reverted

Applied the pco_ir.c:52 TODO literally - moved `pco_schedule` after `pco_ra` (so the WDF/IDF scheduler
runs on final physical registers) - and measured with the standard candidate harness:

| shader | before | schedule-after-RA |
|---|---|---|
| math | ~979-1030 | **731-767** |
| alu | ~130 | 126-130 |

The move is a clear ~25% regression on the math shader: the pre-RA scheduler's placement interacts
with RA in a way the post-RA order does not reproduce.  Reverted (tree clean at 6e9baf84).  The
remaining ranked candidates are #2 (let the dual-issue scan cross single-instruction bitwise groups)
and #1 (enable the pco literal pool for native Gallium graphics).

### Round 212 - vendor USC shader-code oracle captured (open-perf/vendor-usc/)

The missing oracle now exists: the IMG GL driver compiles shaders in userspace and writes the USC HW
code into CPU-mapped GPU heaps (`MemHeap:USC_CODE`) rather than via ioctls, so an LD_PRELOAD shim
that dumps the /dev/dri/renderD128 mappings at glFinish captures it (plus GL_OES_get_program_binary
for the Uniflex blob).  Real glmark2 shaders were composed and captured through gdb-break capture of
glShaderSource, then each stage isolated with differential captures (trivial/trivial,
sceneVS/trivialFS, sceneVS/sceneFS):

- shading: VS 800 B, FS 56 B (the shading FS really is gl_FragColor=Color)
- bump: VS 736 B, FS 264 B (real Blinn-Phong: normalize/dot/pow)
- effect2d: VS 312 B, FS 64 B (glmark2's constant kernel leaves 8/9 taps dead) + a conv9 variant with
  all 9 taps live (FS 608 B)
VS code at offset 0x100 of a 512 KiB heap, FS at 0x1c0 of another; encoding is the USC
instruction-group byte format, so pco's ISA tables apply.  Also notable: the proprietary compiler is
split into /lib/libglslcompiler.so and /lib/libusc.so, both dlopen'd by the GLES driver and callable
directly (exported PVRUniFlexCompileToHw / PVRUniflexWriteHWBinary / PVRUniFlexSymbolicDebug).

Next: disassemble the captured FS blobs (vendor's own symbolic-debug entry preferred, pco-ISA decoder
as fallback), diff against pco's output for the same GLSL, then implement the ranked pco candidates
(#2 cross-bitwise-group pairing; #1 literal pool for native graphics).

### Round 213 - candidate #2 (pairing across bitwise groups) breaks the compiler: reverted

Implemented the static analysis's candidate #2 (is_simple_bitwise_op crosses bbyp0bm_imm32 / msk_* /
logical.* / pck / mov / movc / tst in try_dual_issue's forward scan) and measured with the candidate
harness.  Result: math/alu/ilp/glmark2-shading produced **no fps at all** (shader compile/link
failures or GPU faults), fill ran but slower (1426-1618 vs ~2400), and the 17-group gate failed the
"half-float colour targets" group.  So hoisting a phase-1 ALU op above a bitwise group does not
preserve the hardware's semantics (the literal/bypass results are consumed through the bitwise
phase/bypass path, and the grouping order is load-bearing).  Reverted; tree clean at 6e9baf84.

Both small compiler candidates are now measured negatives: #4 schedule-after-RA -25%, #2 pairing
across bitwise groups incorrect.  The remaining work needs the vendor-code oracle (disassembly of
captures/vendor-usc) to see what IMG actually does differently, rather than more blind edits.

### Round 214 - vendor USC disassembly delivered; pco-side comparison harness needs a kmscon-free switch

The vendor shader-code oracle is complete: /home/radxa/open-perf/vendor-usc/ now has `disasm.py` (a
USC instruction-group decoder built on pco's ISA tables), per-scene disassembly under
captures/<scene>/disasm/{vs,fs}.txt, and validated group counts.  The IMG assembler inside libusc.so
is statically linked/unexported, so pco's tables decode the vendor output; header decode-assist
validation passes for bump FS 16/16 and conv9 FS 25/25 groups.

Vendor FS group counts to compare against pco (the lead candidate shaders):
- shading FS 3 groups / 56 B
- **bump FS 17 groups / 264 B** (real Blinn-Phong: normalize/dot/pow)
- effect2d FS 3 groups / 64 B (8/9 taps constant-folded)
- **effect2d-conv9 FS 31 groups / 488 B** (all 9 taps live)

Compiling those same composed GLSL with pco needs the open driver, but this round the switch hit a new
blocker: `kmscon` (pid holding /dev/dri/card0) survived switch-safe's client teardown and the script
correctly ABORTED ("GPU device still open: /dev/dri/card0 root 546798 kmscon").  Fix for the next run:
`systemctl stop kmscon; pkill -x kmscon` (or extend switch-safe's stop list) before `switch-safe.sh
open`.  The pco comparison harness (shaderstat) was also fixed this round: render to an FBO (pbuffer
default-framebuffer draws never trigger the native 3D path) and bind a dummy buffer to every active
vertex attribute, both of which pco compilation needs.

### Round 215 - ATTRIBUTION: pco emits ~2x the instruction groups of the vendor compiler on real shaders

Fixed the pco-side comparison harness (db2 gained VS_FILE/FS_FILE replay with generic attribute
binding; PCO_DEBUG_PRINT must be "stats,fs", plain "stats" prints nothing) and compiled the exact
composed GLSL that the vendor capture used.  Results (pco stats vs vendor disassembly groups/bytes):

| shader | pco groups / bytes | vendor groups / bytes | ratio |
|---|---|---|---|
| bump FS (Blinn-Phong normalize/dot/pow) | 33 / 376 B (7 temps) | 17 / 264 B | **1.94x groups** |
| effect2d-conv9 FS (9 live taps) | 58 / 624 B (16 temps) | 31 / 488 B | **1.87x groups** |
| effect2d FS (8/9 taps folded) | 39 / 376 B | 3 / 64 B (different variant) | n/a |

So the ~20-50% real-scene fragment gap is explained: pco executes roughly **twice as many instruction
groups** as IMG's compiler for the same GLSL (and 40-60% more code bytes), even though raw
instruction selection/microbenchmarks are at parity.  The vendor compiler packs far more work per
group (p0+p1+p2 slots) - pco's pairing is restricted: single-unit ops (FRCP/FRSQ/FLOG/FEXP) are
excluded from co-issue on BXM-4-64 (comment in is_p0_pairable: co-issuing with them "produced wrong
results on BXM-4-64"), and these dominate the bump/normalize/pow shaders.

Next: investigate that exclusion - which exact pairing produced wrong results (maybe only a specific
combination is unsafe), by testing narrower pairings (e.g. transcendental in phase 1 with an integer
p0 only, or with predication disabled) under the 17-group gate and the bump/conv9 FS group-count
metric.  This is now a measurement-driven target rather than a blind edit.

### Round 216 - ROOT CAUSE FOUND: the vendor compiler uses F16SOP (packed half math) in real shaders

Reading the vendor bump-FS disassembly (captures/bump/disasm/fs.txt) shows why IMG's compiler needs ~half
the instruction groups: the Blinn-Phong shader's math is emitted as **F16SOP** instructions
(`p0: f16sop s0neg/s0abs` etc. - 9 of the 17 groups), i.e. the vendor GLES compiler lowers mediump
math to packed FP16 and gets two half-precision operations per instruction.  pco has NO F16SOP
emission (pco_isa.py: "# F16SOP / # TODO") and keeps everything in FP32, which is exactly the 1.9x
group-count ratio measured in round 215 (bump 33 vs 17 groups, conv9 58 vs 31).

This corrects the earlier external-data conclusion ("F16SOP is not the shader lever"): that vkpeak
measurement exercised FP16 through FP32 paths, not the hardware packed instruction.  The oracle
capture proves the vendor driver uses F16SOP for real mediump GLSL in glmark2's bump/shading scenes,
and those are precisely the scenes where we are 1.5-2x behind.

Plan (compiler project, now measurement-driven):
1. From the captures, catalogue F16SOP variant classes and derive the operand bit layout by correlating
   the raw bytes with the GLSL semantics (mul/add/mad, neg/abs modifiers, .x/.y component pairing).
2. Implement F16SOP in pco_isa.py (bit_struct + operand fields) and a NIR lowering that turns
   mediump fadd/fmul/fmad (and paired components) into F16SOP, with the same precision rules GLES2
   allows for mediump.
3. Validate: bump/conv9 FS group counts (target ~17/31), math/ilp fps, 17-group gate, then glmark2 bump
   and shading scenes.

### Round 217 - F16SOP: layout partially derived, implementation plan written

Two workstreams advanced:

1. Vendor-side layout (vendor-usc/f16sop-layout.md): from 82 captured f16sop instructions plus 19
   purpose-built differential mediump samples, the core encoding is derived and round-trips 7/7:
   byte0 = 0x94 (main_op SNGL, ext0=1, sngl_op=F16SOP=4); bytes1-2 = SOP variant word (99 99 SOPEXPR,
   33 22 multiply, 92 93 SOPMAD); byte4 = op selector (add/sub 0x00, min 0x12, max 0x1b); byte8 bits
   0,2 = negate arg1; byte9 bits 1,3 = abs; bytes 11/14 = arg0 swizzle, byte16 = arg1 swizzle.
   Also extracted the vendor USE-assembler table g_psInstDesc (23 SOP mnemonics) from libusc.so.
   Still missing (the blocker): inline operand register/format and destination encoding; next round
   forces coefficient slots / formats / immediates to close it.
2. pco-side plan (pco-analysis/F16SOP-PLAN.md): represent NIR f16 values as 32-bit packed-half-pair
   refs (same device as the existing 64->2x32 mapping), so RA/legalize/grouping/encoder stay
   unchanged; emit F16SOP from a new mediump-ALU demotion NIR pass + trans_alu_f16(); stop
   pco_nir.c:594 scalarising 16-bit ALU so vectors survive to packing; extend pco_isa.py /
   pco_ops.py / pco_map.py with the new bit structs/ops/maps; gate behind PVR_ENABLE_MEDIUMP_F16
   during bring-up, FS first; targets bump FS 33->17 groups, conv9 58->31, then glmark2 bump/shading.
   Offline pco compile harness is being built so the ISA/round-trip work does not need the GPU.

### Round 218 - F16SOP scope confirmed: the affected shaders really are mediump

Checked the captured composed GLSL for the scenes with the largest deficits: glmark2's bump and
shading fragment shaders declare `precision mediump float;` (their VS files have no precision
statements, so vertex math stays default-highp).  That matches the vendor disassembly (9/17 bump-FS
groups are f16sop, no f16sop in the bump VS) and confirms the FS-first staging in the F16SOP plan:
pco's mediump->packed-half demotion applies exactly where we are 1.5-2x behind, and the VS path can
stay fp32 for now.

### Round 219 - offline pco compile harness verified (no GPU needed for compiler iteration)

The analyst's offline harness works and matches the board exactly: `/home/radxa/open-perf/pco-analysis/
offline-pco/run.sh shaders/math.frag` compiles GLSL -> SPIR-V (glslang) -> NIR -> pco and prints
pco IR/stats/binary without touching the GPU.  The math FS reports **280 code size / 8 temps / 26
groups (17 main, 5 bitwise, 4 control)**, byte-identical to the board measurement from round 195.
This means the F16SOP ISA work, the mediump demotion pass and group-count target checks (bump 17,
conv9 31) can be iterated entirely offline; the board is only needed for fps/gate validation.

Parallel state: hunter derived the pure-temp ADD/MUL/MAD byte layouts and the power-of-two literal
rule (39/39 round-trip), dest lanes and uniform slots confirmed to be driver metadata; the analyst is
turning that layout into a ready-to-apply pco ISA patch validated against the vendor captures in the
offline scratch tree; hunter is pinning the remaining register-number field widths.

## Round 220 - F16SOP ISA encodings land (patch 0371): first concrete step of the mediump work

The vendor-capture-derived F16SOP encoding is now in the pco tree (commit 95e740ef, patch 0371):
F_SOP_SEL / F_SOP_LIT enums, the I_MAIN payload pieces and fields for instruction bytes 1-23, and
five multi-byte instruction structs (I_F16SOP_EXPR / _MUL / _MUL_TEMP / _MAD / _LIT), with O_F16SOP +
OM_SOP_OP in pco_ops.py and the F_SOP_SEL mapping + SOPEXPR encode map in pco_map.py.

Validation: 32/32 offline byte checks against the vendor captures (instruction lengths, every
non-TODO byte for ADD/SUB/ABS/MIN/MAX/MUL/MUL_TEMP/MAD and literals 1.0/2.0/0.5/-1.0, plus the
op/neg/abs semantic deltas); pco builds; the offline harness still reports math FS 280 B / 26 groups
(no behaviour change yet); the 17-group gate passes on this tree.
Remaining TODOs marked in the patch: argument/register encoding in bytes 11-21 (raw argNN fields),
form/source-class selection for MUL/MUL_TEMP/MAD/LIT, op_ref_maps placeholders, and the 0x62
constant-pool literal form.  Next: those fields, then trans_alu_f16 + the mediump demotion pass
(target bump FS 17 groups, conv9 31).

## Round 223 - F16SOP ISA encodings v2 land (patch 0372); work paused here as requested

Applied the round-222 model corrections to the committed F16SOP encodings (mesa 3b7fa13d, patch
0372): one instruction per half-register pair (vec4 = two instructions, xy then zw); real operand
fields instead of raw placeholders - src_present(1)|src_reg(7) at payload[11]/[14]/[16] (0x80|reg,
lane-pair step +2), dest at [19] (ADD/MUL) and [21] (MAD), MAD's extra source at [17]/[18]; the
earlier uniform/temp "bank bit" removed (it was an artefact of low register numbers); the 0x62
non-power-of-two literal form removed (its value lives in a driver constant bank pco does not
produce, so non-PoT constants stay fp32); inline PoT codes 0x40+k / 0x4a-k kept.

Verified: isa-scratch validation 12/12 byte checks + hunter's 54/54 round-trip; the real-tree
generated headers reproduce the captured ADD (22B), MUL (24B) and t20 MAD (reg>b15) bytes exactly;
pco builds; the offline harness still reports math FS 280B / 26 groups (definitions only - no
emission change yet, so the last on-board 17-group gate result on the parent commit still applies).
Remaining TODOs, recorded in the patch: payload[12]/[13] source descriptors stay raw, byte-14 bit-7
convention needs a forced-temp capture, the 0x93 92 20B form is unclassified, pco_map wires only
SOPEXPR until trans_alu_f16 selects forms.

## Round 227 - first pco-emitted F16SOP instruction (patch 0373)

The packed-half path now works end to end in the compiler (mesa fa880179, patch 0373):
pco_ref_nir_def() maps f16 values to packed pairs (bits 16->32, chans=ceil(chans/2)); the pco
scalariser skips 16-bit ALU so f16 vectors survive; trans_alu emits pco_f16sop() for fp16
fadd/fmin/fmax (SOPEXPR forms); pco_map gained the group_map(O_F16SOP) that the encoder needs
(without it: "Instruction group mapping not defined for pseudo op").

Evidence, all offline: --nir-test f16add emits the vendor F16SOP prefix
94 99 99 af 00 c0 32 24 00 00 0e inside the 272 B binary (19 groups); math.frag stays 280 B / 26
groups and --nir-test basic 264 B / 20 groups (no fp32 regression); the 17-group board gate passes.

Next: src1_desc/src1_flag lane-pair descriptors (vendor 0x44/0x62), multi-channel rpt lane-pair
emission for f16vec4 (two F16SOPs), MUL/MAD SOP forms, then the mediump enable
(pvr_screen fp16/int16 caps behind an env) and real-scene measurement (bump FS 33->17 target).

Incident note: a crash-looping offline run was once redirected to /tmp (tmpfs) and grew to 2.3 GB,
filling /tmp and breaking the shell; recovered via the REA process tool. Crash-prone harness runs
now need `timeout` plus bounded output.

## Round 229 - external review fixes integrated (0375-0378) + the gate probe that makes them count

An external review GPU-tested the tree and found the 17-group gate was blind to per-render state: it
passed on fa880179, which fails 6 of 6 targeted
probe cases. Four fix commits were applied on top of 0374 (commits 80e00891, 50eb114f, e6224d3e,
279e5984 = patches 0375-0378):

1. Depth load/store decided from the *render's draws* (any draw tests/writes depth), not from the DSA
   bound at flush: an overlay that disables depth last, or a render ended by binding a target without
   depth, no longer loses its depth store; depth clears are stored (test-only) or handed to the next
   render (never used).
2. Mip regeneration after writes the content version missed: an open render into the texture, a
   deferred clear-only render and the compute fallback now bump write_seq, so glGenerateMipmap after
   drawing into a texture regenerates instead of returning the old chain.
3. Pending renders hold their RT data and glFlush submits the batch (RT-data reuse wait sees deferred
   renders).
4. zonly decided from the render, not the framebuffer: blits/clear materialisation runs while a
   depth-only FBO is bound write their output.

Gate probe: `zls_mip_probe.c` is now part of tools/verify and run-all.sh (group "zls/mip per-render
state").  Cases A B F C D G are per-render state; E passes on every build so it proves nothing about
RT-data reuse.  A case only counts once it fails without its fix.  Verified on the board:
- probe with the fa880179 runtime libs (pvr override): **FAIL A B F C D G, RESULT: FAIL**
- probe with the fix build: **RESULT: PASS**
- full gate on 0374+fixes: **ALL GROUPS PASS** (18 groups, including the new one).
Trap worth recording: the probe must run with `MESA_LOADER_DRIVER_OVERRIDE=pvr`; without it the open
stack loads zink and every case passes trivially (a first run did exactly that).

Honest numbers after the fixes (external measurement, reproduced by the review): the total is flat
(glmark2 800/802 -> 801/802), but refract 44/43 -> 37/37 and terrain 23/23 -> 20/20, because
fa880179 with PVR_NO_MIP_CACHE=1 also gives 37/20: 0368's refract/terrain "gains" were stale mips
every second frame, and that claim is retracted.  Win them back by making mip generation cheaper
(one blit render per level today), not rarer.  0373/0374 are offline-only scalar fp16 emission; no
F16SOP instruction has executed on the GPU, vector halves are unimplemented, and the GLES2 mediump
path stays off.  Also open: the draw scissor is ignored (pvr_set_scissor_states is empty) - add a
scissored-draw gate case before implementing it.

## Round 230 - vendor mediump->highp A/B (VOID: the method changed no shader)

The first attempt copied the data dir and ran `sed s/mediump/highp/g` over the shader files. In those
files `mediump` appears only in 5 comment lines; glmark2 adds `precision mediump float;` to every
fragment shader in code. Both arms therefore ran identical shaders, and 1156/1160 vs 1175/1123 was
run-to-run noise. That run used the schedutil governor and no CPU pinning; with identical shaders the
arm means of single scenes differed by up to 12-15% (conditionals vertex-steps=5 0.88, build use-vbo=true
1.15), single runs of one scene by up to 45%, and the total by 4.6% (1123 vs 1175). The conclusion first
written here ("F16SOP is not the score lever") had no evidence; round 231
redoes the experiment.

## Round 231 - precision A/B redone, draw scissor (0379), review follow-up

**Vendor fp16 A/B, redone.** glmark2's own scene option `fragment-precision=int,float,sampler2d,samplercube`
sets the default precision it adds. Two benchmark files with the bench31 list (33 lines): A ends every
line with `:fragment-precision=default,default,default,default` (stock), B with
`:fragment-precision=default,high,default,default`. Vendor 24.2, bare X :1, performance governor,
system.slice on cpu0-5, glmark2 on cpu6-7, blocks A B A B.

Captures of the composed fragment shader and of the vendor's USC code (vendor-usc tools) first prove
that B took effect (FS header `precision highp float`, F16SOP arithmetic gone):

| benchmark | A: F16SOP arith / groups | B: F16SOP arith / groups |
|---|---|---|
| shading cel | 12 / 33 | 0 / 39 |
| bump height | 12 / 25 | 0 / 27 |
| loop (uniform) | 1 / 11 | 0 / 11 |
| conditionals fs5 | 5 / 31 | 0 / 31 |

Scores: A 1195 / 1197, B 1193 / 1196. Per benchmark, median B/A 1.000 (n=33; same-arm spread of the
worse arm per benchmark: median 2%, max 5%). The largest losses at highp: effect2d conv 0.952, bump
height 0.962, effect2d 5x3 kernel 0.963, ideas 0.971, shading cel 0.979. Loop 0.99-1.02, conditionals
0.99-1.02.

So fp16 is worth at most ~5% to the vendor, on the convolution and bump-height scenes, and little or
nothing on the scenes with the largest native/vendor gaps (shading cel 0.45: -2% at highp; loop 0.48
and conditionals fs5 0.53: none). On loop and conditionals the vendor's code has the same group count
with and without fp16. F16SOP is low priority; control-flow codegen is the lever. mediump stays off.

**0374 fp32 check.** The offline pco harness built at fa880179 and at 279e5984 (0378 head; its pco
files are 0374's) gives
byte-identical output for math, bump, the NIR-builder test and the fp16 io/pack shaders. The f16mul
NIR test differs (256 -> 272 B), so the comparison does see 0374.

**0379 - draw scissor.** pvr_set_scissor_states() was empty, so glScissor did not restrict draws. Each
render now keeps an ISP scissor table, as the Vulkan driver's pvr_setup_isp_depth_bias_scissor_state
does; a scissored draw indexes its box and narrows its TA region clip to the box's tiles; an empty box
drops the draw. New gate probe `scissor_probe.c` (run-all.sh group "draw scissor"):
- S1 box edges off the tile grid, S3 per-draw index inside one tile, S4 empty boxes, S6 boxes st
  clamped, S8 more boxes than the 64-entry table: fail without 0379 (tested on 0375-0378 over 0373;
  0374 changes no reachable shader), pass with 0379. S2 (scissor off after on) passes on both.
- Gate ALL GROUPS PASS. glmark2 803/804 with 0379 vs 804 without (one more run without read 795
  while a build ran). No scene moved more than 4%; drawbench2 modes within noise.
- The committed 19-group gate passes on the final tree (5323b619), 0 GPU faults.

**Found, not fixed: a scissored or colour-masked glClear does not draw reliably** (probe S5, S7; the gate group
reports them as an open bug and does not fail on them). The driver sets neither clear_scissored nor
clear_masked, so st clears with a quad draw. Two causes found so far, but not the whole story:
1. st's clear vertex shader reads VERT_ATTRIB_POS. pco lowers vertex inputs only at
   VERT_ATTRIB_GENERIC0..15 and leaves that load unlowered (garbage position). Moving such inputs to a
   free generic slot fixes the lowering (seen in the NIR dump) but not the clear on its own.
2. A vec4 attribute fed fewer components does not get GL's default w = 1: after a draw that leaves
   w = 2, a 3-component draw comes out at the wrong size. This is a general GL bug, not only a clear
   bug: any app with `attribute vec4` fed by `glVertexAttribPointer(size 3)` can draw garbage. Fix:
   give pco the bound vertex element formats (attrib_formats) through vertex shader variants keyed on
   them, as the FS variants are keyed on the RT formats; pco_nir_pvi then fills the missing components.
With cause 1 fixed in a test build, a clear right after a draw still failed, even after a draw that
leaves w = 1, so a further cause remains; the attrib_formats fix for cause 2 is untested. The S5 and S7
results change between runs (S5 passed in one session, S7 in the final gate run), as expected from
undefined registers.

**Commit messages corrected** (patch files only; text, no code): 0368 (the terrain/refract gains were
stale mips, retracted), 0369 (retracted per-scene numbers removed, drawbench2 kept with its caveat),
0373 (offline only; no gate group runs fp16 shader arithmetic).

## Round 233 - the clear quad is fixed: vertex element formats make the GL default w = 1

Two commits close the open clear bug from round 231 (mesa 58f07283 and bd29bc00 = patches 0380, 0381):

1. **Vertex inputs below VERT_ATTRIB_GENERIC0 move to generic slots** (0380): pco lowers vertex inputs
   only at GENERIC0..15; the state tracker's built-in clear vertex shader reads VERT_ATTRIB_POS, which
   pco left unlowered (garbage position). Before pco runs, such inputs move to a free generic location
   (the fetch follows driver_location, so the data mapping is unchanged).
2. **The vertex shader variant follows the bound vertex element formats** (0381): the base shader
   derived each attribute's fetch format from its declared type, so a vec4 attribute fed by a
   three-component vertex element was fetched as four dwords and the missing w came from the previous
   draw's fourth vertex-input register instead of GL's default 1. The vertex shader now keeps its
   source NIR and build variants keyed on the bound pipe_vertex_element formats, exactly like the
   fragment variants keyed on render target formats; pco's unpack_from_format already fills the
   missing w with 1 from the format's swizzle. This is a general GL bug, not a clear-only one.

Evidence on the board (open driver, pvr override, before/after):
- clear_diag before 0381 (with 0380): D1-D4 scissored/colour-masked clears BAD, D9 scissored clear
  after a draw BAD, D17 3-component attribute after a w = 2 draw BAD, D18/D19 scissored clears after
  draws BAD.
- clear_diag after 0381: **21 ok, 0 BAD** - every D1-D19 case passes.
- scissor_probe after 0381: **S1-S8 all PASS**.  S7 (colour-masked clear) failed on both control
  snapshots and passes here; S5 (scissored clear) also passed on the controls, so S5 is not evidence
  for this change (round 231 already noted its results move between runs).
- Gate: the scissor group now requires S1-S8 and a new **"clear quad (D1-D19)"** group runs clear_diag;
  **ALL GROUPS PASS** (20 groups) on the final tree. No performance claim: not measured.

### Round 233 verification (control first, per the review's rules)

Three runtime-lib snapshots were built from explicit checkouts and each probe run printed its identity:
the renderer/version line (R=PowerVR B-Series BXM-4-64, V=OpenGL ES 2.0 Mesa 26.3.0-devel) through a
wrapper that also prints snapshot name, recorded revision and libgallium md5 (three different md5s).

| snapshot | clear_diag bad cases | scissor_probe |
|---|---|---|
| 5323b619 (no 0380/0381) | 8: D1 D2 D3 D4 D9 D17 D18 D19 | FAIL S7 |
| 58f07283 (0380 only) | 4: D9 D17 D18 D19 | FAIL S7 |
| c981a2df (0380+0381) | 0 (21 ok) | 63 pass, 0 fail, RESULT: PASS |

S5 passed on both control snapshots, so it is not evidence for this change (the review's round-231
note that S5/S7 results change between runs applies to S5).  An instrumented run (temporary env-gated
log, not committed) proved the path fires: the state tracker's clear VS logged
`base clear VS differs, bound formats: [0] el=0 shader_fmt=24 bound_fmt=23` then
`created variant slot=0` - RGBA32F derived versus RGB32F bound - and the GLSL1 test shaders logged
24 versus 22.  The committed build re-ran the probes (21 ok / RESULT: PASS) and the full gate:
ALL GROUPS PASS on 20 groups, including the scissor group required as S1-S8 and the new clear-quad
group (D1-D19).

Caveat: Mesa's GL_VERSION embeds the repository HEAD commit when the tree is built (vcs_tag),
so the control snapshots print git-bd29bc00a7 (the tree HEAD at their build time, later than their
content) while a snapshot built at the fix commit prints its own sha.  What each run loaded is
identified by the wrapper line (snapshot rev + libgallium md5), not by that string.  No performance
claim is made (not measured).  Per rule 10, an independent reviewer gets the diff and the evidence
paths before any push.

### Round 234 - independent review findings fixed (0382), text corrected

The fresh reviewer checked the diff and the raw evidence against this text and found: claim 5 unbacked
(no gate artifact existed; the group count was 20, not 21); the GL_VERSION caveat named the wrong sha
(the control snapshots print the tree HEAD at build time, git-bd29bc00a7); the commit text claimed S5
as fixed when S5 passes on the controls; the 0381 patch file carried the pre-amend `From:` sha; and
five code findings in the variant path.  All were addressed:

- 0382 (mesa 0f4838b6) hardens the variant path: hashed slot instead of hardcoded 0 past the cache
  size (and the evicted variant is freed), variant selection moved after the draw's drop paths, only
  fetch-accepted float/dword formats reach attrib_formats, the NIR clone is freed and the fallback
  logged when a variant fails to compile, absent vertex element state no longer creates a no-op
  variant, stale comment updated.
- Text corrected here and in the commit messages (S5 no longer claimed; 20 groups; sha caveat fixed);
  0381/0382 patches regenerated from the rewritten commits (content unchanged: `git diff` against the
  pre-rewrite tree is empty).

Verification, identity-tagged, on the rewritten head (0f4838b6), build snapshot libs-r2, libgallium
md5 21a052c0f2bebc8daf9991fbc0caef75, V=OpenGL ES 2.0 Mesa 26.3.0-devel (git-0f4838b6b9):
- clear_diag: 21 ok, 0 bad (runs/008-r2-verified/clear_diag-r2.txt)
- scissor_probe: 63 pass, 0 fail, RESULT: PASS (runs/008-r2-verified/scissor-r2.txt)
- full gate: 20 groups, 0 failures, ALL GROUPS PASS (runs/008-r2-verified/gate-r2.log)
Control runs and their exact failing sets are unchanged (task runs/000, /004, /005).  Reviewer delta review outcome (HEAD fe34d38c): all six findings addressed; deltas D1 (0382 message
cited the pre-rewrite evidence: now points at runs/008-r2-verified, libs-r2, libgallium md5 21a052c0)
and D2 (a bullet described no diff change: removed) fixed in the amended 0382 message; D3 (freeing an
evicted variant can leave fsv->coeff_vs a stale pointer - parked follow-up: content-key or clear the
coefficient cache, or keep the leak), D4 (duplicated fragment-variant comment above the vertex
selection) and D5 (a failed variant compile is not negatively cached) are non-blocking follow-ups.
Conditional sign-off satisfied: code and evidence sound, outbound text fixed.  Patches 0381/0382
regenerated from 6681058a / fe34d38c (trees empty-diff against the pre-rewrite commits).  Nothing is
pushed; the push needs the human go.

## Round 235 - perf attribution: same-shader A/B shows parity across the replicated draw shapes

Five controlled A/B runs tested whether the glmark2 loop/conditionals scene gaps (0.48/0.53) live in
the compiled shader or in the draw path.  Same composed VS/FS pair (captured from the vendor run),
same db2 binary, same target size (800x600), performance governor, cpu pinning, identity from each
db2 header (vendor arm must print "build 24.2"; open arm the pvr native renderer):

| run | workload | vendor/pco fps ratio |
|---|---|---|
| 012 | 3-vertex draw, FS loop only (matches the earlier microbenchmark shape) | 1.20x (open arm noisy) |
| 013 | 20k vertices, per-vertex loops 5, but no mat4 uniform set (all vertices collapse to the origin) | ~1.00x - vertex work only |
| 014 | 20k vertices, identity MVP, VertexLoops 5 and 0, FragmentLoops 5 | 0.99-1.01x |
| 015 | same but glDrawElements with 16-bit indices, random overdraw | 0.94-1.0x |
| 016 | same as 014 plus per-frame MVP + loop-uniform updates (the way glmark2 drives it) | ~1.00x |

Predictions were written before each run (runs/NNN/card.md); the shader-codegen hypothesis predicted a
1.5-2x ratio and was refuted by 014-016.  So the loop/conditionals scene gap is NOT the compiled
shader, the per-vertex or per-fragment loop work, the indexed draw path, or per-frame uniform updates
under these replicated shapes.  Remaining candidates (H11): a state mix not yet replicated (depth
test/cull with a real mesh), the scene's exact attribute layout, or per-frame driver CPU cost at the
scene's high frame rates.  Harness additions used by these runs (identity print, DB2_SET_INT list,
FILE_VERTS, FILE_INDEXED, DB2_UNIFORM_PER_FRAME, mat4 identity) are in tools/verify/drawbench2.c.
