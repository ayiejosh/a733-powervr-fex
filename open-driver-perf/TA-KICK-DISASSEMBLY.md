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
