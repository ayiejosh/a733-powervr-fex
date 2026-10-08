# The sync architecture gap: vendor 0 ioctls vs open 1 ioctl per operation

## What was found

The two winsyses in the same Mesa driver implement `vk_sync` completely differently:

| | pvrsrvkm (vendor path) | powervr (open/DRM path) |
|---|---|---|
| file | `pvrsrvkm/pvr_srv_sync.c` (276 lines) | uses Mesa's `vk_drm_syncobj` |
| representation | `struct { bool signaled; int fd; }` - **pure userspace state** | one DRM syncobj per `vk_sync` |
| ioctls per op | **0** (grep for PVRSRV/ioctl/Bridge/syncobj in that file: 0 hits) | 1 per op (`SYNCOBJ_CREATE`/`TRANSFER`/`DESTROY`/`RESET`/`WAIT`) |
| features | BINARY, GPU_WAIT, GPU_MULTI_WAIT, CPU_WAIT, CPU_RESET, CPU_SIGNAL, WAIT_ANY | binary only until this session added the timeline type |

`pvr_srv_sync_signal()` and `_reset()` are literally `srv_sync->signaled = true/false`. The GPU side is
carried by a sync_file fd that the **kernel driver** hands back after a submit
(`pvr_srv_set_sync_payload(srv_sync, payload)`), so the CPU never issues a sync ioctl at all.

## Why it matters, measured

Xwayland per client frame: **~190 syncobj ioctls** (56.8 `TRANSFER`, 41.6 `CREATE`, 41.7 `DESTROY`,
~10 `TIMELINE_WAIT`) out of ~250 DRM ioctls total. At ~60 us each that is **~10 ms of the 16.63 ms
of Xwayland CPU per frame at 800x600**, and half of Xwayland's CPU is kernel time.

## Why the queue generates that many

`pvr_arch_queue.c` creates a sync per job and destroys the previous ones:

```c
static void pvr_update_job_syncs(...) {
   if (queue->next_job_wait_sync[t]) { vk_sync_destroy(...); ... = NULL; }
   if (queue->last_job_signal_sync[t]) vk_sync_destroy(...);
   queue->last_job_signal_sync[t] = new_signal_sync;
}
```

9 `vk_sync_create` sites and 17 `vk_sync_destroy` sites, i.e. ~3 syncobj ioctls per job
(1 create + 2 destroys), and 3 jobs per render submit.

## The unlock, and why it is not a one-liner

The mainline UAPI carries a **timeline point** (`struct drm_pvr_sync_op.value`), so one syncobj can
carry many points and the create/destroy/transfer churn disappears. But today the winsys hardcodes it:

```
pvr_drm_job_render.c   10 sites of  .value = 0,
pvr_drm_job_render.c    8 sites of  assert(!(x->flags & VK_SYNC_IS_TIMELINE));
pvr_drm_job_compute.c / pvr_drm_job_transfer.c  same pattern
```

and `vk_sync_get_value()` cannot be used to fill it in: it asserts `VK_SYNC_IS_TIMELINE` and its
implementation issues a **`DRM_IOCTL_SYNCOBJ_QUERY`** - so querying the value would add an ioctl per
op, defeating the purpose.

**Therefore the value must be tracked, not queried**, which requires:

1. `struct pvr_winsys_render_submit_info` (and the compute/transfer equivalents) to carry a value
   alongside each `struct vk_sync *wait` / signal sync - today they are bare pointers
   (`pvr_winsys.h:346  struct vk_sync *wait;`).
2. `pvr_arch_queue.c` to keep a persistent timeline sync per job type and increment its point per
   job, instead of creating and destroying.
3. The winsys to pass the tracked point as `.value` and drop the 8 asserts.

**This was attempted as a partial change and reverted deliberately:** removing the asserts alone
would let a timeline sync through with `.value = 0`, signalling the wrong point - silent corruption.
The three steps have to land together.

## Payoff estimate

Eliminating ~190 of ~250 ioctls per frame removes roughly 10 ms of a 53 ms display loop on the X11
path - on the order of +15-20%, not the 18x that the copy/flip issue is worth. **It is the correct
architectural fix for the open driver, but it is not the biggest lever.**

## Churn breakdown, measured (Xwayland, windowed client, 800x600)

Instrumented `pvr_drm_winsys_null_job_submit` (`PVR_SUBMIT_MIX=1`):

```
null submits: ~16,400 in 22 s = ~745/s = ~17.7 per client frame
distribution: waits:0=0   1=5302 (33%)   2=10217 (63%)   3+=687 (4%)
```

| null-path case | ioctls per call | per client frame |
|---|---|---|
| waits:1 (33%) | 1 `SYNCOBJ_TRANSFER` (already optimal) | ~5.9 |
| waits:2 (63%) | `CREATE` + 2 `TRANSFER` + `DESTROY` | ~34 |
| waits:3+ (4%) | `CREATE` + N `TRANSFER` + `DESTROY` | ~3 |

The waits:2/3+ cases exist because the driver emulates *"signal dst when all waits complete"* by
transferring each wait into a **temporary syncobj** and then transferring that to the destination -
a binary syncobj cannot accumulate. A timeline syncobj can, which is exactly what
`dma_fence_chain` provides kernel-side.

**So of Xwayland's ~250 DRM ioctls per frame, the null path is ~52 (~20%).** The remaining ~80% is the
queue's per-job sync create/destroy (3 ioctls per job: 1 create + 2 destroys), which is why the local
null-path fix is not worth landing on its own - it addresses a fifth of the problem.

Both are removed by the same change: **timeline syncobjs end-to-end**, so no create/destroy per job and
no transfer-based dependency emulation.
