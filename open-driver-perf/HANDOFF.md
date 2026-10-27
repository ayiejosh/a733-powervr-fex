Status 2026-10-27 (GL frontend functional via Wayland; fresh benchmark): **glmark2 now runs on the open
stack through the Wayland frontend and beats vendor native GL on the same run conditions.** Fresh,
same-session numbers (glmark2 2023.01 GLES2, default set, 800x600; logs in
`captures/bench-2026-10-27/`): vendor native GL on X11 = 511/533 (median 522); open stack (zink -> open
Vulkan, headless weston + Wayland) = 543/570/546 (median 546). The X11/Xwayland route for the open stack
currently dies early on `DRM_IOCTL_MODE_CREATE_DUMB: Permission denied` (open DRM node has no dumb-buffer
support), so Wayland is the working frontend; `bench/glmark2-wayland-open.sh` reproduces the result. The
earlier recorded 289-309 (open) vs 720 (vendor) numbers came from a different session and the Xwayland path;
they are superseded by the fresh measurement above. Also finalised this session: the open stack runs a
**official IMG open-ABI firmware** (see below; the firmware-image request is resolved - it is published in
IMG's linux-firmware fork), and only DM2/TA firmware-side observability remains in
`docs/IMG-FIRMWARE-REQUEST-36.56.104.183.md`.
Status 2026-10-26 (goal round 4): **vendor-parity check is in `PERF-PARITY-STATUS.md`**. Short version:
open Vulkan is at parity with vendor Vulkan; OpenGL through zink is ~2.5x behind vendor native GL (the
remaining gap); the native GL driver's 3D path is still blocked by the DM2/TA hardware stall, with its
compute-fallback draw path benchmarked (98 fps @512x512, pixel-exact) and its benchmark comparison recorded.
The TA blocker after four goal rounds: the native client's kick is bit-identical to the working Vulkan
client's in every firmware-visible artifact (firmware trace, pre-kick registers, command streams, HWRT
dataset args, static context state, FWCtx layout) yet the hardware never starts DM2; no accessible register
or memory differential remains - cracking it needs vendor/IMG firmware-side observability. Evidence in
`TA-KICK-DISASSEMBLY.md` and `captures/`.
Status 2026-10-26 (BENCHMARK OBTAINED on the working native draw path; TA still the open blocker):
**the native driver's draw path is benchmarked.** New `PVR_PROBE_COMPUTE_BENCH=1` mode renders through the
driver's `pipe_context` API and measures the full synchronous round trip (encode + submit + GPU + completion
fence), 200 frames after 5 warm-up frames, with the pixel counts verified against the expected triangle
coverage every run (mesa `0fc2f88`):
- 512x512 default triangle: **10.198 ms/frame, 98 fps, 25.7 Mpix/s** (inside=73728);
- 1366x768 full-screen vertex-buffer triangle: **39.832 ms/frame, 25 fps, 26.3 Mpix/s** (inside=1049088);
- 1920x1080 full-screen vertex-buffer triangle: **75.413 ms/frame, 13 fps, 27.5 Mpix/s** (inside=2073600).
Probe prints --- ALL PASS with the benchmark enabled and without it, and under the drm-shim. This is the
number a caller of the driver's draw API sees today (one synchronous compute-fallback draw per frame).

The fixed-function 3D/TA path is *still* hardware-refused, and this session pinned the failure one level
deeper:
- firmware traces with the full trace mask were captured for a working Vulkan TA kick and the failing
  native one. Everything through the kick is identical (same KCCB command shape including the combined
  0x75 kick, same "cCCB Woff update = 200 DM = 2", same freelist/RTData/region-header/ALIST setup, same
  "Kick TA: FWCtx ... First kick:1, Last kick:1, CSW resume:0"). The working client then logs
  `Irq Task (EVENT_STATUS=0x20)` -> `TA finished`; the native one logs neither and goes to the DM2 timeout /
  HWR path.
- new `/dev/mem`-based register tooling (`tools/gpu-regs/`, `kicktrace.c`) shows the pre-kick register state
  is identical apart from timers/random values and addresses, and that the failing kick *does* briefly
  assert `MTS_BGCTX (0xba0)=1` and then clears it: MTS schedules DM2, but DM2 stalls silently before it
  reaches any observable state (no EVENT_STATUS bit, no ALIST/MLIST stack-pointer writes, no USC slots
  used, no BIF fault).
- a failing native run does not poison the hardware: `vkrender_fif 512 1` still passes immediately
  afterwards (and passes on a fresh module too), so the gate is per-context/per-submission, not global.
- a terminate-only control stream (PVR_PROBE_MINIMAL_GEOM=1) also times out, so the gate is before the TA
  reads any of our draw commands - it is in DM2 start/context-load or the firmware->MTS scheduling path,
  not in the control stream content.
- `TA-KICK-DISASSEMBLY.md` records the line-by-line firmware reading (gp=0xc0034360, kick handler
  c0011a00-c0011d00, doorbell at c0011cce, MTS_SCHEDULE=0 at c0011d00, alternate path at c0011f6a,
  completion/log at c0011e80-c0012100) plus the next break steps.
# Native GL: where we left off

Handoff for a fresh session. Read this first, then `NATIVE-GL-DESIGN.md`.
Status 2026-10-26 (PIPELINE COMPLETE + SYNC FIXED): **draws now complete synchronously and the job
completion fences work** (mesa `5e5d833`). The completion waits had been broken by a timeout-convention
mismatch: this kernel's syncobj wait treats `timeout_nsec` as an absolute CLOCK_MONOTONIC deadline when
`DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT` is set (the convention the reference winsys and Mesa's Vulkan
runtime always use), while the driver passed a relative duration with no flags - so every wait failed and
submits returned long before the GPU was done. All waits now go through helpers that pass an absolute
deadline with WAIT_FOR_SUBMIT. Verified: the compute-fallback draws complete synchronously (no "did not
signal" errors; readback immediately after the wait shows the final image), the two-case pipeline test still
passes (512x512 default triangle inside=73728; 1366x768 vertex-buffer full-screen triangle
inside=1049088/1049088), probe `--- ALL PASS` with and without `PVR_PROBE_COMPUTE_DRAW` and under the
drm-shim, board healthy (vendor + desktop, 0 faults, firmware original). The rasteriser shader is the
guarded variant, so target sizes that are not multiples of the 8x8 local size work (1366x768 tested). The
raw 3D render job still times out because the TA path itself is hardware-refused - the known
fixed-function blocker, not fence plumbing. Status (before sync fix): **draw_vbo now renders the pipeline's own geometry into the
bound render target, through the standard pipe_context API**. The compute fallback is part of the driver
(mesa `774b442`, `171150f`):
- `pvr_compute.c/h`: per-context lazy construction - compile the rasteriser shader with pco from embedded
  SPIR-V, generate the PDS kernel/fence/descriptor programs with libpowervr_pds, allocate every dispatch
  buffer from the real heaps through pvr_vm_heap_alloc (general pages for descriptor set + literal pool,
  draw parameters, CDM control stream, context state, border table; PDS page for the programs; USC page for
  the shader), build the CDM control stream (rebuilt when the dispatch shape changes) and the compute fw
  stream, create a compute context with minimal static state, submit through pvr_job_submit_compute_sync();
- `pvr_context.c`: draw_vbo executes the draw - it takes the three clip-space vertices from the bound
  vertex buffer (vec3 positions, w=1) and the render-target dimensions from the bound framebuffer, patches
  the descriptor set (binding 0 = target, binding 1 = parameters) and submits; the compute state is
  destroyed with the context;
- probe `PVR_PROBE_COMPUTE_DRAW=1` drives it end-to-end through the API:
  case 1: 512x512, no vertex buffer -> inside=73728 (the reference's exact coverage), centre=0xFF0000FF;
  case 2: 256x256 with a full-screen triangle in a bound vertex buffer -> inside=65536/65536 (every pixel).
  Both PASS with 2 draws executed; --- ALL PASS with and without the env var, and under the drm-shim.
Remaining known limitations: the fallback requires target dimensions that are multiples of the 8x8 local
size (pco crashes in pco_cf on an early `return` before a store, so the shader has no bounds guard); the
vertex stage is the fallback's identity transform (vertices are taken as clip-space); kernel fence delivery
to userspace is still broken so tests poll readback; the fixed-function TA/3D path remains unusable.
Status 2026-10-26 (native compute draw WORKS): **the word "replay" is gone - the native driver now
renders the triangle with its own programs and submission**. `PVR_PROBE_CDM_NATIVE` (mesa `774b442`) does
the whole compute draw natively:
- compiles the triangle compute SPIR-V through the native pco pipeline (456-byte USC binary, same as the
  reference);
- generates the compute kernel / fence / descriptor-upload PDS programs with the PDS library (kernel data
  `00007200 00000010 90000002 80000000`, descriptor program `f0040000 f4050100`, descriptor data
  `0004b080 00000080 0004b040 00000080 10000004 90008009`);
- builds the **22-dword CDM control stream** and the **44-byte compute fw stream** from those programs -
  both byte-identical to the reference capture
  (`0000002c 00000000 0001c400 00000020 001b1000 00000080 0006b000 00000080 00900808 ...`,
  PDS layout fence 0x1040/0x1050, descriptor program 0x2600, kernel 0x2640/0x2660, descriptor data 0x2700);
- creates the compute context (a 64-byte stream with zero program pointers is enough - the dispatch does
  not use the reference's context-switch programs; verified by replay first);
- submits through the native VM and reads the target back: **PASS - inside=73728 pixels (exactly the
  reference's count), centre=0xFF0000FF, corner=0xFFFF0000**, i.e. the rasterised triangle is really in the
  target.
The remaining work for the native compute path is the rest of the driver integration (allocate from the
real heaps instead of fixed debug addresses, wire it behind the pipe_context draw path, add the
kernel-sync fix), not the compute pipeline itself. Captures/output in `captures/cdm-native/`.

Status 2026-10-26 (native compute port, continued): **shader, kernel program and descriptor-upload program
all byte-identical to the reference**. The probe's compute check now also generates the descriptor-upload
PDS program and its dispatch data segment from the native compile's pco data:
- desc code: `f0040000 f4050100`
- desc data: `0004b080 00000080 0004b040 00000080 10000004 90008009`
- kernel PDS data: `00007200 00000010 90000002 80000000`
- kernel PDS code prefix: `c2c00005 90012000 90016041 f0024001 d3000000 f0034001 fc000002 d3000000`
All four match the reference capture exactly (mesa `e1e9cd2`). Remaining for the native compute path: the
CDM control stream, the compute fw stream, compute context creation (CSW static state) and the submission
itself. Status (native compute port started): **the native compiler already matches the reference
byte-for-byte**. `pvr_shader_compile_compute()` runs the reference's compute pco pipeline (preprocess ->
descriptor bindings from the NIR -> lower -> postprocess -> compute sysval/shmem/descriptor shared-register
allocation -> trans) and hands back the USC binary plus pco_data. The probe's new
`PVR_PROBE_CDM_COMPILE` mode compiles the triangle-draw SPIR-V with the native compiler:
- **456 bytes of USC code, byte-identical** to the shader the reference uploaded for the same module;
- final pco data temps=7 shareds=13 vtxins=1 coeffs=3 literals=9 wg_id_mask=0x3 (reference capture: 9
  literals);
- the PDS compute kernel program generated with the PDS library matches the captured reference: data
  `00007200 00000010 90000002 80000000`, code prefix `c2c00005 90012000 90016041 f0024001 d3000000 f0034001
  fc000002 d3000000` (the reference's extra code words are uninitialised staging-buffer heap, zeros in
  ours).
Remaining for the native compute path: descriptor/literal upload programs, CDM control stream, compute fw
stream, context creation and submission (the replayed route already renders the triangle for comparison).
Mesa `ed86b1c`. Status (round 87): **FIRST DRAW + READBACK ACHIEVED - from the native driver**. Root cause of
the replay's address anomaly found and fixed: pco loads larger immediates from a **compile-time literal
pool** - a general-heap buffer whose address the descriptor PDS constants point at (for the probe shaders:
offsets 0x20..0x4000; for the triangle: edge-function constants 0xC27E0000/0xC3DFC000/... and the pixel
colours). The replay left that buffer zeroed, so every constant offset >= 8 collapsed to zero. After dumping
pco's literal pool under Vulkan (`PVR_DUMP_LITERALS`) and reconstructing it in the probe at the captured
address, the replayed compute job is exact: the offsets sweep lands all 15 offsets, and **replaying the
captured triangle-draw job renders the triangle in the render target**:
`readback inside=73728 pixels (exactly Vulkan's count), center=0xFF0000FF (inside colour),
corner=0xFFFF0000, marker 0xCAFEBABE` -> `PASS (native compute draw of the captured Vulkan triangle job)`.
So the native driver now submits a GPU job that rasterises a triangle into a 512x512 target and a CPU
readback shows the expected image. This is the compute/CDM route (a captured reference job replayed through
the native driver's own submission path); the fixed-function TA rasterisation path is still
hardware-refused (see MILESTONE2-STALL-REPORT.md). Captures in `captures/cdm-draw/`; mesa `218263b`.
Status (round 86): **constant-offset limit mapped precisely; runtime PDS state matches Vulkan**.
An offsets-sweep shader (stores to px[0,1,2,3,4,8,16,32,64,100,128,256,512,1024,4096]) verifies fully under
Vulkan (15/15). Replaying that captured job from the native driver: offsets 0..4 land correctly and **every
offset >= 8 resolves to 0** (all those stores overwrite px[0]; final value there is the px[4096] store's
0x A000000e). So the constant buffer offsets are not being formed for offsets >= 8 - and the USC code shows
why it is structural: the first five stores are inline (instruction word `ff4N0000` carries the offset), the
later stores go through a helper sequence (branch/call blocks with `ff6a8002`). A new runtime PDS dump hook
(`PVR_DUMP_COMPUTE_PDS_RUNTIME`) shows the compute PDS code/data at dispatch are **identical to the
upload-time capture** (the `0xaaaa` words are encoding, not dispatch patches), so the replay's PDS state is
right; the remaining suspect is the USC helper's scratch/stack state (USC common/unified store) that pco's
runtime helpers expect, which the replay does not reproduce. Next: decode the helper's input register /
where it gets the address operand, then reproduce that state (or port the Vulkan compute stream builder and
dispatch natively instead of replaying). Captures in `captures/cdm-offsets/`; mesa `e81c3a1`.
Status (round 85): **replay store anomaly narrowed to the larger constant buffer offsets**. Two
captured constant-index probes replayed from the native driver:
- six-store shader (`px[0..3]`, `px[100]`, `px[4096]`): Vulkan writes all six; the replay writes `px[0..3]`
  correctly but the `px[100]`/`px[4096]` stores land at offset 0 (the `px[4096]` marker overwrites `px[0]`).
- single-store shader (`px[4096] = 0xCAFEBABE`): Vulkan writes `px[4096]`; the replay writes `0xCAFEBABE`
  at `px[0]`.
So small constant offsets (0..3 dwords) resolve correctly while any larger constant offset collapses to 0:
in the replay the shader's constant-offset addressing is not formed. The captured USC program at
USC+0x7200 is the one Vulkan executes (byte-identical capture; different shaders give different bytes at the
same offset), the descriptor set content is identical `{addr=0x80000b1000,size=0x100000,offset=0}`, and the
target/descriptor/PDS/USC pages are reconstructed at the captured device addresses. Next: find where pco
obtains the larger offsets/immediates - the USC common-store initialisation done by the PDS programs at
0x2600 (empty program) / 0x2650 (kernel program) and the CDM kernel data at 0x2640, which are the only
remaining pieces of per-dispatch state. Captures in `captures/cdm-const/` (vkcdm8/vkcdm9). Mesa `22bccac`.
Status (round 84): **the replayed compute shader executes and its stores land on the GPU**. With
an ID-probe shader (stores 0x11110000|localID, 0x2222000x|groupID, 0x3333/0x4444 patterns and a 0xCAFEBABE
marker at px[4096]) the Vulkan reference behaves exactly as expected (local0=0x11110000, wgx1=0x22220001,
wgy1=0x33330001, gid1=0x44440001, marker at px[4096], PASS). Replaying its captured job from the native
driver now writes 0xCAFEBABE at **target[0]** plus a repeating 1..7 pattern (505 nonzero dwords) - the
CDM/descriptor/USC path runs end to end, so the remaining anomaly is the **store address/value mapping**:
the marker appears at px[0] instead of px[4096], and the probe stores lose their 0x1111/0x2222/0x3333/0x4444
tags. All replay buffers are reconstructed at the captured device addresses, so the open question is the
pco descriptor/base-address interpretation in the replay (next: dump the shader's computed store addresses,
compare the pco descriptor encoding and the CDM task data). Captures in `captures/cdm-ids/`; mesa
`22bccac`. Status (round 83): **native compute (CDM) job now completes on hardware - only the shader's
stores are missing**. Root cause of the previous lock-up found with a clean firmware trace (fresh module +
fw_trace_mask): the CDM kick faulted at **USC+0x7200** (BIF0 Req Status 0x00181fe000007200) - the compute
shader's real USC program lives at 0x7200 (captured, 408 B) but the replay only mapped USC [0x1000,0x2fff).
After extending the reconstructed USC page to 0x1000..0x7fff the firmware now logs
**Kick Compute -> EVENT_STATUS=0x4 -> Compute finished -> UFO update** with no fault and no HWR. The job
completes on hardware. The target still reads back all-zero and a scan of every reconstructed buffer
(target, descriptor page, control stream, context state, border table, PDS page, USC page) shows the GPU
wrote nothing: so the same compute control stream that draws a triangle under Vulkan runs but its USC
shader task apparently does not produce stores in the replayed job (next: compare the successful Vulkan
firmware trace, verify the kernel-program data at PDS 0x2640 (USC offset 0x7200), and check the
workgroup/indirect setup and descriptor constants used by the replay). Mesa `8ea2504`. Status (round 82): **compute (CDM) draw path: reference works, native replay hangs the GPU**.
The reference now has a compute triangle draw (`captures/cdm/vktq_draw.c` + `tri.comp`): a compute shader
rasterises a 512x512 triangle into a storage buffer, and Vulkan verifies it (inside=73728, outside=188416,
bad=0, PASS). Its CDM job was captured with new Vulkan dump hooks (compute fw stream, control stream,
context state BO, static context state, PDS programs, USC programs, descriptor-set constants and the
descriptor set): `captures/cdm/`. The native probe replays it (`PVR_PROBE_CDM_DIR`): it creates a compute
context from the captured static state, reconstructs every referenced buffer at the captured device
addresses (control stream @0x80001b1000, context state @0x800006b000, border table, PDS kernel/fence/
descriptor programs, USC programs, descriptor page @0x800004b000 with the target's pco buffer descriptor,
target @0x80000b1000) and submits the captured fw stream via `pvr_job_submit_compute_sync()`. Result:
submit accepted, firmware activates and kicks DM4 (CDM), but the job **hangs the hardware** (the firmware
logs a lock-up/HWR and sends an unhandled context-reset FWCCB notification, `Received unknown FWCCB
command 2abc0069` = CONTEXT_RESET_NOTIFICATION) and the target reads back zero. So the plumbing works; the
missing piece is in the reconstructed state (CDM context-state BO content / CSW programs / descriptor
constants), not in submission. Board restored, `pvr-probe --- ALL PASS`, firmware original. Mesa `8adbe8c`.
Status (round 81): **full-surface GPU write to the render-target buffer, verified by readback**.
The TQ fill replay gained `PVR_PROBE_TQ_TARGET_ADDR`: the job's destination address is patched into the
pixel-event PDS data (word 2 = addr>>2 low 32 bits; word 3 bits[0:5] = addr>>34) and the buffer at that
address is the target. Using a captured 1 MiB Vulkan fill job (2048x128 RGBA8, `captures/tq512/`), the
native driver's TQ job writes a 512x512 RGBA8 render-target-sized buffer at the RT address 0x9800000000:
readback **changed=262144/262144 dwords, mismatched=0, first=0xdeadbeef, in ~1 ms**, `pvr-probe ---
ALL PASS` (mesa `c2e0c04`). So the native driver can now write the whole render target on the GPU and read
it back correctly - only the rasterisation/TA draw is still missing. Next: a compute-(CDM-)based triangle
draw via the same capture/replay recipe (write a tiny Vulkan compute test, capture its job/PDS/USC, replay
from the native driver into the target) so the "first draw + readback" has a real GPU-rendered image even
while the fixed-function TA kick remains hardware-refused. Status (round 80): **completion-sync investigation continued and parked (no ad-hoc kernel fence
hooks)**. Confirmed with clean tracing: the kernel arms the user sync op with the sched finished fence,
the firmware completes the TQ job, the scheduler frees the job with the finished fence signalled - yet the
user syncobj stays unsignalled (drmSyncobjWait/TimelineWait both time out, query point 0). New data point:
the working Vulkan client's submit/wait sequence is SUBMIT_JOBS -> SYNCOBJ_TRANSFER -> SYNCOBJ_WAIT (it
does not wait directly on the submit's signal syncobj); replicating the transfer-then-wait from the probe
also times out, so the difference is still in how the pvr kernel's signal op is delivered for our submits.
Parked deliberately: the probe verifies GPU writes by readback (TQ fill: changed=4096/4096 mismatched=0),
and further kernel fence hacks are off-limits after they caused two board reboots. The previous round's
SPIR-V vertex shader path (goal's shader clause) is in place and `pvr-probe --- ALL PASS` (shim), same path
runs on the real driver. Status (turn 84): **the goal's vertex-shader clause is done - SPIR-V -> spirv_to_nir -> pco**.
The probe's vertex shader now comes from a SPIR-V module (Input vec3 at location 0; plain Output vec4
decorated BuiltIn Position; assembled with spirv-as) parsed by `spirv_to_nir()` with
`pco_spirv_options()/pco_nir_options()` - the Vulkan driver's path - instead of hand-built NIR. pco compiles
it to a **56-byte USC program with temps=0, vtxins=3, vtxouts=5**: the attribute load survives and the
register allocator no longer aborts; the "byte-identical to a constant shader" guard still passes. Finding:
the glslang form that wraps gl_Position in the `gl_PerVertex` interface block **segfaults**
`nir_split_per_member_structs()` inside `build_member_deref` in this pco build, so the module uses a plain
Output variable (the shape pco's own lowering produces). `pvr-probe --- ALL PASS` (shim) and the same path
runs on the real driver (mesa commit `81ee713`, sources/outputs in `captures/spirv/`). Board rebooted twice
during the round because of two of our own kernel debug hacks (an unbalanced `dma_fence_put` and a static
fence callback: refcount underflow -> `dma_fence_release` NULL deref); both reverted, kernel worktree source
clean again. Status (turn 83): **TQ fill completion-sync root cause narrowed in the kernel (still open)**. Added
temporary kernel prints to `pvr_queue.c` (`PVRDBG TQ ...`), `pvr_sync.c` (`PVRDBG update_fences`/`push_fences`)
and `pvr_queue_free_job`. For the native TQ fill job the kernel: writes the job, wires the user's sync op to
the sched **finished** fence (`update_fences ops=1`, `push_fences`), then the firmware completes the job
(`PVRDBG TQ done fence=... seqno=1 ufo=1`), and the scheduler frees it with
`finished_signaled=1` - i.e. **the sched finished fence does signal**. Despite that, a userspace
`drmSyncobjWait`/`drmSyncobjTimelineWait` on the armed syncobj times out (`ret=-62 ETIME`, query point 0)
for both binary and timeline syncojbs, while the GPU write itself completes and is verified by readback
(`changed=4096/4096 mismatched=0`). This is a kernel-side sync-signal delivery difference between our
submit and Vulkan's (next place to look: `pvr_sync_signal_array_*` / how the signal op is attached when the
job completes before/around `drm_sched_fence_scheduled`). The TQ fill replay remains a verified GPU write;
the readback path is used for verification in the probe. Mesa commit `7fc81e8` (timeline syncobj attempt).
Status (turn 82): **FIRST GPU WRITE FROM THE NATIVE DRIVER, VERIFIED BY READBACK**. The
native probe can now replay a captured working Vulkan TQ_3D fill job end to end: it creates a transfer
context, maps a destination buffer at the same device address the reference used (0x80000b1000), installs
the pixel-event PDS program (PDS+0x2600 / code PDS+0x2620) and the 16-byte TQ fill USC kernel (USC+0x5c00)
at the offsets the captured stream names, and submits the 176-byte captured stream verbatim with the new
`pvr_job_submit_transfer_sync()`. Result (`captures/tq/tqfill-native-run.txt`, trace
`captures/tq/tqfill-native-trace.txt`): firmware accepts the kick - `Kick 3D TQ: FWCtx 0xc002f000` ->
`EVENT_STATUS=0x18` -> **`3D Transfer finished`** - and a CPU readback shows the GPU wrote the whole target:
`changed=4096/4096 mismatched=0 first=0xdeadbeef`; **pvr-probe --- ALL PASS**. Two steps are needed to turn
this into a real draw: (a) the kernel-side completion sync for transfer jobs does not signal yet (the
readback verifies the write; investigate the sync/UFO path), and (b) the TQ fill writes a buffer, so the
next target is a TQ/raster write into the render target, then the TA kick. Mesa commit `c78195f` adds the
transfer submit helper + probe replay. Status (turn 81): **a working Vulkan TQ fill job was produced and captured; native replay is
next**. Wrote a minimal standalone Vulkan test (`captures/tq/vktq_fill.c`: buffer + vkCmdFillBuffer with
0xdeadbeef + fence + host readback) - it **PASSES on the open driver** (`fill: 4096 dwords, changed=1,
mismatched=0, first=0xdeadbeef`), i.e. a real no-TA TQ_3D GPU write works on this stack. Added two
temporary env-gated dump hooks in the open Vulkan driver: `PVR_DUMP_TRANSFER_STREAM=<path>` (winsys
transfer submit; appends `[len][stream]`) and `PVR_DUMP_PDS_PROGRAMS=<path>` (`pvr_gpu_upload_pds`; logs
`data_off/data_dw/code_off/code_dw` + data/code dwords). Captured the fill job:
`captures/tq/vk-tqfill.bin` = 176-byte transfer stream whose decoded fields are a clean fill
(`usc_clear_register0 = 0xdeadbeef`, `isp_render=3`, `pds_bgnd*` zero, event PDS code/data
0x2620/0x2600) and `captures/tq/vk-tqfill-pds.txt` = 37 PDS program uploads including the event
pixel-event program (data 0x2600 / code 0x2620) and the nop/program code blocks. The hooks were reverted
after capture. Next: obtain the fill destination device address (from the test via a device address query
or the PDS data), map a native buffer at that same VA, install the captured PDS programs at the same PDS
offsets (PDS heap bases are identical across client VMs), submit the captured stream on a native transfer
context, and verify sync + readback. Status (turn 80): **3D-without-TA ruled out; TQ transfer job captured as the next GPU-write
path**. New probe branch `PVR_PROBE_FRAG_REAL` submits the real draw's fragment stream alone (no geometry
job): the firmware processes it ("3D check: missing TA" -> "Kick 3D") but the hardware faults immediately
with the same BIF0 fault as the dummy-state fragment run (`Req Status 0x0010650000000000` decodes to
**address 0x0** - a null tile-data pointer, MMU Status 0xd041) and locks up; the readback stays unchanged.
So a 3D job cannot write without TA tile data, i.e. the 3D path needs TA even for a clear. Next path: the
**TQ_3D transfer job** (kernel supports `DRM_PVR_CTX_TYPE_TRANSFER_FRAG` + `DRM_PVR_JOB_TYPE_TRANSFER_FRAG`
-> `ROGUE_FWIF_CCB_CMD_TYPE_TQ_3D`), which is the 3D hardware path Vulkan uses for image copies/clears
without TA. A working Vulkan transfer job stream was captured via a temporary
`PVR_DUMP_TRANSFER_STREAM=<path>` hook in the winsys (`captures/tq/vk-transfer.bin`: 176-byte / 44-dword
csbgen stream from the `vkCmdCopyImageToBuffer` readback, plus `vk-transfer-run.txt` = PASS run). Next:
decode that stream with the kernel's `pvr_cmd_transfer_stream` defs, rewrite its device addresses to
probe-allocated buffers, submit it on a native transfer context, and verify completion + readback.
Status (turn 79): **native vertex shader proven valid on hardware; new plan for milestone 2**.
Two temporary Vulkan-side hooks (`PVR_VK_OUR_VS=1`, `PVR_VK_INJECT_VS_BIN=<file>`) plus a probe-side
`PVR_PROBE_DUMP_VS=<file>` were used to run the native driver's exact 56-byte vertex USC binary on the
working Vulkan client: with the native VS NIR shape and with the native binary injected, Vulkan's TA kick
still reaches `Kick TA` -> `TA finished`, so the native vertex program is a valid USC program and not the
TA-kick trigger (captures: `captures/vs-binary/`). The hooks were reverted after the experiment. Since the
TA path is still refused for the native client while the 3D/fragment path executes, the plan for milestone 2
changes to: implement a **3D/TQ clear job** (the same 3D hardware path Vulkan uses for clears/copies, which
needs no TA) in the native driver, verify completion and a CPU **readback showing the target changed** -
the first native-driver GPU write - then use the same plumbing to attack the remaining TA block. Status
(turn 77): **firmware patching proves the kick doorbell executes for the native client**.
Single-byte immediate patches to the plaintext microMIPS firmware were used to retarget the kick-doorbell
store at 0xc0011cce (instruction 0xc0011cc8 `addiu $v0,$v0,0x400`) to the readable latched register 0xc88:
after the failing native geometry submission `0xc88 = 0x00000001`, exactly the doorbell value - so the
firmware reaches and executes the doorbell write for our client. Control: redirecting the doorbell away
from 0x400 makes the *working Vulkan client fail identically* (all pixels wrong, no TA event), confirming
0x400 is the TA kick doorbell and that missing it reproduces our symptom. New pre-kick capture
(`pvr_kickwidepre`) on fresh module loads shows the native and working Vulkan clients present **identical
pre-kick hardware state** (only free-running timers differ), and a successful Vulkan draw earlier in the
session does not change the native failure. This closes the driver/firmware side conclusively: the hardware
acknowledges an identical TA kick only for the Vulkan client. Captures: `captures/prekick/`. Status (turn 76): **VDM control stream compared semantically** (the last client data path):
reference stream captured at submit time from the open Vulkan driver (`PVR_DUMP_CTRLSTREAM=1`) and from the
native probe (`PVR_DUMP_CTRL=1`). Both are 12 dwords and structurally identical (same PPP state update with
word_count 25, same `VDM_STATE0 = 0x4c000f80`, vs_output_size 5, same index list, same terminate packets);
only per-client PDS data/code offsets/addresses and the shader-dependent `vs_pds_data_size` field differ.
The extended `pvr_kickwide` capture (now also CR 0x2000-0x21ff and 0x3800-0x383f hardware context-state
block) again shows the working client's post-execution state and no configuration difference for the
native client. Every client-supplied byte is now compared and equivalent: CCB/kick/TA block/HWRT/context/
free-list/static areas/SR+PT programs/control stream. The failure remains purely "firmware issues an
identical TA kick; the hardware never acknowledges it". Raw dumps: `captures/ctxstream/`. Status (turn 75): **context-switch PDS programs proven byte-identical to the reference**. The
last executable client-provided programs (SR shared-storing store/load and PT stream-out store/resume,
named by the 192-byte static context state and run by the firmware on every render context switch) were
dumped from both drivers with a temporary `PVR_DUMP_CTXPROG=1` hook (`REF_SR*/REF_PT*` in the open Vulkan
driver, `OUR_SR*/OUR_PT*` in the native probe). Data/code sizes are identical (SR 16/3 dwords, PT store
8/5, PT resume 8/6) and the words are byte-identical except the embedded per-client buffer addresses and
USC task upload offsets. This closes the last long-standing executable-program suspect for the missing TA
acknowledgement. Raw dumps: `captures/ctxprog/`. Status (turn 74): full-window register diff 200 us after the kick (new `pvr_kickwide` debugfs,
all non-zero CR dwords of both firmware-thread banks): the failing native geometry kick and a working
minimal Vulkan geometry kick program **identical** kick registers (only addresses/timestamps differ); the
working client additionally shows the hardware's post-TA state at 200 us (ALIST0 SP=3, MLIST0 SP=9 and
related dm state, i.e. hardware executed the kick), while the native client shows none. The only persistent
control difference in the window is **thread-bank register 0x0348 = 0x1f (native) vs 0x0e (Vulkan)**; the
firmware only reads/saves this register (never writes it), so it reflects a hardware status difference, not
a client-supplied value. With the identical programming and no client-visible difference left, the failure
is now constrained to a hardware state/acknowledgement precondition. Captures: `captures/kickregs/kickwide-*`.
Status (turn 73): **kick handshake fingerprint established** with a new 2 us-resolution
in-kernel capture (`pvr_kickfine`, 256 samples x 2 us after every kick). Working minimal Vulkan
geometry kick: kick registers programmed -> `MTS_BGCTX (0xba0)=1` -> firmware kick-pending mirror
`0xd20` falls 1->0 -> `EVENT_STATUS=0x20` -> cleared. Failing native geometry kick: same kick-register
programming, then `0xd20` stays 1 forever and **no EVENT_STATUS=0x20 ever appears** (no hardware
acknowledgement). The fragment/3D kick in the same native run does reach the hardware, so the client,
VM and kernel path are fine. This localizes the failure precisely to "firmware issues the kick; the
hardware never acknowledges it". Captures: `captures/kickregs/kickfine-{vk,frag}.txt`. Status (turn 72): **hardware kick-programming comparison achieved**. The firmware's MMIO base is
confirmed: `ROGUE_MIPSFW_REGISTERS_VIRTUAL_BASE = 0xcf800000` maps the GPU register block (DT reg
0x01800000+0x8ffff), and per-thread register banks are at CR offset `thread * 0x80000`. Two new safe
debugfs tools in the local module - `pvr_kickregs` (targeted snapshot incl. both firmware-thread banks)
and `pvr_kicksamples` (in-kernel 8-sample x 50 us register timeline captured immediately after every kick,
ring of 4) - were used to compare the TA kick of the failing native client with a working minimal Vulkan
geometry-only kick: **both program the same kick registers** (TPC 0x248/24c, control-stream base
0x408/40c, tail 0x410/414, callstack 0x418/41c, FL base 0x210/214, future flags 0x420=1, 0x430=1,
0xc88=0x202, 0xc98=0x01ff01ff, 0xce0=1 - values identical, addresses differ as expected). The only
difference is the firmware's kick-pending mirror register **0x0d20**: 1 and stuck in the native client vs
0 in the working Vulkan samples - the kick is issued and never acknowledged/cleared by the hardware. The
kick doorbell `(thread<<19)+0x400` is write-only (reads 0 on both). Captures in
`captures/kickregs/`. Status (turn 70): firmware assert capture now works. With `dbg_assert_hwr=1` (module sets
`RGXFWIF_INICFG_ASSERT_ON_HWR_TRIGGER` in the firmware config flags before boot) the firmware asserts when
HWR triggers and writes its source location into the per-thread assert buffers, now dumpable via
`/sys/kernel/debug/dri/1/pvr_assert/pvr_assert`. The failing geometry submission asserts at
`firmware/rogue/rgxfw_hwr.c:3086` (`(gsRGXFWCtl.ui32StateFlags & RGXFW_STATE_ASSERT_ON_HWR_TRIGGER) == 0U`)
- i.e. the firmware's own HWR path fires for the stall; the HWR trace samples signature registers
(USC_UVS0_CHECKSUM 0x5000, PPP_SIGNATURE 0x5020, TE_SIGNATURE 0x5028, VCE_CHECKSUM 0x5030,
PDS_DOUTM_STM_SIGNATURE 0x5060, firmware-only 0x40e0/0x4600) with OldValue == CurrValue. Next: locate the
assert call site in the disassembly (strings/VMAs are known) to map firmware line numbers to code, then
instrument the TA kick path. Board healthy. Turn 69 status: the earlier "firmware is encrypted" conclusion was WRONG -
the firmware is plaintext microMIPS and decodes correctly with **Capstone 5** (`CS_ARCH_MIPS` +
`CS_MODE_MICRO`); GNU objdump's microMIPS decoder simply fails on this image. A public **vendor DDK 24.2
kernel source tree** was cloned (TI `ti-img-rogue-driver`, DDK 24.2.6643903, `/home/radxa/ddk242`) with
full register definitions, firmware-shared structs, log-helper, init-config flags and vendor debug
routines. A new safe debugfs dump of the firmware **sysdata** (fault records with firmware source
path/line, per-thread failed-poll address/mask/count, HWR state/recovery flags) was added and read: after
the failing geometry submission there are no failed polls and no firmware faults - DM2 is blocked waiting
for a hardware interrupt. Tooling and next steps documented in `FIRMWARE-RE-TOOLING.md` (disassembly
recipe, DDK pointers, sysdata map, firmware-patching plan incl. STATEFLAGS_CTRL assert-on-HWR).

Status 2026-10-25 (round 66): the research phase is **closed / blocked**. One more safe negative: the
geometry job's kick flags (FIRST|LAST default, 0, FIRST-only, LAST-only) all time out identically, so no
kernel-visible job-flag variant matters. Every host-visible input, address choice, session/state variant,
power state, boot order, firmware object, static area, generated program and kernel-visible flag has now
been verified equal/irrelevant; the firmware records the TA hang as DM2 UNKNOWNFAILURE/HWR_DM_STALLING and
the hardware never raises an event; the firmware image is in a GPU-decoded private encoding (data plaintext,
code undecodable; no decryption in kernel or vendor module) and the vendor firmware cannot boot under the
open kernel. Resolution requires IMG/vendor input or a decrypted/debug firmware build - see
`MILESTONE2-STALL-REPORT.md`. Round 65b: a crafted vendor-firmware image (vendor ELF + open image's
device-info block + synthetic v3 fw-info, flags OPEN_SOURCE) is **accepted** by the open kernel
(`FW version v1.1 (build 6603887 OS)`) but the vendor firmware never completes the boot handshake
(`*ERROR* Firmware failed to boot`, probe -110) - so the vendor build cannot be trivially run under the
open kernel either; firmware file restored and the experiment failed safely. Status verified 2026-10-25
(round 65): the firmware-info blocks were parsed: the open image is fw-info
**v3** with `PVR_FW_FLAGS_OPEN_SOURCE`, layout table (MIPS_CODE @0xc0000000, exceptions @0x9fc02000, boot
@0xbfc00000, data @0xc0032000, ...), fw 1.1 build 6976702; the vendor image is fw-info **v2** (fw 24.2
build 6603887, same layout shape, larger code). The open image's **data sections contain plaintext
strings** (firmware source paths, KCCB error strings) while its **code section bytes are not valid
MIPS/RISC-V** and the kernel does no decryption - so the GPU decodes the code sections itself and the
firmware's TA-kick decision remains unreadable with host-side tooling. This closes the firmware-analysis
avenue. Round 64: firmware-encoding cross-check + safe firmware-swap test. The vendor
stack's firmware (`/lib/firmware/rgx.fw.36.56.104.183`, loaded by pvrsrvkm) contains **plaintext strings**
(firmware source paths, KCCB/CCB error strings) while its code sections are as undecodable as the open
driver's image -> data plaintext, code encoded. The vendor module (`pvrsrvkm.ko`, not stripped) has no
decryption/decompression code: `OSLoadFirmware` is a plain `firmware_request_nowarn` + 16-byte alloc, so
the GPU itself decodes code. Substituting the vendor image for the open driver's firmware fails cleanly at
probe time with "Unsupported fw info version 2" (open kernel supports fw-info v1) - a safe experiment,
file restored. Round 63: three more safe firmware dumps added to the local module (OS data
incl. the empty SLR log, and the kernel CCB slots/offsets) and one negative: setting
`fw_os_config_flags = 0xF` (the vendor value) at firmware boot does not change the TA-kick failure. KCCB
dump shows `read_offset == write_offset` in both the failing and working runs, so the firmware consumes
the KICK command and DM2 then stalls after it. Round 62: **new firmware observability** - the HWR info
buffer (allocated by
the kernel, filled by the firmware on HWR, never read by the open driver) is now dumpable via
`/sys/kernel/debug/dri/1/pvr_hwr/pvr_hwr` (temporary debugfs in the local module; set
`hwr_debug_dump_limit = ROGUE_FWIF_HWR_DEBUG_DUMP_ALL` in `pvr_fw.c` to get full records). Findings:
working Vulkan has `hwr_counter=0`; the failing geometry kick records **type=UNKNOWNFAILURE, dm=2,**
`event_status=0`, **`hwr_recovery_flags=0x20` (HWR_DM_STALLING)** - i.e. the firmware sees the DM2 thread
stalled with no poll/BIF/MMU signature, while the fragment run records a real `BIF0FAULT` on dm=3 (the
3D hardware did execute). This confirms the TA kick is silently not accepted by the hardware. Escalation
report (`MILESTONE2-STALL-REPORT.md`) updated with the HWR tool and findings. The board reboot at
12:42 was caused by our own delayed-work kick probe (UAF after module unload) - removed; board healthy
(vendor + desktop, 0 faults). Keep artifacts under /home/radxa (`/tmp` is a per-command tmpfs).

Earlier (round 57/58): the last two "our data" candidates are now closed. (1) The
working Vulkan client accepts **the probe's exact TA state block words** (injected words 5-9 =
0x3000/0/0/0x4000/0): `Kick TA` -> `TA finished` - the TA block content is definitively not the trigger.
(2) A fresh in-kernel **CCB + kick-data byte dump** (PVRDBGKICK) of the minimal Vulkan submission vs the
probe shows the CCB commands identical modulo addresses only (hwrt addr, control-stream base,
TPU-border-table address, UFO address; all other 46 dwords equal). With the KCCB return-slot probe also
negative, every byte the kernel hands the firmware for a geometry job is now compared on current builds.
Board healthy (vendor + desktop, 0 faults). No pixel has been drawn yet.

## Where the code is

| thing | path |
|---|---|
| Mesa tree | `/home/radxa/mesa/mesa-main` |
| branch / HEAD | `claude/perf-2026-10-10` @ `b1c775a` (tree clean) |
| driver | `src/gallium/drivers/pvr_native/` (~8.7 k lines) |
| runnable check | `src/gallium/drivers/pvr_native/pvr_probe.c` |
| reference draw capture | `captures/` (read `captures/README.md`) |
| real-GPU run logs | `captures/real-runs/` |
| the why (numbers, ruled-out list) | `NATIVE-GL-DECISION.md` |
| the how (stages, milestones, ports) | `NATIVE-GL-DESIGN.md` |
| vendor driver reference | `src/imagination/vulkan/` (same tree) |

Build: `ninja -C build`
Check (no GPU needed, DRM noop shim):

```
cd /home/radxa/mesa/mesa-main
PVR_SHIM_DEVICE_BVNC=36.56.104.183 \
LD_PRELOAD=$PWD/build/src/imagination/drm-shim/libpowervr_noop_drm_shim.so \
./build/src/gallium/drivers/pvr_native/pvr-probe
```

Expect `--- ALL PASS`, exit 0. Under the shim the syncobj calls have no implementation, so the render
submit reports refusal/incompletion; that is expected there and not a failure of the probe.

Real GPU run (desktop down, board restored afterwards):

```
~/open-perf/open-enter.sh
cd /home/radxa/mesa/mesa-main
PVR_PROBE_TRACE_SYNC=1 ./build/src/gallium/drivers/pvr_native/pvr-probe
~/open-perf/open-exit.sh
```

Expect in `captures/real-runs/probe-real10.txt`: every `SUBMIT_JOBS` accepted, then
`empty-control-stream render DID NOT complete in 2 s` and the same for the real draw. That is
today's blocker, not a regression.

## Why this project exists

The open driver's **Vulkan** is already at vendor parity (cstp 0.86x … vkrender 0.96/0.83/0.92x).
GL is 2.5x slower because zink translates GL→Vulkan. Measured on this board, glmark2
`-b texture -b bump -b effect2d`:

```
vendor native GL        720
zink -> vendor Vulkan   293
zink -> open Vulkan     289
```

The entire gap is the translation layer. Native GL on the open driver is the only way to close it.
Don't re-argue this; `NATIVE-GL-DECISION.md` has the A/B evidence and the list of things already
ruled out by experiment.

## Where it stands

**Done and verified under the noop shim:**

- VS reads the position attribute; `vtxins=3`, `vtxouts=5`; binary differs from a constant-position
  shader
- full VDM vertex state; packing reproduces the captured working draw dword for dword
- TA state block (25 words) matches the capture at all 20 state-determined words; non-indexed index
  list byte-identical (`0x68000600`, TRI_LIST); terminate sequence; fragment stage named through
  `TA_STATE_PDS_SHADERBASE` + a PDS kick program
- the reference's three-job submission pattern (geometry + PARTIAL_RENDER fragment + fragment) with
  syncobj chaining and a CPU completion wait
- **context-switch support** (new, ported from the reference): VDM/GEOM context-state buffers, the
  precompiled USC SR store/load binaries, their PDS launchers, the PT persistent-temp store/resume
  PDS programs, and the populated 192-byte static context state handed to CREATE_CONTEXT. All
  programs/buffers are placed with a per-heap bump allocator and named by heap-relative offsets.
- HWRT dataset values aligned to the reference capture for 512x512: tpc 2048 B (4 KiB clamp removed),
  no RTC, no macrotile array, ISP-merge registers zero, region headers per device formula
- **kernel-side tracing** (out-of-tree module patched with temporary prints, then reverted): every
  `SUBMIT_JOBS` is run and kicked by the kernel, including the combined geometry+fragment kick, and
  the parsed geometry command is dword-identical to the reference's. The fragment command differed;
  its cause (PBE words never built) is fixed - `pvr_csb_pbe_words()` now encodes the three 64-bit PBE
  register words per attachment in the reference's format
- context-state buffers bypass the device cache and the SR buffer is cache-line aligned, as the
  reference marks them (a cached context-save buffer hands the firmware stale state across switches)

**Blocked:** the firmware never completes our render submission, even with an empty control stream.
The kernel accepts every `SUBMIT_JOBS`; no fault, and no GPU-scheduler timeout message appears (the
transient one from earlier malformed runs, "unknown FWCCB command", is gone). The working Vulkan
driver renders through the same kernel/GPU at the same time (vkrender PASS), so the difference is in
this driver's submission data or context setup, not the hardware.

## Reference values captured from the working driver (2026-10-24)

Captured with temporary env-gated dumps in the Vulkan tree (since reverted; raw dumps are in
`captures/real-runs/ref_vkrender_dump2.txt`):

- **Static context state** (192 B = 24 qwords), populated, not zeros:
  q0=0xc0, q1 VDM context-state base, q2=0, q3 GEOM context-state base, then two 10-qword groups:
  store task0/1/2 + two zero words, resume task0/1/2 + two zero words. Task0 packs
  VDMCTRL_PDS_STATE0|STATE1, task1 PDS_STATE2 (code address), task2 TA_STATE_STREAM_OUT1|OUT2.
  Two program sets with distinct addresses.
- **Geometry firmware stream** (40 B): `00000028 00000000 <ctrl addr> <ppp 00000202> 00020000
  20010000 00000000` - same shape as ours; our `pvr_geom_static_stream()` matches it.
- **Fragment firmware stream** (384 B) - same length as ours; both fw streams dumped dword-by-dword.
- **HWRT for 512x512**: tile 16x16, tiles 32x32, mtiles 4x4, tiles/mtile 8x8, tpc_size 2048,
  tpc_stride 0, rgn_hdr_size 256, rgn_headers_stride 1536 (single region header 6, simple-parameter
  format version 2 ⇒ group 2), macrotile array size 0, vheap 384×4 B, ISP-merge values all zero.

The reference code these come from: `pvr_arch_job_context.c` (switch programs + static state),
`pvr_arch_job_render.c` (HWRT sizing), `winsys/powervr/pvr_drm_job_render.c` (static state stream
format), and the kernel's `pvr_stream_defs.c` (what the kernel parses).

## CCCB diff: what the firmware is actually handed (round 5)

With the kernel's CCCB command bytes dumped (temporary `pr_info` in
`pvr_cccb_write_command_with_header`, reverted), our draw and `vkrender_fif`'s can be compared
directly. Findings, all recorded in `captures/real-runs/dmesg-ccb-*.txt`:

- Geometry: identical to the reference except addresses and the frame counter.
- Fragment: the PBE block was wrong. The fragment *firmware stream* carries exactly three 64-bit
  PBE **register** words per attachment - `PBESTATE_REG_WORD0` (memory layout, encoded stride,
  channel swizzles), `REG_WORD1` (clip extents), and a zero word. The target **address and output
  format** are `PBESTATE_STATE_WORD0/1` and are NOT in this stream: the reference puts them in the
  emit words of a per-job pixel-event program (`pvr_arch_job_common.c` `pvr_pbe_state_words`,
  `pvr_arch_cmd_buffer.c` `pvr_sub_cmd_gfx_per_job_fragment_programs_create_and_upload`, which
  compiles the EOT with `pvr_usc_eot()` and wraps it in a PDS DOUTU upload). Our stream had state
  words in place of register words, so the kernel parsed the PBE block into nonsense.
- The kernel's own stream definitions (`pvr_stream_defs.c`, `rogue_fwif_cmd_frag_stream`) confirm the
  fragment stream field order, and `CR_ISP_SCISSOR_BASE` etc. are offset by two dwords each. The
  geometry and fragment stream layouts are byte-verified against `ref_csb_dump.txt`.
- The probe's draw now runs in ONE VM context (the screen's, where the PDS/USC programs are placed);
  it used to submit on a second VM context whose heap addresses were unmapped for those programs.

Reference fragment stream (384 B) for the same 512x512 draw: `captures/real-runs/ref_csb_dump.txt`.

## Round 6: what is now built, and the bisect that narrows the stall

Built and verified under the shim:

- per-job end-of-tile program: `pvr_shader_eot_create()` compiles the EOT shader with pco (the
  equivalent of `pvr_usc_eot`), the probe places the USC binary in the USC heap and the PDS
  pixel-event data segment in the PDS heap, and the fragment stream's `CR_EVENT_PIXEL_PDS_DATA`
  names that data segment (it used to be a hardcoded 0x2780).
- `pvr_csb_pbe_state_words()`: the PBE state words (target address, format, MRT, norm, pack mode)
  for the EOT program.
- HWRT free lists now the reference's: global 512..65536 pages growing 256 at 13%, local fixed at
  the runtime minimum aligned to the free-list size alignment (32 pages). The raw 25-page minimum
  fails CREATE_FREE_LIST with EINVAL (page counts must be a multiple of 4) and every submit fails
  with it.

Then the completion tests (real driver, `captures/real-runs/probe-real22.txt`):

- full render (geometry + PR fragment + fragment): no completion
- geometry-only submission with the draw's stream: no completion
- **minimal geometry job: 40-byte firmware stream dword-identical to the reference, empty control
  stream: no completion**

The minimal job's only inputs are its firmware stream (reference-identical), the context it is
submitted on, the HWRT data set, and the static context state. Since the kernel accepts and kicks it,
whatever stalls is in firmware context/queue initialisation, common to every geometry job.

## Round 7: firmware trace works, and the failure is a single MMU page fault

How to get the firmware's own log (the highest-value tool in this project):

```
sudo sh -c 'echo 0xffffffff > /sys/kernel/debug/dri/1/pvr_params/fw_trace_mask'
# run the driver/test
sudo cat /sys/kernel/debug/dri/1/pvr_fw/trace_0 > trace.txt    # trace_1 is the second FW thread
# reset: echo 0 > .../fw_trace_mask, then set the mask again
```

Findings from the traces (archived: `captures/real-runs/fwtrace-r7h.txt` = our draw,
`fwtrace-vkrender-r7.txt` = the working Vulkan draw, same mask):

1. The probe was poisoning its own runs. A synthetic geometry job with a zeroed control stream makes
   the TA time out or page-fault; the firmware then hardware-resets the GPU and discards every later
   command in the process. None of the previous "never completes" evidence was valid. Fixed: the real
   draw is submitted FIRST, synthetic jobs are behind `PVR_PROBE_DIAGNOSTIC`, and every submitted
   control stream contains at least STREAM_TERMINATE (with `completeonterminate` set on the geometry
   firmware stream - a TA whose stream terminates without that bit waits for more work).
2. Kernel free-list slots: slot 0 = LOCAL, slot 1 = GLOBAL. The probe had them swapped; fixed, and the
   trace now shows "type: 0 ... FL Total Pages 32 (max=32, grow 0)" and
   "type: 1 ... 448 (max=65536, grow 256)" - the same shape as the reference.
3. With the probe fixed, the FIRST real draw geometry job kicks the TA and then immediately
   page-faults:
   `Kick TA: FWCtx ... First kick:1, Last kick:1, CSW resume:0` →
   `MMU fault event: 0x200` / `BIF0 page fault detected (Core 0 MMU Status: 0x0000000000002001
   Req Status: 0x001018da00000000)` → `GPU-0 has locked up`.
   The reference run's equivalent trace line is `Is TA: 1, finished: 1 ... ALIST0 SP = 3, MLIST0 SP = 9`
   with nonzero VCE/TE/ALIST pointers: the TA completes and produces tile lists.
4. The pre-kick TA setup lines are otherwise equivalent between our run and the reference
   (freelists, VHeap/TPC/region-header addresses, FWCtx/RTData, context base); the fault happens
   when the TA starts reading the job's data.

## Round 8: the fault is a zero PDS pointer

- **Fault address decoding:** BIF0 Req Status carries the address in its low 40 bits. Proof: patching
  the geometry stream's control-stream base to an unmapped 0x8000500000 produced
  `Req Status 0x0010098000500000`. The real fault decodes to **0x000000da00000000 = PDS heap base + 0**
  (`fwtrace-r8-labelled.txt`, `fwtrace-r7h.txt`).
- **TA state block mapping** (25 words, from the emission order and csbgen lengths):
  `[0] header, [1] ispctl, [2] ispa, [3] ispb, [4] ispdbsc, [5] PDS_SHADERBASE, [6] PDS_TEXUNICODEBASE,
  [7] PDS_SIZEINFO1, [8] PDS_SIZEINFO2, [9] PDS_UNIFORMDATABASE, [10..11] region clip, [12..17]
  viewport, [18] wclamp, [19] outselects, [20..22] varying, [23] ppp ctrl, [24] stream-out`.
  Against the capture only words 5-9 differ; the reference points TEXUNICODE/UNIFORM at real nop
  programs (0x2780 / 0x28c0). Pointing ours at a real PDS offset did NOT move the fault, so those
  are not the dereferenced pointer.
- **Heap map** (PVR_DUMP_HEAPS): general 0x8000000000, PDS 0xda00000000, USC 0xe000000000,
  RGNHDR 0xdbf0000000 (present on this device!), vis, transfer. PDS+0 is the fault.
- Probe diagnostics added: `PVR_PROBE_PATCH_CTRL=<addr>` (label a fault), `PVR_DUMP_TA`,
  `PVR_DUMP_HEAPS`.

## Round 9: the fault was the probe freeing the static firmware programs

The kernel's static data area table (`pvr_vm.c`): general heap fence at 0, YUV CSC at 0x80; **PDS
heap VDM_SYNC at offset 0 and EOT at 0x80; USC heap VDM_SYNC at offset 0**. The fault address
0xda00000000 is therefore the VDM sync PDS program, which the firmware runs on the first TA kick of
a context. `pvr_static_memory_init()` wrote it, and then the probe called
`pvr_static_memory_fini()` before the draw - unmapping and freeing all three carveouts. The firmware
was executing the sync program from unmapped memory (and the pixel-event program would have been next).

Fixed: the probe now keeps the static memory mapped for the process (`static struct
pvr_static_memory`); a real driver belongs at screen-create time and never frees it.

The firmware trace with that fixed (`captures/real-runs/fwtrace-r9-fixed.txt`):

- no MMU fault any more (that part is solved);
- `Kick TA: ... First kick:1, Last kick:1` is accepted, then DM2 **times out** with
  "USC slots: 0 used by DM2" / "64 empty" and the GPU lockup is a timeout, not a page fault.

So the TA never launches the USC part of the VDM sync/context-resume path. The reference's identical
kick line finishes within ~200 ticks and prints `Is TA: 1, finished: 1 ... ALIST0 SP = 3, MLIST0 SP = 9`.

Also ruled out this round: the context-switch SR/PT task program offsets (labelled PDS+0x500000 in
each, one per run - the fault address never followed), and the TA block's TEXUNICODE/UNIFORMDATABASE
fields. The probe now runs the draw first, has `PVR_PROBE_GEOM_ONLY` (geometry-only submission of the
same stream) and dumps the static area offsets.

## Round 10: static programs byte-identical; the TA itself never starts

- **Static programs proven identical.** Dumped the bytes this driver writes into the static areas
  (`captures/real-runs/ref-static.txt` is the reference's, via a temporary env-gated dump in
  `pvr_winsys_helper.c`, reverted) and our PDS VDM sync / USC VS_NOP / PDS pixel-event areas:
  byte-identical. So the static-memory content is no longer a suspect.
- **HWRT BO flags** now match the reference (free lists + PM mlist `PM_FW_PROTECT`, TPC/vheap/
  macrotile/region headers with no CPU access).
- **Minimal-geometry bisect** (`PVR_PROBE_MINIMAL_GEOM`: geometry stream + STREAM_TERMINATE-only
  control stream, as the first job): still times out. The trace
  (`captures/real-runs/fwtrace-r10-min.txt`) shows the kick line and then no TA event; the reference
  emits `EVENT_STATUS=0x20` / `TA finished` ~120 ticks after its kick.
- TA-terminate block and field values verified against the capture (header 0x02000000, clip
  15/0/15, render_target 0) - identical shape.

The only remaining hardware-visible difference in the job is the TA state block's PDS group:
ours `[5..9] = 0x3000, 0x00000000, 0x00000000, 0x00004000, 0x00000000` vs the capture's
`0x2900, 0x2780, 0x01000000, 0x00804000, 0x28c0` (SHADERBASE, TEXUNICODEBASE, SIZEINFO1, SIZEINFO2,
UNIFORMDATABASE).

## Round 11: same command, same logs, TA still idle

Experiments and results (`captures/real-runs/fwtrace-r11-baseline.txt`):

- The kernel-parsed geometry command the firmware receives is **identical to the reference's 28
  dwords apart from addresses** (context/ctrl-stream/border-table), flags included.
- Our pre-kick firmware trace is **line-for-line identical** to the reference's: freelists
  (`local 32 / global 448 max 65536 grow 256`), TA RTData/buffers, region-header base, ALIST0/MLIST0
  stack pointers, and the kick line itself.
- After the kick: ours has no TA interrupt at all (`EVENT_STATUS=0x00000000`, `RGX DM2 phase
  count=0x00000000`, `USC slots: 0 used by DM2`, then `DM2 timed out`). The reference gets
  `EVENT_STATUS=0x00000020`, `TA finished`, ALIST/MLIST counts, and a TPC flush within ~120 ticks.
- Pointing the TA block's TEXUNICODE/UNIFORMDATABASE at the driver's real fragment PDS program
  (mapped, valid) changes nothing (`PVR_PROBE_PDS_TEX` / `PVR_PROBE_PDS_UNI`). So the PDS group of
  the state block is not what keeps the TA from starting.

That leaves the hardware-side kick itself. The firmware decided to kick and logged it; the hardware
never ran. Things to probe next, in order:

1. **Read the TA kick registers from the kernel right after the kick** (temporary read of the TA
   control/kick registers in the kernel's KCCB/APM path, or a debugfs entry): compare the values the
   firmware wrote for our job against a working Vulkan kick. The firmware logs what it intends, not
   what it wrote; this is the only remaining layer we have not observed.
2. **Check the register-set allocation difference.** Our trace has `Alloc PC set 1 as register range
   [2 - 2]` / `Grab reg set 1`, the reference's has `Alloc PC set 0 [1 - 1]` / `Grab reg set 0`.
   Probably just context ordering, but if hardware kicks reference register set 0 by convention, an
   off-by-one context/register-set mapping would explain a silent no-start. Try making the probe
   create fewer contexts before the draw (or submit from the first context created on a fresh
   device) and compare.
3. **Try the reference's exact submission pattern end-to-end**: geometry + PR fragment + fragment
   with a fragment stream whose PDS background words are real programs (ours are zeros). The TA's
   kick and the PR/fragment's kick are one combined command; a malformed companion could keep the
   whole kick from being issued even though the log line is printed.
4. If hardware-side probes are not possible, instrument the firmware log groups further (fw_trace
   mask already all-on) - check whether a "kick written" style message exists at a different group,
   or use the HWR register dump at timeout to see which DM/unit register pages were touched.

## Old next step (superseded): build the reference's PDS "nop/descriptor" programs for the TA block

The reference fills the TA block's PDS bases from real programs, not zeros:
- `TA_STATE_PDS_SHADERBASE` = the fragment job's PDS pixel-shader program data offset
  (`setup_pds_fragment_program` in pvr_arch_cmd_buffer.c);
- `TA_STATE_PDS_TEXUNICODEBASE` = the descriptor shader state's texunicode program code offset;
- `TA_STATE_PDS_UNIFORMDATABASE` = that state's uniform data offset;
- SIZEINFO1/2 carry the matching sizes.
When those are absent the reference still has a **nop program** for them:
`pvr_device_init_nop_program()` (pvr_arch_device.c) builds it from `pco_usclib_common[FS_NOP_COMMON]`
+ `pvr_pds_generate_pixel_shader_program`, and its data offset is used for exactly these bases.

So: build the nop program in the native driver (the same two calls), place it in the PDS heap, and
point SHADERBASE (and TEXUNICODE/UNIFORM if needed) at it, with SIZEINFO values that match the
reference capture. Then re-run the minimal-geometry test - the TA should emit its finish event if the
PDS group was what kept it from starting. If it still does not, dump the TA/hardware registers from
the kernel's HWR path while the timeout is in progress (the firmware trace already prints HWR
signature registers, see `fwtrace-r10-min.txt`; all-zero values mean the unit never started).

## Old next step (superseded): why does the VDM sync not launch USC

Compare the two programs the firmware runs before the control stream, against the reference's:

1. **The VDM sync PDS program at PDS+0** and its **USC VS_NOP binary at USC+0**. Our bytes come from
   `pvr_pds_kick_usc()` + `pco_usclib_common[VS_NOP_COMMON]`, the same generators the reference's
   `pvr_setup_static_vdm_sync()` uses (`winsys/pvr_winsys_helper.c`). Dump the carveout bytes and
   compare with the reference by making the reference driver's static memory readable (e.g. capture
   the PDS/USC carveouts before first submit in Vulkan), or re-derive both programs in a tiny
   standalone check with the same generator inputs.
2. **What tells the firmware to run it:** the geometry command's `vdm_context_resume_task0_size`
   (0x20010000 in our stream, same as the captured reference) and the static context state's
   store/resume tasks (`captures/real-runs/ref_vkrender_dump2.txt` q4..q23). Our static state matches
   the reference field for field except addresses, so if the programs themselves are byte-identical
   the remaining difference is in the firmware's interpretation of `vdm_context_resume_task0_size`
   (PDS_STATE0 with usc_common_size=1) versus the resume-task words.
3. Also worth checking: the context state buffers (VDM/GEOM) and the callstack - the firmware may
   wait on a context switch that never completes because the resume program never signals. The
   firmware trace's "CSW resume:0" means no switch is recorded for the first kick.

## Old next step (superseded): label-test the remaining PDS pointers

1. Candidates for the zero PDS pointer, in order of likelihood:
   - the **context-switch store/resume task state** (`pds_state1.pds_data_addr`, `pds_state2.pds_code_addr`
     in the static context state: ours are 0x7000/0x7040 etc. for the SR store, 0x8000/0x8040 for the
     load, and the PT programs 0x9000/0xa000/0xb000/0xc000 regions). Patch one at a time to a
     deliberately unmapped PDS offset (e.g. 0x500000) and see whether the BIF0 fault address follows
     it - the same labelled-fault trick used to decode the encoding in round 8;
   - the VDM state's PDS data/code addresses (0x2000/0x1000);
   - the PDS shader base in the TA block (0x3000).
   The probe builds the static state in `pvr_static_ctx_switch_init()` (pvr_static.c); add a
   `PVR_PROBE_PATCH_CTX_TASK` env there that rewrites one word with a labelled address, run once per
   candidate, and read `BIF0 page fault detected ... Req Status` from the firmware trace.
2. Once the field is identified, work out why the firmware dereferences it: the reference's values
   for the same field come from `captures/real-runs/ref_vkrender_dump2.txt`
   (static context state q4..q23). A field the firmware uses but we leave zero is the bug; a field it
   uses with a value we set wrong is a smaller fix.
3. Keep the draw-first probe ordering: one bad job locks the GPU up and silently discards every later
   submission in that process.

## Old next step (superseded): get the faulting address from the kernel

The BIF0 status decode above made the kernel register read unnecessary: the address is already in
the firmware trace. Kept for reference: the BIF0 fault registers can be read from the kernel
(`ROGUE_CR_BIF0_*`) at the APM/interrupt path if a fault ever appears without a trace line.

## Old next step (superseded): find the faulting address, then map it

The MMU status does not print the faulting address; get it, then fix the mapping/placement:

1. **Read the BIF0 fault registers from the kernel.** The hardware latches the faulting address in
   BIF0 MMU registers (`ROGUE_CR_BIF0_MMU_STATUS`/`REQ_STATUS`, and the address registers around
   them - see `pvr_rogue_cr_defs.h` for the BIF block). Add a temporary read of those registers in
   the kernel's APM/interrupt path (or a small debugfs entry) and dump them right after a fault;
   the address is in the BIF request-status/address registers. Then check which BO should have been
   mapped there.
2. **Or pin it down by experiment.** The TA's first fetches are: the VDM control stream, the TA
   (PPP) state blocks it names, the VDM state's PDS data/code addresses, and the PDS attribute
   program/USC vertex shader. Submit variants that change exactly one of those addresses (e.g. point
   the control stream at another mapped BO) and watch the faulting req status change - with the
   tracing setup above each iteration is one probe run.
3. Candidate suspects on inspection: the second `PPP_STATE_UPDATE` (terminate block) address, the
   VDM state's PDS data/code offsets (0x2000/0x1000 in this run), and the fragment stream's
   border-colour-table address. Compare each against a mapped BO in `PVR_PROBE_DIAGNOSTIC` mode
   before touching the stream.

## Old next step (superseded): diff the firmware context the kernel builds

1. **Kernel-side dump of the parsed firmware context.** `pvr_context.c` parses
   `static_context_state` into `static_render_context_state.ctxswitch_regs[]` of the fw render
   context object. Add a temporary dump of that object (and of `fw_geom_registers_caswitch` fields)
   for our context and for `vkrender_fif`'s, exactly as the CCCB/kick traces were done in rounds 4/5.
   This shows whether our 192-byte stream lands where the kernel expects field for field - the last
   unverified boundary between the driver and firmware.
2. If that matches, enable firmware trace (`fw_trace_mask` module parameter + `pvr_fw_trace.c`
   buffers) and watch which stage the firmware never leaves.
3. Re-check the two remaining reference differences on the geometry side while there: our HWRT
   `tpc_size` is 2048 like the reference now, but the kernel still builds the fw HWRT from the
   client's addresses; dump both kernel HWRT fw structs and compare fields (not addresses). Also
   compare the geometry submission's CCCB dwords [2]/[3] (frame counters) and the `flags`/`hwrt`
   fields the kernel stores.

### Kernel tracing setup### Kernel tracing setup### Kernel tracing setup (worked in round 4)

```
cd /home/radxa/kernel-src/powervr           # out-of-tree powervr module source
cp powervr.ko /tmp/powervr.ko.orig          # keep the working module!
# ... edit (dev_info prints), then:
make -C /lib/modules/$(uname -r)/build M=$PWD modules
~/open-perf/open-enter.sh
sudo rmmod powervr && sudo insmod $PWD/powervr.ko
# run probe / vkrender, then `sudo dmesg`
sudo rmmod powervr && sudo insmod /tmp/powervr.ko.orig
~/open-perf/open-exit.sh
```
Traces from round 4: `captures/real-runs/dmesg-probe2.txt` (ours) and
`dmesg-vkrender2.txt` (reference), both with `PVRDBGCMD` lines carrying the parsed command the
kernel hands the firmware.

## The compiler blocker, corrected (keep this)

The earlier handoff said "build the VS from SPIR-V". **Wrong.** The driver passed a zeroed
`pco_data`: `pco_nir_pvi()` lowers an attribute only when `attrib_formats[]` has an entry for its
location, and `trans_load_input_vs()` names the registers through `attribs[]`. `pvr_shader_*` fills
both from the shader's declared inputs, after `pco_lower_nir()` (lowering adds the gl_PointSize
output; a map built earlier misses it and the translator writes it to register 0).

Sub-traps paid for in this area:

- `pco_ra()` forces `hw_vtxins = 0` (`FIXME` upstream), so the entry function's `vtxins` is 0 even
  for a shader that reads attributes; `pvr_shader.vtxins` carries `pco_data.common.vtxins`, the
  driver-side count the hardware state needs.
- `pco_shader` keeps a pointer to its NIR. pvr_shader_compile_data now `ralloc_steal`s the NIR (the
  reference's rule); freeing it from the caller corrupted the heap at later, unrelated allocations.
- PDS const-map entries have different sizes: walk them by `type`.
- PDS/TA state addresses are offsets from their heap base; only the DUTOU execution address is a
  USC-heap offset. The dump tool prints absolute values by adding the base back.

## Safety (non-negotiable)

- Never run a GPU compositor (weston/KDE-GL) with `pvrsrvkm` bound — it NULL-derefs the kernel.
- Never hand-roll a driver switch; use `~/open-perf/open-enter.sh` and `open-exit.sh`
  (alive-compositor abort + refcount-0 gate, which took 36 s to clear once).
- Switch back to `pvrsrvkm` **before** restarting display-manager.
- GPU hangs surface as board resets, not clean errnos: small surface, one draw at a time.

Board is healthy, `pvrsrvkm` bound, kwin alive (verified after the real-GPU round).

## Firmware provenance (CORRECTED 2026-10-27): the open firmware is the official IMG 36.56 build

The board's `/lib/firmware/powervr/rogue_36.56.104.183_v1.fw` (sha256 `1db1c399...`) is byte-identical to
`powervr/rogue_36.56.104.183_v1.fw` in Imagination's own linux-firmware fork, branch `powervr`
(https://gitlab.freedesktop.org/imagination/linux-firmware, commit `8a58f818`). The earlier "relabelled 36.52
build" conclusion was wrong - the five-byte difference from the 36.52 image is the normal difference between
two IMG build configurations. No new firmware build is needed; the remaining IMG ask is only firmware-side
observability/documentation for the DM2/TA stall (see docs/IMG-FIRMWARE-REQUEST-36.56.104.183.md), and the
community out-of-tree driver `apercel` (upstream + 30 backported drm/imagination fixes) was tested with this
firmware and shows the same native TA stall, so driver fixes are ruled out too.

## Continual TA-unblock watch (2026-10-27)

`tools/firmware/check-open-firmware.sh` checks linux-firmware's powervr directory for a genuine open-ABI
`rogue_36.56.104.183_v1.fw` (or an updated BXM build) and prints download/install/test steps if one appears.
Last check: not published (only 33.15.11.3, 36.52.104.182, 36.53.104.796). Run it on each work cycle while
the goal is open; if a genuine image appears, install it and re-run the native geometry probe - a fix for the
DM2/TA stall would show up as `TA finished` in `/sys/kernel/debug/dri/1/pvr_fw/trace_0`.

In-board differentials for the DM2/TA stall are now all closed (firmware-visible state, command streams,
HWRT dataset, context creation args, FWCtx/RTData, registers, full-mask trace): the failing native client
never gets the hardware DM2 completion event 0x20 while the firmware trace is identical to the working
Vulkan client up to the kick. The remaining requirement is IMG-side firmware input; the request text is
finalised in `docs/IMG-FIRMWARE-REQUEST-36.56.104.183.md`.

## Tool map (2026-10-27)

`tools/README.md` indexes the board diagnostics: GPU MMIO readers/timelines (`tools/gpu-regs/`), the kernel
firmware-VA debugfs patch (`tools/kernel/`), the verify/measurement scripts (`tools/verify/`), and the
firmware watch + IMG bundle scripts (`tools/firmware/`). The two temporary kernel-module builds
(`/home/radxa/powervr.fwva.ko` - firmware-VA reads; `/home/radxa/powervr.trace.ko` - enlarged trace ring) are
left in place for reuse; both are built from the patched source and never installed permanently.
