# Fresh Vulkan A/B matrix (2026-10-27, current Mesa HEAD)

Two block.sh rounds per stack (vendor first then open, then open then vendor); medians.

| test | vendor | open | open/vendor (ms lower better, fps higher better) |
|---|---|---|---|
| r512 (ms) | 0.367 | 0.493 | **1.34x slower** |
| r2048 (ms) | 2.308 | 2.016 | **0.87x faster** |
| b512 (ms) | 0.643 | 0.776 | **1.21x slower** |
| b2048 (ms) | 5.640 | 5.421 | 0.96x |
| c512 (ms) | 0.265 | 0.246 | 0.93x |
| c2048 (ms) | 2.702 | 2.534 | 0.94x |
| heavy (fps) | 179.98 | 172.32 | 0.96x |
| cstpf (fps) | 145.7 | 144.05 | 0.99x |
| cstpi (fps) | 146.45 | 151.20 | 1.03x |
| cstp (fps) | 369.35 | 441.05 | **1.19x faster** |
| many512 (ms) | 0.851 | 0.878 | 1.03x |
| many2048 (ms) | 6.159 | 6.080 | 0.99x |

Interpretation: overall still parity, but the **small-draw / fixed-overhead workloads (r512, b512) are now 21-34%
behind vendor**, whereas the older matrix (Mesa `c737360`, doc note said it predated the last 12 commits) had
them near parity (+/-6%). The larger workloads are equal or ahead (r2048 13% faster, cstp 19% faster). Since
the open Vulkan code was not touched by the recent pvr_native work, this looks like a CPU-side submission
overhead regression introduced by the recent Mesa perf-series commits - a concrete next target. Raw rounds:
captures/ab-2026-10-27/ab-round{1,2}.txt.

## Follow-up: r512 CPU-vs-wall measurement (same session)

Matched `MODE=render ./vkrender 512 600` runs with rusage CPU accounting (`tools/verify/measure_cpu.py`):
- vendor: 0.407 / 0.450 / 0.413 ms/frame, CPU 0.106-0.125 s per 600 frames (~48% of wall)
- open:   0.479 / 0.483 / 0.490 / 0.519 / 0.522 ms/frame, CPU 0.112-0.184 s per 600 frames
So the CPU time per frame is comparable; the wall-time difference (open ~15-25% slower at this tiny 512x512
render) is on the GPU/submission-timeline side, not a host-CPU regression as first suspected. Absolute values
drift run to run (vendor 0.35-0.45, open 0.44-0.55 across the session), so this needs a dedicated alternating
A/B session with proper driver-switch settling (switch-vendor alone leaves vendor Vulkan failing to
enumerate until display-manager has started once) before drawing conclusions. Flagged as open perf item, not
a confirmed regression.

## Settled alternating A/B for r512 (same-session, order-controlled)

Sequence: vendor V1 -> open O1 -> vendor V2 (driver switches with the proper scripts; desktop stopped for
measurements; each cell is 3x `MODE=render vkrender 512 600`).

| block | ms/frame (median) | CPU per 600 frames |
|---|---|---|
| vendor V1 | 0.413 | 0.095-0.099 s |
| open O1 | 0.457 | 0.110-0.125 s |
| vendor V2 | 0.421 | 0.099-0.115 s |

So the settled small-draw gap is **~10%** (open slower), not the 21-34% the first noisy pass suggested; CPU
per frame is comparable (~0.19 ms both), so the difference is on the GPU/submission-timeline side. b512
vendor V2 = 0.697 ms vs open 0.78 ms earlier -> ~12% gap. Next step if pursued: compare DRM ioctl/submission
counts per frame (strace -c) between the two stacks to see whether the open winsys issues more kernel work per
frame; the earlier matrix (Mesa c737360) had this case at parity, so a small regression is plausible but not
yet localised.

## ioctl accounting per frame (strace -c / -e trace=ioctl, MODE=render, 120/20 frames)

Total DRM ioctls per frame:
- vendor: 677 ioctls / 120 frames = **5.6 per frame**
- open:   1933 ioctls / 120 frames = **16.1 per frame** (~3x)

Open-stack breakdown (20-frame tally): 20x PVR_SUBMIT_JOBS (1/frame), 83x SYNCOBJ_CREATE, 79x
SYNCOBJ_TRANSFER, 79x SYNCOBJ_DESTROY, 22x SYNCOBJ_WAIT, 20x SYNCOBJ_RESET, 38x PVR_CREATE_BO and 38x
PVR_VM_MAP (~2 BO create+map pairs per frame). Vendor command names decode unreliably (vendor DDK ioctl
numbers collide with other DRM drivers in strace), so only the total count is comparable, and it shows the
open winsys does roughly 4 syncobj ops and 2 BO create+map pairs per frame that the vendor path does not.
Frame-count differencing (5 vs 25 frames) shows the BO create/map pairs are **one-time device init** (38 both
runs), so the per-frame cost is syncobj churn: `SYNCOBJ_CREATE` 4/frame, `SYNCOBJ_TRANSFER` 4/frame,
`SYNCOBJ_DESTROY` 4/frame, plus ~1 `SYNCOBJ_WAIT` and ~1 `SYNCOBJ_RESET` per frame (the last two are the
app's fence wait/reset). The 4 create/transfer/destroy cycles are Mesa runtime/Vulkan-side temp-sync
traffic (vk_drm_syncobj transfer_payloads creates a temporary syncobj for the many-to-many case); the winsys
render-submit path itself only fills sync ops. Found it with an LD_PRELOAD chain shim (`tools/verify/syncobj_chain_shim.c`, counts drmSyncobjCreate call
stacks and resolves them with addr2line):
- 4 creates/frame come from `vk_sync_create` called inside **`pvr_process_cmd_buffer`**
  (`pvr_arch_queue.c`), reached from `pvr_driver_queue_submit <- vk_queue_submit <- vk_common_QueueSubmit` -
  i.e. the driver creates temporary vk_sync (DRM syncobj) objects per command-buffer barrier/event while
  processing the frame's one vkQueueSubmit;
- 1 create/frame comes from `pvr_process_event_cmd_barrier` -> null-job submit path (also in
  pvr_arch_queue.c).
The vendor driver expresses the same dependencies inside its kernel driver with no extra DRM ioctls. So the
small-draw overhead is Mesa Vulkan's per-barrier **temporary syncobj churn** (create+transfer+destroy plus a
null/transfer job) in `pvr_arch_queue.c`. Next step: reduce that churn - e.g. reuse/timeline-ify the queue's
stage sync objects instead of create/destroy per submission, or skip null-job signalling when the signal
sync has no consumer.

## Mechanism and proposed fix (code-level reading)

`pvr_update_job_syncs()` (`pvr_arch_queue.c`) destroys the previous `next_job_wait_sync[type]` and
`last_job_signal_sync[type]` and stores the freshly created signal sync after every sub-command submit; the
render submit path creates two `vk_sync` objects per render sub-command (geometry + fragment signals, via
`vk_sync_create` -> DRM_IOCTL_SYNCOBJ_CREATE), and `pvr_process_event_cmd_barrier` creates one more for its
null-job signalling (only when there are src jobs to wait for). Each signal sync is later destroyed and
replaced. On this SoC each ioctl costs a few microseconds, so ~4 create+destroy pairs/frame roughly accounts
for the measured ~10% gap at 512x512 (0.41 ms vendor vs 0.46 ms open).

Proposed optimization (not implemented; needs GPU validation):
- keep a small per-queue pool of binary `vk_sync` objects instead of create/destroy each submission, and reset
  a recycled DRM syncobj (SYNCOBJ_RESET, 1 ioctl) instead of CREATE+DESTROY (2 ioctls) - saves ~2 ioctls per
  sync (the render path makes 2/frame, plus barrier syncs);
- check whether the `pvr_process_event_cmd_barrier` null-job signalling can be folded into the following real
  job's wait list (removing a create+transfer+destroy cycle and one null-job submit per barrier);
- validate with the A/B block plus a correctness run of the Vulkan CTS/scene checks before adopting.

This is peripheral to the goal objective (native driver TA/frontend), so it is recorded for the performance
track rather than pursued further in these cycles.

## Why the obvious sync-pool "fix" is unsafe

A per-queue pool that recycles the transient `vk_sync` objects via `vk_sync_reset()` instead of
create/destroy was considered and rejected: the syncs being replaced in `pvr_update_job_syncs()` may still be
the signal target of the just-submitted GPU job (the kernel job holds its own reference to the DRM syncobj).
The current code destroys the userspace handle at that point, which is safe because a fresh create gets a new
kernel object; recycling would keep the same kernel object and could deliver the previous job's delayed signal
into the new submission's chain. A safe optimisation therefore needs either
(a) reuse only after the old job's completion (extra tracking + waits, likely cancels the saving), or
(b) per-job sync handling moved into the submission such that no cross-submission sync objects exist.
Not pursued; recorded as a dead end so it is not re-attempted blindly.

## Upstream Mesa watch (2026-10-27, useful for the small-draw overhead)

Our branch is `claude/perf-2026-10-10` (base commit `9a2b374`). Upstream commits/MRs touching `src/imagination`
after that base that are worth evaluating for the sync-barrier overhead:
- open MR **!44826 "pvr: translate external subpass dependencies into barriers"** (2026-10-09) - changes how
  subpass dependencies become barriers, i.e. exactly the code path that creates the per-frame sync objects;
- merged **`a8dd2d8c` "pvr: advertise VK_KHR_internally_synchronized_queues"** (2026-10-20) - internally
  synchronised queues can remove host-side sync bookkeeping for some patterns;
- merged pco/pvr fixes through 2026-10-23 (pco barrier/sequence counter, blend consts, etc.) - not sync related.
None of these are backported here (the branch is a curated perf series and a Mesa update needs a full
validation pass); they are the first candidates if the Vulkan small-draw overhead is pursued further.
