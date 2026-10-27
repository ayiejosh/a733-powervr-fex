# Reference captures from the working Vulkan driver

Taken 2026-10-24 evening while the board was on the open kernel driver (`~/open-perf/open-enter.sh`,
`powervr` bound, desktop down) so the *working* Vulkan driver produced them:

```
cd ~/open-perf
VK_ICD_FILENAMES=/home/radxa/pvr_gen_icd.json \
PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
PVR_DEBUG=cs ./vkrender_fif 512 > vkrender_stdout.txt 2> vkrender_csb.txt
./../open-perf/open-exit.sh   # back to vendor + desktop
```

- `vkrender_fif` is the offscreen test: one 512x512 RGBA8 render target, `vkCmdDraw(3)`, copies the
  image back and compares every pixel. `vkrender_stdout.txt` shows 262144/262144 PASS, so this is
  the stream of a draw that provably works on this device.
- `PVR_DEBUG=cs` makes the Vulkan driver dump the geometry job's control stream, decoded by
  `pvr_dump_csb.c`. `vkrender_csb.txt` is the raw dump (includes BO contents); `_decoded` strips the
  large buffers down to the decoded stream, which is what to read.

## Geometry control stream anatomy

48 bytes = 12 dwords (the rest of the 4 KiB BO is unused):

| dword | packet | content |
|---|---|---|
| 0..1 | `VDMCTRL_PPP_STATE_UPDATE` | word_count 25, block at 0x800004b140 (general heap) |
| 1..5 | `VDMCTRL_VDM_STATE_UPDATE` | STATE0 0x4c000f80 (`vs_data`+`vs_other` present, cam_size 31), STATE2 0x2880 (PDS data offset), STATE3 0x27c0 (PDS code offset), STATE4 5 (5 varying dwords), STATE5 0x20000441 (max instances 16, USC unified 16 B, PDS temps 16 B, PDS data 16 B) |
| 6..7 | `VDMCTRL_INDEX_LIST` | topology TRI_LIST, index_count 3, no index address (non-indexed draw) |
| 8..9 | `VDMCTRL_PPP_STATE_UPDATE` | word_count 3, block = STATE_HEADER with only `pres_terminate` + STATE_TERMINATE (clip 0..480, render_target 0) |
| 10 | `VDMCTRL_TERMINATE` | context 0 |

The 25-word PPP block (in general heap, pointed at by the first packet) is the TA state:

```
STATE_HEADER   presence: ispctl, ispctl_fa, ispctl_fb, ispctl_dbsc, pds_state_ptr0,
                        region_clip, viewport(1), wclamp, outselects, varying0/1/2,
                        ppp_ctrl, stream_out_size
STATE_ISP      ISPCTL + ISPA(front) + ISPB(front) + ISPDBSC
STATE_PDS      SHADERBASE, TEXUNICODEBASE, SIZEINFO1, SIZEINFO2, UNIFORMDATABASE
REGION_CLIP    mode OUTSIDE, 0..992 in 32-pixel units
VIEWPORT 0     a0/m0/a1/m1/a2/m2 = 256,256,256,256,0,1
WCLAMP         0x29e12e13 (1.0e-13f bit pattern)
OUTPUT_SEL     vtxsize 5, psprite_size_pres 1
STATE_VARYING  3 dwords (all counts 0)
STATE_PPP_CTRL wclampen 1, drawclippededges 1, clip_mode FRONT_REAR, flatshade VERTEX_0
STATE_STREAM_OUT 1 dword, all zero
```

Notes that correct earlier guesses:

- TA state is NOT inline packets in the control stream; it is a memory block named by
  `PPP_STATE_UPDATE`. The driver's existing `pvr_csb_emit_ta_*` writers are block-content writers,
  and `pvr_csb_emit_ppp_state()` already emits the pointer packet.
- The VDM state must carry STATE3/4/5 (`vs_other_present`), not just the PDS data address, or the
  vertex program never starts.
- PDS addresses (`SHADERBASE`, `STATE2`, `STATE3`, ...) are offsets within the PDS heap (0xda00000000
  here); the dump tool adds the heap base back when printing.
- The fragment stage is named by `TA_STATE_PDS_SHADERBASE` (a `pvr_pds_kick_usc` program uploaded to
  the PDS heap) plus SIZEINFO1/2 and UNIFORMDATABASE, not by a DDM-targeted PDS state update.
- `vs_output_size` is 5 for a shader that writes only gl_Position, because pco adds a gl_PointSize
  write to every vertex shader.

## Real-run logs (`real-runs/`)

`probe-real*.txt` are runs of `pvr-probe` against the **real open kernel driver** (desktop down,
`~/open-perf/open-enter.sh`, then restored with `open-exit.sh`). `probe-real10.txt` is the latest:
every `SUBMIT_JOBS` is accepted, and both the real draw and the empty-control-stream liveness render
fail to signal completion within 2 s. The working Vulkan driver renders through the same kernel and
GPU immediately after, so the stall is in this driver's submission setup, not the hardware.

## Kernel-side traces (`real-runs/dmesg-*.txt`)

Round 4 patched the out-of-tree powervr kernel module (`/home/radxa/kernel-src/powervr`) with
temporary `dev_info` prints in `pvr_queue_run_job` (`PVRDBGPROBE`, `PVRDBGCMD`) and in the CCCB kick
path, then reverted it. `dmesg-probe2.txt` is this driver's submission; `dmesg-vkrender2.txt` is the
working Vulkan draw. Result: the kernel runs and kicks every job we submit, including the combined
geom+frag kick; our parsed geometry command matches the reference dword for dword. The fragment
command differed (PBE words were never built - fixed in the driver), and the firmware still does not
complete our kicks, so the remaining delta is inside the kick (context-switch task words / HWRT data).

## CCCB command diffs (`real-runs/dmesg-ccb-*.txt`, `ref_csb_dump.txt`)

Round 5 dumped the kernel's CCCB command bytes (`pr_info` in `pvr_cccb_write_command_with_header`,
reverted afterwards). `ref_csb_dump.txt` is the working driver's static command streams via the
existing `PVR_CSB_DUMP` path. Key results: our geometry command matches the reference; our fragment
PBE block did not, because the fragment firmware stream carries PBE *register* words only (layout,
stride, swizzles, clip) while target address/format belong to the per-job pixel-event program's emit
words. The probe now uses one VM context (the screen's) for programs and submitted jobs.

## Round 6: EOT program + minimal-geometry bisect (`real-runs/probe-real18..22.txt`)

The driver now builds the per-job end-of-tile (pixel-event) program with pco and names it from the
fragment stream, and the HWRT free lists match the reference (global 512..65536 growing, local 32
pages - the raw 25-page minimum is rejected by the UAPI). Completion bisect on the real driver: the
full render, a geometry-only submission with the draw's stream, and the minimal 40-byte geometry job
with an empty control stream all never complete - the last one's firmware stream is dword-identical
to the reference's. So the stall is in firmware context/queue initialisation, not in any stream the
driver assembles.

## Round 7: firmware traces (`real-runs/fwtrace-*.txt`)

The powervr firmware log is available through debugfs: write `0xffffffff` to
`/sys/kernel/debug/dri/1/pvr_params/fw_trace_mask`, run the test, then read
`/sys/kernel/debug/dri/1/pvr_fw/trace_0` (and `trace_1`). `fwtrace-r7h.txt` is our draw's trace,
`fwtrace-vkrender-r7.txt` the working Vulkan draw's. They show that the probe's synthetic jobs were
poisoning its own runs (zeroed control stream -> TA timeout/MMU fault -> GPU lockup -> all later
commands discarded), and that with that fixed the first real draw geometry job kicks the TA and then
takes a single MMU page fault (BIF0 MMU Status 0x2001, Req Status 0x001018da00000000) while the
reference TA finishes and produces tile lists.

## Round 8: fault address decoded (`real-runs/fwtrace-r8-labelled.txt`)

The firmware's BIF0 fault status carries the faulting address in its low 40 bits (verified by
patching the geometry stream's control-stream base to an unmapped 0x8000500000 and seeing exactly
that value in `Req Status`). The real fault decodes to 0x000000da00000000 - the PDS heap base with
offset 0 - so the TA dereferences a PDS pointer left at zero. The TA state block's PDS group was
mapped and compared with the capture; pointing TEXUNICODE/UNIFORMDATABASE at real offsets did not
move the fault, so the zero pointer is elsewhere (context-switch task state is the lead).

## Round 9: static-memory lifetime (`real-runs/fwtrace-r9-fixed.txt`)

The round-8 fault at PDS heap base + 0 was the kernel's VDM sync program area (PDS offset 0): the
probe placed the static firmware programs, then freed their carveouts before the draw, so the
firmware ran the VDM sync program from unmapped memory. Keeping the static memory mapped removed the
MMU fault entirely. The remaining failure is a TA timeout with zero USC slots used (no page fault) -
the VDM sync/context-resume path does not launch the USC program.

## Round 10: static programs identical; TA never starts (`real-runs/ref-static.txt`, `fwtrace-r10-min.txt`)

Dumped the static firmware programs both drivers write (temporary dump in the Vulkan winsys helper,
reverted): PDS VDM sync, USC VS_NOP and PDS pixel-event bytes are identical, so the remaining TA
timeout is not static memory content. The reference's TA emits EVENT_STATUS=0x20 / "TA finished"
~120 ticks after its kick; ours emits nothing, even for a terminate-only geometry job. The only
hardware-visible difference left is the TA state block's PDS group (SHADERBASE/TEXUNICODE/SIZEINFO/
UNIFORM), which the reference points at real nop/descriptor programs.

## Round 11: identical command/logs, TA never runs (`real-runs/fwtrace-r11-baseline.txt`)

The kernel-parsed geometry command matches the reference's 28 dwords except addresses, and the
pre-kick firmware log is line-for-line identical. After `Kick TA` ours gets no interrupt at all
(EVENT_STATUS 0, phase count 0, USC slots 0) and DM2 times out; the reference gets EVENT_STATUS 0x20
and `TA finished`. Pointing the TA block's PDS bases at a real mapped program does not change it.
Next probes: read the TA kick registers from the kernel, check the register-set allocation difference
(ours uses set 1, reference set 0), and try a fragment stream with real PDS background programs.

## Round 12: more elimination, context-reset notification decoded (`dmesg-ctx2.txt`)

With the kernel module's experimental options off (combine_pr/chain_frag/fw_code_cached), the TA still
never starts; substituting the trivial dummy fragment stream changes nothing; the kernel-side firmware
render-context fields are identical to Vulkan's. The recurring `Received unknown FWCCB command
2abc0069` is decoded: 0x69/105 = ROGUE_FWIF_FWCCB_CMD_CONTEXT_RESET_NOTIFICATION - the firmware
telling the kernel a context was reset, which the open kernel driver does not handle.

## Round 14: fresh boot and low addresses ruled out (`real-runs/trace-freshboot.txt`, `tr-lowva.txt`)

The probe as the first open-driver submission after a reboot still hangs the geometry kick the same
way, and forcing all device addresses into the low 0x8000_xxxx_xxxx range Vulkan uses also changes
nothing. The local kernel module source now carries a `pvr_regs` debugfs register reader for the next
probe.

## Round 15: fragment-only jobs are kicked (`real-runs/tr-fragonly.txt`)

A fragment job submitted alone (no geometry) gets a real hardware kick ("Kick 3D") and runs until it
page-faults on our fragment stream - so the context, CCB, register-set and firmware hardware-kick
path all work. The geometry kick remains the only path that logs "Kick TA" and then never programs the
hardware. The fragment stream's PDS background programs and TPU border table are now filled (they were
zero, i.e. PDS heap base + 0).

## Round 16: fragment-kick fault is job-level (`real-runs/dmesg-cmd-*-r16.txt`)

The fragment-only job page-faults at the same BIF0 address/status with either the dummy or the full
fragment stream, ~130 ticks after "Kick 3D" - so the fault comes from the fragment job configuration,
not the stream contents. A 1 s post-context-creation delay changes nothing for the TA kick. The
kernel-parsed geometry command is identical to the reference's including job refs and flags. The
fragment kick is now the working path to bisect with.

## Round 17: fragment ISP state filled (`real-runs/tr-fragisp.txt`)

The fragment stream was missing every ISP/background scalar the reference writes, so the firmware's
3D setup computed all-zero partition state. With the reference's captured values filled in, the
firmware now prints the same "Updating Tiles In Flight (PartitionMask=0x5, ISPCtl=0x80035000)" and
"USCTiles=6" as the reference. The combined geometry+fragment kick is unchanged (Kick TA then
timeout); the next step is a per-register diff of the TA kick hardware state against a working
Vulkan frame using the pvr_regs debugfs.

## Round 18: hardware register-coverage diff (`real-runs/regs-diff-r18.txt`)

Sampled the `pvr_regs` debugfs register blocks continuously during working Vulkan frames and during
our hung geometry kick, then diffed "ever non-zero" offsets. 24 offsets in the VDM/TA/PPP register
block are programmed by Vulkan and never observed during our hang (freelist bases 0x218/0x21C,
region-header base 0x2E0, counts at 0x270-0x280, ...), consistent with the firmware never reaching
the register-programming part of the geometry kick. Sampling includes Vulkan's idle gaps, so treat it
as corroboration, not proof.

## Round 19: Vulkan geometry-only A/B works (`real-runs/vk-geomonly.txt`)

With a temporary winsys patch submitting only the geometry job (fragment signal moved onto it), the
open Vulkan driver's lone geometry kick reaches "TA finished" in ~180 ticks. So the kernel/firmware
accept geometry-only, and the remaining difference is in the memory our geometry command points at
(control stream, TA block, generated PDS/USC programs) - not the command itself. The patch was
reverted afterwards.

## Round 20: our geometry-only kick still hangs; SIZEINFO ruled out

Retested our lone geometry kick on the latest build: still "Kick TA" then timeout, while the Vulkan
geometry-only baseline reaches TA finished. Patching the TA state block's SIZEINFO words to the
reference's captured values (0x01000000 / 0x00804000) changes nothing. The next bisect is byte-level:
dump the reference's control-stream BO, PDS/USC program bytes and kernel-built firmware objects and
diff against ours.

## Round 21: control streams compared (`ref-ctrl-stream-r21.txt`, `our-ctrl-stream-r21.txt`)

Dumped the reference's control-stream BO from a working Vulkan geometry-only frame and compared with
ours: identical structure (PPP/terminate headers encode word-count+address-high the same way, VDM
state differs only in PDS addresses/sizes, index list and terminate identical). A geometry job with
no vertex program at all still hangs at the TA kick, so the VDM state/attribute program/control
stream are ruled out; the remaining inputs are the kernel-built HWRT and context firmware objects.

## Round 22: firmware objects dumped and compared (`dmesg-hwrtb-*.txt`)

Word-by-word dumps of the kernel-built firmware objects for this driver and a working Vulkan run:
`rogue_fwif_hwrtdata_common` identical (0/22 words); `rogue_fwif_hwrtdata` differs only in
device/firmware addresses (11/96); `rogue_fwif_fwrendercontext` differs only in addresses (25/112).
Overriding the TA block's PDS SHADERBASE also changes nothing. The whole data path is now ruled out;
the next candidates are submission/queue state and device-level initialisation the probe does not do.

## Round 23: upstream job-submission fixes backported (`pvr_queue-r23-backport.diff`)

The custom powervr module predates the upstream March-2026 drm/imagination job-submission fixes. The
two stable fixes (count the paired job fence in prepare_job; fit the paired fragment job in the
fragment CCCB) were backported and the module rebuilt. The open Vulkan driver still passes; the native
probe's TA stall is unchanged. The next step is auditing the module against newer upstream fixes.

## Round 24: kernel module audit (`kernel-module-audit-r24.md`)

The module is a Linux 6.16 `drm/imagination` backport plus local customisations. Every post-6.16
upstream commit was triaged; none of the untaken fixes can explain a per-client stall because the open
Vulkan driver's geometry kick works on this exact module. The next probe is a diff of the two clients'
UAPI call sequences (ioctl arguments and ordering), the last per-client kernel-visible surface.

## Round 25: UAPI call sequence compared (`real-runs/*-uapi-r25.txt`, `pvrioctl.c`)

New LD_PRELOAD ioctl logger decodes every DRM_IOCTL_PVR_* call, including the full SUBMIT_JOBS job
array. Probe vs geometry-only Vulkan: CREATE_CONTEXT/FREE_LIST/HWRT_DATASET/SUBMIT_JOBS arguments are
identical; only sequence difference was the probe's zero-length self-check submission (hardware test:
no effect; now skipped by default). Every host-observable layer now matches the working reference, yet
the TA kick never starts - the next probes have to target the firmware boundary itself.

## Round 26: firmware log-template audit (`real-runs/fw-template-diff-r26.txt`)

Normalised firmware log templates from the working Vulkan session vs the fresh-boot failing native
run: the only reference-only messages are consequences of the successful pipeline (TA finished, TPC
flush, 3D buffers, HWRT cleanup) plus a firmware-internal "Host Sync Partition repeat". The pre-kick
sequence for the real draw is structurally identical including GPU Units init, register set setup,
freelist loads, TA buffers and region headers. The next probe captures registers from the kernel at
kick+1 ms instead of polling from userspace.

## Round 27: kernel register capture - the kick IS written (`real-runs/dmesg-regtimeline-*.txt`)

Temporary kernel sampler reads VDM/TA registers every 10 ms after a geometry kick. At +1 ms our
hardware context is programmed almost exactly like a working Vulkan kick (freelist bases, TA buffers,
control-stream address, callstack, EVENT_STATUS=0x10). At ~20 ms our context is cleared by the
firmware's hang handling while the working one stays stable. Ten small-value registers differ at
+1 ms (0x1a8, 0x270, 0x2c0, 0x2f8, 0x300, 0x310, 0x318, 0x348, 0x3a0, 0x420). The stall is after the
hardware kick, not before it.

## Round 28: extended register diff (`real-runs/dmesg-wide-*.txt`)

Sampler extended to 0x0000-0x0800, 0x7000-0x7900, 0x8800-0x8900 with a pre-kick snapshot. The
firmware writes the same context/PPP/TA register set for both clients; round-27's apparent missing
registers are sampling-order artefacts (they include hardware execution counters). Two real
differences remain: register 0x348 (ours 0x1f, Vulkan 0xe, invariant to stream contents) and 0x244
(written only for our job). Next: a busy-wait write-order sampler to find where the sequence stops,
then attribute 0x348/0x244 by varying job flags/HWRT/priority.

## Round 29: firmware register write order (`real-runs/dmesg-spin-*.txt`)

Kernel busy-wait sampler prints every register write for 4 ms after the KCCB kick (~230 us passes).
Both clients finish by ~230-470 us. Written by both: context setup (0x210/214, 0x224, 0x248/24c,
0x2b0, 0x2d8/2dc, 0x348, 0x3d0/3d4, 0x408-414, 0x420). Vulkan-only: 0x1a8=1, 0x270=3, 0x2c0=9,
0x2f8=4, 0x300=2, 0x310=1, 0x318=9, 0x3a0=8, 0x418/41c (looks like hardware execution/completion
state - values match ALIST0 SP=3 / MLIST0 SP=9). Native-only: 0x0008, 0x244=0x80 and a 0x600-0x720
group. 0x348: 0x1f vs 0xe. Next: fault-inject the working Vulkan client to reproduce our symptom.

## Round 30: fault-injection wiring on the working Vulkan client

Temporary hooks were added (and reverted) to make the open Vulkan driver submit a geometry job with a
zeroed TA state block, to see whether the working client can be made to reproduce our stall. The
control stream was parsed successfully (PPP state update -> 25 dwords at 0x800004b140), but that block
is a separate state-block BO, not `rstate->ppp_state_bo` (0x800004b040, 3 dwords). Next: hook the
TA-state upload itself, zero the block, and check whether Vulkan then stalls like the native driver.

## Round 31: reproduced the native stall on the Vulkan client

Fault injection at the Vulkan PPP/TA state-block upload: zeroing the 25-word block makes the working
Vulkan geometry job fail exactly like the native driver (`Kick TA` -> `DM2 timed out` -> locked up).
Per-word bisect: word 5 (TA_STATE_PDS_SHADERBASE) is the only critical word; zeroing words 6-9
(TEXUNICODE, SIZEINFO1/2, UNIFORMDATABASE) still finishes the TA. Our native block equals the
reference word for word except words 5-9; word 5 native=0x3000 (our fragment PDS program) vs
reference 0x2900. Next: compare/validate the fragment PDS program bytes that word 5 points at.

## Round 32: FS PDS launcher layout vs reference

Our fragment PDS launcher is 36 bytes: data segment (8 dwords) then DOUTU code (1 dword) contiguous;
TA block SHADERBASE points at the buffer base and TEXUNICODE is 0. The reference uploads the two
segments as separate 16-byte-aligned PDS allocations (like `pvr_gpu_upload_pds`) and points
SHADERBASE/TEXUNICODE/UNIFORMDATABASE at different addresses with real SIZEINFO sizes. Re-pointing our
fields (SHADERBASE=code, TEXUNICODE=data) did not change the stall. Next: split placements, fill
SIZEINFO from the shader, and compare against the reference device NOP program bytes.

## Round 33: uninitialised PDS buffers fixed; reference TA field semantics decoded

The fragment/vertex PDS program buffers were uninitialised stack memory (reference zero-fills via
vk_alloc); now zero-initialised, fragment launcher bytes clean, but the TA stall is unchanged.
Decoded the reference's TA PDS fields: SHADERBASE = fragment program data segment; TEXUNICODE =
coefficient/descriptor program's code segment (a different program); UNIFORMDATABASE = that program's
uniform data; SIZEINFO = computed sizes. Next: validate our DOUTU SRC0 encoding and test whether the
device NOP program's data offset in word 5 makes the TA finish.

## Round 34: SHADERBASE is not the trigger

Vulkan injection: the TA finishes whether TA-block word 5 (SHADERBASE) names the bound pipeline's
fragment PDS program (0x2900) or the unrelated device NOP program's data offset (0x1000) - only zero
hangs. Native side: word 5 = 0x3000 (launcher data), 0x1000 (attribute code), 0x2000 (attribute data),
0x3004 (launcher code) all still hang. The remaining host-visible input is the client CCB command
stream + KCCB kick data; the next probe byte-compares them between the native and Vulkan
geometry-only submissions.

## Round 35: CCB bytes + kick data identical (`real-runs/dmesg-ccb-*-r35b.txt`)

Kernel dump of kick data and the client CCB: geometry submission is byte-identical between the native
and working Vulkan clients apart from the three expected address fields; same woff=200, wrap=0x7fff,
nclean=1, work_est=0, same UPDATE command. Every host-visible input now matches a working submission,
yet only ours stalls. Next: free-list/cleanup-ctl BO contents, repeated Vulkan A/B on a fresh device,
and possible PDS address clash with firmware-reserved programs.

## Round 36: PDS placement, free lists and launcher bytes eliminated (`real-runs/dmesg-fl-*.txt`)

Moving all PDS programs to +0x1000/+0x10000, and low-VA builds, still hang. Firmware free-list
structures are identical except fw-id assignment order. Overwriting the working Vulkan client's NOP
program with our exact fragment launcher bytes still yields TA finished - so the launcher content is
valid. Remaining candidates: PDS heap layout/object placement (Vulkan populates it with device-init
programs) and VM mapping of our PDS BOs.

## Round 37: PDS placement and reference-shaped PDS words eliminated

Placing the fragment launcher first at PDS+0x1000 (Vulkan's NOP offset) still hangs; so does a fully
reference-shaped TA PDS group (SHADERBASE=data, TEXUNICODE=code, UNIFORMDATABASE=data, reference
SIZEINFO values). New unexplored region: the firmware static data areas - Vulkan's
pvr_winsys_helper_fill_static_memory writes VDM sync + pixel-event + **YUV CSC table** (general+0x80),
while the native init writes VDM sync + pixel-event but never the YUV CSC table. Next: add that fill and
byte-compare all static area contents between the two drivers.

## Round 38: static data areas byte-identical

The native static memory init now fills the YUV CSC table like the reference. Dumped and compared all
four firmware static areas: PDS VDM sync launcher, USC VDM sync binary, PDS pixel-event program and the
YUV CSC table are byte-identical (32/32, 32/32, 32/32, 256/256 dwords). The geometry kick still hangs.
With every memory region verified equal to a working submission, only allocation addresses and
device/timing state remain - next: an address-layout clone of the Vulkan allocations, device-state A/B,
and creating dummy compute/transfer contexts like Vulkan does.

## Round 39: fixed addresses and extra context types eliminated

Mapping the control stream and TA state block at Vulkan-like addresses (0x80000c4000/0x800004b000) via
a new pvr_vm_alloc_at() still hangs, and creating the extra transfer + compute contexts the reference
client has (compute needs a 64-byte zeroed static state stream) does not help either. Every
host-observable input and address choice now matches a working submission; next probes target
device/power state (repeated Vulkan A/B on a fresh boot; full power-state firmware-message comparison;
matching the probe's power state before submit).

## Round 40: device power state eliminated (`real-runs/tr-holdpower.txt`)

Added a `pvr_hold_power` debugfs control to the local kernel module (take/release a runtime-PM
reference from userspace). With the GPU/firmware already powered before the probe submits, the geometry
kick still hangs. The remaining difference is firmware-session-internal; next: repeated Vulkan
geometry-only A/B same-client failure hunt, full-session firmware trace diff, and a minimal Vulkan
client (skip extra contexts) as the last session-shape test.

## Round 41: clean fresh-firmware A/B (`real-runs/ab-*.txt`, `fw-diff-fresh-r41.txt`)

Module reload is the standard way to get a fresh firmware and trace buffer. The patched Vulkan
geometry-only submission passed 10/10 on freshly reloaded firmware; the native probe fails. With clean
single-session traces the pre-kick firmware sequence is **line-for-line identical** through `Kick TA`;
working-only messages are all post-kick pipeline messages, failing-only ones the HWR/recovery storm.
Next: register-write timeline comparison on fresh firmware (0x348/0x244), then byte-compare the static
context state stream.

## Round 42: context order and static context state eliminated

Creating the transfer/compute contexts before the render context (the reference's order) still hangs.
Byte-compared the static context state streams of both drivers: both 192 bytes / 48 dwords with identical
layout; only the VDM/GEOM state base addresses and PDS task-program offsets differ. Next: reverse bisect -
strip the working Vulkan client down (skip extra contexts, then queue features) until it breaks or proves
session shape irrelevant.

## Round 43: minimal Vulkan client still works (`real-runs/tr-minctx-1.txt`)

With temporary patches (skip transfer/compute/query contexts via PVR_VK_MIN_CTXS, submit only the
geometry job, MODE=render so no copy/transfer jobs) the open Vulkan client still reaches TA finished,
2/2 on freshly reloaded firmware. Its session is now one render context + one geometry job - the same
shape as the failing native probe. The remaining difference is Vulkan's device-init PDS/USC programs
(NOP pixel program, static clear state, view-index programs, ...); next step is stripping those one at a
time to see whether one of them is what the firmware needs.

## Round 44: free-list order negative; firmware image is encrypted

Creating the global free list before the local one still hangs. The firmware the open driver loads
(powervr/rogue_36.56.104.183_v1.fw) is an ELF32 MIPS32r2 microMIPS-flagged image whose code sections do
not decode as MIPS or RISC-V and whose rodata has no plaintext; the kernel copies segments verbatim with
no decryption. So it is in an IMG-private encoding decrypted by GPU boot hardware - firmware
instrumentation is not practical without an IMG toolchain. Next: byte-compare the last un-compared
generated programs (the SR/PT context-switch programs).

## Round 45: SR/PT context-switch programs verified equivalent

The native SR/PT program generators mirror the reference's (same doutw_control, num_const64=2, reserve
offset, DOUTU sample-rate handling, stream-out terminate/init programs), and the static context state
streams show identical state/size words with only the PDS program addresses differing. This closes the
last un-compared generated program. Remaining options: vendor-stack differential, a second open client
with a fresh DRM fd, deep kernel-side object diff with the minimal Vulkan client, or an IMG toolchain.

## Round 48: board-safety incident; object diff re-validated

The raw-register-sampling kernel module caused two kernel crashes/board resets (last: Internal error /
data abort, board rebooted at 09:37). The sampler is now gated behind a `dbg_regs` module parameter
(default off); only safe dev_info object dumps remain. Re-ran the firmware-object byte diff at equal
512x512 resolution: HWRT common identical, HWRT data/context differ only in addresses and per-client
sequence numbers. Board restored to vendor driver, kwin up, no faults.

## Round 49: SHADERBASE is only a non-zero gate (Vulkan injection)

On the working Vulkan client, setting the TA block's SHADERBASE word to any nonzero value (0x3000,
0x8000, 0x100000 — even unmapped) still finishes the TA; only zero hangs. So the firmware only checks
"is a pixel shader program named at all"; it does not dereference the word during the TA kick. Our native
0x3000 passes that gate — word 5 is definitively not the cause. A strict pre-kick line diff between
fresh-firmware Vulkan and probe traces shows identical sequences (only idle-cycle count differs).

## Round 50: HWRT data-index negative

Forcing the geometry job's HWRT data_index to 1 (second rt-data of the double-buffered dataset) still
hangs. Together with round 49 (SHADERBASE is only a non-zero gate) and the identical pre-kick traces,
there is no host-visible input left that differs. Handoff records the decision point and remaining safe
options (vendor-stack side channel, second DRM fd, safe kernel structure audit) before escalation.

## Round 51: vendor-stack side channel opened (`vendor-stack/`)

The vendor driver (pvrsrvkm) exposes firmware_trace, debug_dump, status and apphint controls in
/sys/kernel/debug/pvr/. Enabled firmware log groups (comma list, not numeric) and captured a working
vendor GL run: its TA kick emits the same firmware message sequence as the open Vulkan client, so no
hidden kick stage exists in the vendor path either. Vendor DDK 24.2@6603887, same firmware
36.56.104.183. Artifacts: vendor-trace.txt (1820 lines), vendor-debug-dump.txt (709 lines).

## Round 52: fresh TA block diff; fence-shape negative

Fresh minimal Vulkan 512x512 geometry-only TA block vs probe: 20/25 words identical, differences only
words 5-9 (PDS pointers/sizes; all previously tested). Optional second signal sync op on the probe's
geometry submit (matching the reference's two-signal fence shape) still hangs. No host-visible
difference remains.

## Round 53/54: boot directly into the open driver

Booted the board with pvrsrvkm blacklisted, powervr in modules-load.d and display-manager masked, so the
open driver is bound from boot and no desktop runs. The probe was the first-ever GPU client: the
geometry-only submission still hangs (Kick TA -> DM2 timed out). So neither the vendor stack's earlier use
of the GPU nor boot-time device state is the trigger. Board restored to vendor + desktop, 0 faults.

## Round 54 (cont.): low-address retest with the current build

PVR_PROBE_LOW_VA (all general-heap allocations in the 0x8000_0000 range like the reference) still hangs;
so does LOW_VA + FIXED_ADDRS (control stream 0x80000c4000, TA block 0x800004b000). Address layout is
not the trigger. Board restored to vendor + desktop, 0 faults.

## Round 55: 3D kick works, TA kick does not (`real-runs/tr-fragonly.txt`)

PVR_PROBE_FRAG_ONLY: firmware logs the full 3D path (3D Buffers, 3D RTData ready, Updating Tiles In
Flight, Phantom 0, 3D check: missing TA, Kick 3D) and the hardware responds with EVENT_STATUS=0x200 ->
MMU fault event -> BIF0 page fault -> lockup/HWR. So our context/VM can kick and execute the 3D hardware
path; the TA/geometry kick still gets no hardware event at all. A genuinely new asymmetry to chase.

## Round 56: firmware KICK return-status probe - no signal

Added a probe that reads the KCCB KICK command's firmware return slot 200 ms after sending it (the kick is
fire-and-forget, so the kernel never reads it). The slot is 0x0 (NO_RESPONSE) in both the working Vulkan
run and the failing native run (and for the working fragment kick), so this firmware does not report KICK
completion through the KCCB return slot. Probe closed; no discrimination.

## Round 57/58: TA block accepted by Vulkan; fresh CCB byte dump identical

Injecting the probe's words 5-9 into the working Vulkan TA block still reaches TA finished, so the TA
block content is not the trigger. A fresh in-kernel CCB/kick dump of minimal Vulkan vs probe shows the
geometry command image identical modulo addresses (hwrt addr, control-stream base, TPU border table,
UFO address); 46 of 50 dwords equal. Dumps: kickdump-vk-r58.txt / kickdump-probe-r58.txt.

## Round 61: escalation report

MILESTONE2-STALL-REPORT.md (repo root) is the standalone escalation packet: reproduction, verified-equal
inputs with capture paths, the 3D-works/TA-silent asymmetry, KCCB-return-slot negative, firmware encoding
finding, and three questions for IMG. Risky probes (malformed compute job) parked and reverted.

## Round 62: HWR info buffer observability (`hwr/`)

Added a safe debugfs dump of the firmware HWR info buffer (kernel-allocated, firmware-filled, previously
never read by the open driver) plus `hwr_debug_dump_limit = HWR_DEBUG_DUMP_ALL` in the local module.
Findings: working Vulkan hwr_counter=0; failing geometry kick -> type=UNKNOWNFAILURE, dm=2,
event_status=0, hwr_recovery_flags=0x20 (HWR_DM_STALLING), no poll/BIF/MMU detail; fragment run -> real
BIF0FAULT on dm=3 (3D hardware did execute). Records saved in captures/hwr/. A board reboot during the
work was traced to a temporary delayed-work kick probe in the module (UAF after unload) - removed.

## Round 63: OS-data and KCCB dumps; optional config flags negative

Added safe debugfs dumps of the firmware OS data (fw_os_config_flags, sync marks, kccb_cmds_executed,
interrupt counts, SLR stall/signal-loss log - empty for the failing run) and of the kernel CCB
(offsets/slots/return values - read_offset == write_offset in both failing and working runs). Setting
fw_os_config_flags to the vendor value 0xF at firmware boot does not change the TA-kick failure.

## Round 64: firmware-encoding cross-check and safe firmware-swap test

Vendor firmware (rgx.fw.36.56.104.183) has plaintext rodata strings (fw source paths, KCCB errors) while
its code sections are undecodable like the open image -> data plaintext, code encoded (GPU-side decoding;
vendor module has no decryption code, OSLoadFirmware is request_firmware + 16-byte alloc). Swapping the
vendor image into the open driver fails safely: open kernel rejects it with "Unsupported fw info version
2" (it supports v1) and the device does not bind; firmware file restored.

## Round 65: firmware-info blocks parsed

Open image: fw-info v3, PVR_FW_FLAGS_OPEN_SOURCE, layout MIPS_CODE @0xc0000000/0x17000, exceptions
@0x9fc02000, boot @0xbfc00000, data @0xc0032000, fw 1.1 build 6976702, 4096 B device info. Vendor image:
fw-info v2, fw 24.2 build 6603887, same layout shape, code 0x1a000. Open image data sections carry plaintext
strings (firmware source paths, KCCB errors) while code bytes are not valid MIPS/RISC-V and the kernel
does no decryption -> GPU-side code decoding. Firmware-analysis avenue closed for host tooling.

## Round 65b: crafted vendor-firmware boot attempt

Built a crafted firmware from the vendor image (same BVNC/ELF layout) with the open image's device-info
block and a synthetic v3 fw-info header (OPEN_SOURCE flag, fw 1.1, vendor layout entries). Kernel accepts
it (FW version v1.1 build 6603887 OS) but boot handshake times out ("Firmware failed to boot", probe -110);
vendor firmware cannot be run under the open kernel this way. Firmware restored, board recovered.

## Round 66: kick-flag variants negative; research phase closed

Geometry kick flags FIRST|LAST / 0 / FIRST / LAST all time out identically. No host-visible input or
configuration remains that changes the outcome; the blocker is at the firmware/hardware boundary and needs
IMG/vendor input (see MILESTONE2-STALL-REPORT.md). Previous: crafted vendor firmware cannot boot under the
open kernel; firmware code is GPU-decoded.

## Turn 69: firmware RE unlocked (plaintext microMIPS, Capstone; DDK 24.2 source)

The firmware is NOT encrypted - it is plaintext microMIPS that decodes correctly with Capstone 5
(CS_ARCH_MIPS + CS_MODE_MICRO); GNU objdump's microMIPS decoder fails on this image. Cloned the public
TI PowerVR Rogue DDK 24.2 kernel source (/home/radxa/ddk242) for register definitions, firmware-shared
structs and vendor debug routines. Added a safe debugfs dump of the firmware sysdata (fault records with
firmware source path/line; per-thread failed-poll address/mask/count); after the failing geometry
submission there are no failed polls and no firmware faults. Full tooling notes: FIRMWARE-RE-TOOLING.md.

## Turn 70: firmware assert capture works (captures/hwr/assert-after.txt, trace-assert.txt)

`dbg_assert_hwr=1` sets RGXFWIF_INICFG_ASSERT_ON_HWR_TRIGGER in the firmware config flags; the firmware then
asserts on HWR and records source path/expression/line in per-thread assert buffers (new debugfs
/sys/kernel/debug/dri/1/pvr_assert/pvr_assert). The failing geometry submission asserts at
firmware/rogue/rgxfw_hwr.c:3086 - the firmware's HWR path itself. HWR samples signature regs
0x5000/0x5020/0x5028/0x5030/0x5060 and firmware-only 0x40e0/0x4600, all unchanged.

## Turns 71-72: kick-register capture and hardware-programming comparison (captures/kickregs/)

Confirmed the firmware MMIO mapping (ROGUE_MIPSFW_REGISTERS_VIRTUAL_BASE 0xcf800000 -> GPU reg block at
0x01800000; per-thread banks at CR offset thread*0x80000). Added safe debugfs tools: pvr_kickregs
(targeted register snapshot incl. both firmware-thread banks) and pvr_kicksamples (in-kernel 8-sample x
50us register timeline right after every kick, ring of 4). Comparison of the native TA kick with a
working minimal Vulkan geometry-only TA kick shows identical kick-register programming (TPC, ctrl-stream
base/tail, callstack, free-list base, flags/checksums); only the firmware kick-pending mirror 0x0d20 is 1
and stuck for native vs 0 for working Vulkan - kick issued, never acknowledged by hardware. The kick
doorbell (thread<<19)+0x400 is write-only.

## Turn 73: kick handshake fingerprint (captures/kickregs/kickfine-*.txt)

New 2us-resolution in-kernel capture (pvr_kickfine: 256 samples x 2us after every kick). Working
minimal Vulkan geometry kick: d20 1 -> MTS_BGCTX(0xba0)=1 -> d20 0 -> EVENT_STATUS=0x20 -> clear. Failing
native geometry kick: kick registers programmed, d20 stuck at 1, no MTS_BGCTX scheduling value and no
EVENT_STATUS=0x20 ever. The 3D kick in the same native run does reach hardware. Failure localized:
firmware issues the kick, hardware never acknowledges it.

## Turn 74: full-window register diff (captures/kickregs/kickwide-*.txt)

New pvr_kickwide debugfs: all non-zero CR dwords of both firmware-thread banks, 200us after every kick.
Native failing vs working Vulkan geometry kick: identical kick-register programming; working client shows
the hardware post-TA state (ALIST0 SP=3, MLIST0 SP=9, dm state) which the native client lacks; the only
persistent control difference is thread-bank register 0x0348 (native 0x1f vs Vulkan 0x0e), a register the
firmware only reads/saves (hardware status, not client data). Failure constrained to a hardware
state/acknowledgement precondition.

## Turn 75: context-switch PDS programs byte-compared (captures/ctxprog/)

Temporary PVR_DUMP_CTXPROG=1 hooks in both drivers dump the four context-switch PDS programs (SR
store/load, PT store/resume). Reference (open Vulkan) and native probe programs are byte-identical except
embedded per-client buffer addresses / USC task upload offsets; sizes match (SR 16/3 dwords, PT store 8/5,
PT resume 8/6). Closes the last executable client-provided program suspect for the missing TA ack.

## Turn 76: control stream semantically identical; extended wide capture (captures/ctxstream/)

Captured the reference VDM control stream at submit (PVR_DUMP_CTRLSTREAM=1) and the native one
(PVR_DUMP_CTRL=1). Both are 12 dwords with identical structure and fixed field values (PPP state update
word_count 25, VDM_STATE0 0x4c000f80, vs_output_size 5, index list, terminate); only per-client PDS
offsets/addresses and the shader-dependent vs_pds_data_size differ. pvr_kickwide was extended to the
0x2000-0x21ff/0x3800-0x383f hardware context-state block; same result (working client shows post-execution
state, no config difference for native). Every client-supplied byte is now compared and equivalent.

## Turn 77: doorbell execution proven; pre-kick state identical (captures/prekick/)

Single-byte microMIPS firmware patches retargeted the kick-doorbell write (instruction at 0xc0011cc8,
store at 0xc0011cce) to the readable register 0xc88: after the failing native geometry submission
0xc88 = 1 (the doorbell value), proving the firmware executes the doorbell write for the native client.
Redirecting the doorbell away from 0x400 makes the working Vulkan client fail identically (all pixels
wrong, no TA event), confirming 0x400 is the TA kick doorbell. New pre-kick capture (pvr_kickwidepre) on
fresh module loads: native and Vulkan clients have identical pre-kick hardware state (only free-running
timers differ). Firmware patching recipe: 32-bit addiu immediate is byte+2 low, byte+3 high.

## Turn 79: native vertex shader proven valid via Vulkan injection (captures/vs-binary/)

PVR_VK_OUR_VS=1 (replace Vulkan VS NIR with the native probe's shape) and PVR_VK_INJECT_VS_BIN=<file>
(upload an external USC binary) hooks in the open Vulkan driver, plus PVR_PROBE_DUMP_VS=<file> in the
probe. Vulkan geometry-only with either the native VS shape or the exact 56-byte native USC binary reaches
Kick TA -> TA finished. So the native vertex program is accepted by the hardware; the shader binary is not
the TA-kick trigger. Hooks reverted after the experiment. Plan: implement a 3D/TQ clear job (3D path works
for the native client) and show a readback of the changed target.

## Turn 80: 3D-without-TA fault decoded; Vulkan TQ transfer job captured (captures/tq/)

PVR_PROBE_FRAG_REAL submits the real draw's fragment stream alone: firmware processes it ("3D check:
missing TA" -> "Kick 3D"), hardware faults identically to the dummy-state run (BIF0 Req Status
0x0010650000000000 -> address 0x0, null tile data), readback unchanged. So 3D needs TA tile data even for
a clear. PVR_DUMP_TRANSFER_STREAM=<path> in the Vulkan winsys captured a working transfer job
(vk-transfer.bin, 176-byte csbgen stream from vkCmdCopyImageToBuffer) to replay from the native driver on
a transfer (TQ_3D) context - the remaining no-TA path to a GPU write + readback.

## Turn 81: working Vulkan TQ fill + captured stream/programs (captures/tq/)

vktq_fill.c (buffer + vkCmdFillBuffer 0xdeadbeef + fence + host readback) PASSES on the open driver: a
real no-TA TQ_3D GPU write works on this stack. Captured its transfer job via temporary hooks
(PVR_DUMP_TRANSFER_STREAM in the winsys, PVR_DUMP_PDS_PROGRAMS in pvr_gpu_upload_pds): vk-tqfill.bin =
176-byte transfer stream (decoded: usc_clear_register0=0xdeadbeef, isp_render=3, pds_bgnd*=0, event PDS
code/data 0x2620/0x2600), vk-tqfill-pds.txt = 37 PDS uploads (event pixel-event program at data 0x2600 /
code 0x2620). Hooks reverted after capture. Next: locate the destination address, map a native buffer at
the same VA, install the captured PDS programs at the same offsets, submit the captured stream on a native
transfer context, verify sync + readback.

## Turn 82: FIRST native-driver GPU write + readback (captures/tq/tqfill-native-run.txt, tqfill-native-trace.txt)

The native probe replays the captured working Vulkan TQ_3D fill job: transfer context, destination mapped
at 0x80000b1000, pixel-event PDS program at PDS+0x2600/0x2620 and the 16-byte TQ fill USC kernel at
USC+0x5c00 installed, captured 176-byte stream submitted verbatim (new pvr_job_submit_transfer_sync).
Firmware: "Kick 3D TQ" -> EVENT_STATUS=0x18 -> "3D Transfer finished". CPU readback: changed=4096/4096,
mismatched=0, first=0xdeadbeef. pvr-probe --- ALL PASS. Kernel-side completion sync for transfer jobs does
not signal yet (the readback verifies the write). Mesa commit c78195f.

## Turn 83: transfer completion-sync narrowing (captures/tq/tqfill-r82d.txt)

Kernel prints: for the native TQ fill job the kernel arms the user sync op with the sched finished fence
(update_fences ops=1, push_fences), the firmware completes the job (PVRDBG TQ done seqno=1 ufo=1), and the
scheduler frees it with finished_signaled=1 - the finished fence does signal. Yet userspace
drmSyncobjWait/TimelineWait times out (ret=-62, query point 0) for both binary and timeline syncobjs. GPU
write is confirmed by readback. Kernel-side sync delivery difference vs Vulkan remains to be fixed.

## Turn 84: SPIR-V -> spirv_to_nir vertex shader (captures/spirv/)

The probe's VS now comes from a SPIR-V module (captures/spirv/vs_pos_plain.spvasm, assembled with spirv-as)
parsed by spirv_to_nir with pco's options - the Vulkan driver path - instead of hand-built NIR. pco compiles
it to 56 bytes USC with temps=0, vtxins=3, vtxouts=5 (attribute load survives; no RA abort; differs from a
constant shader). glslang's gl_PerVertex interface-block form segfaults nir_split_per_member_structs in this
pco build (build_member_deref), hence a plain Output variable. pvr-probe --- ALL PASS; mesa commit 81ee713.
Board rebooted twice earlier from our own kernel debug hacks (unbalanced dma_fence_put / static fence
callback); reverted.

## Round 80: completion-sync parked (no more ad-hoc kernel fence hooks)

Clean tracing established: kernel arms the user sync op with the sched finished fence; the firmware
completes the TQ job; the scheduler frees the job with finished_signaled=1; the user syncobj still never
signals (drmSyncobjWait/TimelineWait time out, query point 0). Working Vulkan's sequence is SUBMIT_JOBS ->
SYNCOBJ_TRANSFER -> SYNCOBJ_WAIT; doing transfer-then-wait from the probe also times out. GPU writes remain
verified by readback (TQ fill changed=4096/4096, mismatched=0). Further kernel fence instrumentation
stopped: two of our own debug hooks (unbalanced dma_fence_put, static fence callback) caused the two board
reboots earlier. SPIR-V -> spirv_to_nir vertex shader path in place; pvr-probe --- ALL PASS (shim).

## Round 81: full-target GPU write with readback (captures/tq512/)

PVR_PROBE_TQ_TARGET_ADDR patches the TQ fill's destination address into the pixel-event PDS data (word 2 =
addr>>2 low 32, word 3 bits[0:5] = addr>>34) and makes the target the render-target-sized buffer. Replaying
a captured 1 MiB Vulkan fill job (2048x128 RGBA8): the native driver writes 512x512 RGBA8 at
0x9800000000; readback changed=262144/262144, mismatched=0, first=0xdeadbeef, ~1 ms; pvr-probe ALL PASS.
Mesa c2e0c04. Only the rasterisation/TA draw remains; next plan: compute-(CDM-)based triangle draw via the
same capture/replay recipe.

## Round 82: compute (CDM) draw captured; native replay hangs GPU (captures/cdm/)

Reference: vktq_draw.c + tri.comp - a compute shader rasterises a 512x512 triangle into a storage buffer;
Vulkan verifies inside=73728, outside=188416, bad=0 PASS. Captured CDM job (new Vulkan hooks): compute fw
stream, CDM control stream @0x80001b1000, compute context state BO @0x800006b000, 64-byte static context
state, PDS programs (kernel @0x2640/0x2660, fence, descriptor constants @0x2700), USC programs, descriptor
page @0x800004b000 (descriptor set @+0x80: {addr=0x80000b1000,size=1 MiB,offset=0}), target @0x80000b1000.

Native replay (PVR_PROBE_CDM_DIR): creates the compute context from the captured static state,
reconstructs all referenced buffers at the captured addresses, submits the captured fw stream. Firmware
activates and kicks DM4, then logs a GPU lock-up/HWR and sends CONTEXT_RESET_NOTIFICATION (kernel prints
"Received unknown FWCCB command 2abc0069"); target stays zero. Missing piece is in the reconstructed CDM
state (context-state BO content / CSW programs / descriptor constants).

## Round 83: compute replay completes on hardware (captures/cdm/fw-trace-compute-finished.txt)

With a fresh module and fw_trace_mask enabled, the previous CDM lock-up was a fetch fault at USC+0x7200
(BIF0 Req Status 0x00181fe000007200): the compute shader's real USC program is at 0x7200 (408 B, captured),
but the replay only mapped USC 0x1000-0x2fff. Extending the reconstructed USC page to 0x7000 makes the
firmware log "Kick Compute" -> EVENT_STATUS=0x4 -> "Compute finished" -> UFO update: the native compute job
completes on hardware with no HWR. The target is still untouched and every reconstructed buffer stays
unchanged, so the remaining gap is that the shader task does not produce stores (kernel-program data at PDS
0x2640 names USC 0x7200; next step: compare with the successful Vulkan trace and verify the descriptor
constants/workgroup setup).

## Round 84: replayed compute shader executes; stores land but values/addresses are odd (captures/cdm-ids/)

ID-probe shader (Vulkan reference: local0=0x11110000, wgx1=0x22220001, wgy1=0x33330001, gid1=0x44440001,
marker at px[4096], PASS). Native replay of that captured job writes 0xCAFEBABE at target[0] and a
repeating 1..7 pattern (word index mod 8), 505 nonzero dwords: the CDM/descriptor/USC path runs end to
end. Remaining anomaly: store address/value mapping (marker at px[0] not px[4096]; probe values lose their
tags). Next: dump the shader's computed store addresses, review pco's descriptor encoding and CDM task data.

## Round 85: replay constant-offset anomaly (captures/cdm-const/, vkcdm8 + vkcdm9)

Six-store probe (px[0..3], px[100], px[4096]) and single-store probe (px[4096]=0xCAFEBABE) captured under
Vulkan and replayed from native: Vulkan writes the expected addresses; the replay writes px[0..3] fine but
px[100]/px[4096] land at offset 0 (single store: 0xCAFEBABE at px[0]). The captured USC program at 0x7200 is
the exact program Vulkan executes and the descriptor set content matches, so the missing piece is how pco
forms large constant offsets: the USC common-store initialisation by the PDS programs (empty program at
0x2600, kernel program at 0x2650) and the CDM kernel data at 0x2640 - the last pieces of per-dispatch state.

## Round 86: constant-offset sweep (captures/cdm-offsets/)

Offsets sweep shader (px[0,1,2,3,4,8,16,32,64,100,128,256,512,1024,4096]) verifies 15/15 under Vulkan.
Replay: offsets 0..4 land correctly; every offset >= 8 resolves to 0 (all overwrite px[0], last is px[4096]'s
0xA000000e). The USC code has inline-offset stores for the first five and helper call sequences for the
rest (branch blocks `ff6a8002`). Runtime PDS dump (PVR_DUMP_COMPUTE_PDS_RUNTIME) shows dispatch-time PDS
code/data are identical to the upload-time capture, so PDS state matches Vulkan; the remaining suspect is
the USC helper scratch/stack (common/unified store) state.

## Round 87: FIRST DRAW + READBACK from the native driver (captures/cdm-draw/)

Root cause of the replay address anomaly: pco loads larger immediates from a compile-time literal pool
(general-heap buffer whose address the descriptor PDS constants point at). Dumping it under Vulkan
(PVR_DUMP_LITERALS) and reconstructing it in the probe makes the replayed job exact. Replaying the captured
triangle-draw job now renders the triangle: readback inside=73728 (Vulkan's exact count),
center=0xFF0000FF, corner=0xFFFF0000, marker 0xCAFEBABE -> PASS. This is the compute/CDM route; the
fixed-function TA rasterisation remains hardware-refused (see MILESTONE2-STALL-REPORT.md).

## Native compute port (started): compiler matches reference

pvr_shader_compile_compute() (native driver) compiles the triangle-draw compute SPIR-V to **456 bytes of
USC code byte-identical** to the reference's uploaded shader, with matching pco data (temps=7 shareds=13
vtxins=1 coeffs=3 literals=9 wg_id_mask=0x3). The PDS compute kernel program generated with the PDS library
matches the capture too (data `00007200 00000010 90000002 80000000`, matching code prefix). Mesa ed86b1c.
Remaining: descriptor/literal upload programs, CDM control stream, compute fw stream, context creation and
submission.

## Native compute port: descriptor-upload program matches reference

The native compute path now generates the descriptor-upload PDS program + dispatch data segment from its own
pco data. Against captures/cdm-draw:
  desc code: f0040000 f4050100 (identical)
  desc data: 0004b080 00000080 0004b040 00000080 10000004 90008009 (identical)
  kernel PDS data: 00007200 00000010 90000002 80000000 (identical)
  kernel PDS code prefix: same 9 dwords (identical)
Mesa e1e9cd2. Remaining: CDM control stream, compute fw stream, context creation, submission.

## Native compute draw works (captures/cdm-native/)

PVR_PROBE_CDM_NATIVE renders the triangle with no captured reference data: native pco compile (456-byte
USC), PDS kernel/fence/descriptor programs, 22-dword CDM control stream, 44-byte compute fw stream
(byte-identical to the reference), compute context creation (minimal static state), submission through the
native VM, readback. Result: PASS - inside=73728 (reference's exact count), centre=0xFF0000FF,
corner=0xFFFF0000. Mesa 774b442; output in captures/cdm-native/native-draw-output.txt.

## Full pipeline integration: draw_vbo renders the pipeline's geometry (captures/cdm-native/)

The compute fallback is now part of the driver (mesa 774b442 + 171150f): pvr_compute.c builds the state
from the native pco compile + PDS library programs on real heap allocations; pvr_context.draw_vbo takes the
bound vertex buffer's three clip-space vertices and the bound render-target size, patches the descriptor
set and submits the compute job. Probe (PVR_PROBE_COMPUTE_DRAW=1): 512x512 default triangle -> inside=73728
exact; 256x256 vertex-buffer full-screen triangle -> inside=65536/65536; PASS with 2 draws executed;
--- ALL PASS with/without env and under the shim. Output:
pipeline-draw-2cases.txt. Known limitations: target dimensions must be multiples of 8 (no early-return
guard in the shader - pco_cf crash); vertex stage is identity; fence delivery still polled; TA/3D path
remains unusable.

## Pipeline complete: sync fixed, draws synchronous (captures/cdm-native/pipeline-draw-synced.txt)

Completion waits were using a relative timeout with no flags; this kernel's syncobj wait takes an absolute
CLOCK_MONOTONIC deadline with DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (the reference/Vulkan convention). All
waits now use that convention: compute-fallback draws complete synchronously with no "did not signal"
errors; two-case pipeline test still passes (512x512 default -> inside=73728; 1366x768 vertex-buffer
full-screen -> 1049088/1049088); probe --- ALL PASS with/without the draw check and under the shim. Mesas
5e5d833; output pipeline-draw-synced.txt. (Raw 3D render job still times out: TA path hardware-refused.)
