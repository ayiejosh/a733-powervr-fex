# Timeline migration, step 2 — mechanical specification (GEOM first)

**Step 1 is landed and verified behaviour-identical**: `struct pvr_queue` carries
`struct vk_sync *job_sync[PVR_JOB_TYPE_MAX]` and `uint64_t job_value[PVR_JOB_TYPE_MAX]`
(`pvr_queue.h:47-48`), created in `pvr_queue_init` (`pvr_arch_queue.c:156-164`), destroyed in
`pvr_queue_finish` (`:228-231`). Both are declared but **never used**, so behaviour is unchanged.

**Why this is worth doing**: the per-job binary syncobjs cost ~190 ioctls/frame (52 CREATE + 52 DESTROY +
72 TRANSFER), which is the measured **84% of frame time spent in the kernel**. A timeline syncobj carries
many points on one object, so the per-job create/transfer/destroy pairs disappear.

## The exact sites for GEOM

| # | site | change |
|---|---|---|
| 1 | `pvr_arch_queue.c:295-299` | delete `vk_sync_create(..., &ws->syncobj_type, 0U, 0UL, &geom_signal_sync)`. Replace with `geom_signal_sync = queue->job_sync[PVR_JOB_TYPE_GEOM];` and `queue->job_value[PVR_JOB_TYPE_GEOM]++;` |
| 2 | `:335` (split-submit path) | wait on GEOM: `.wait_value = queue->job_value[PVR_JOB_TYPE_GEOM] - 1` (still `.sync = queue->job_sync[PVR_JOB_TYPE_GEOM]`) |
| 3 | `:363` (main submit) | same as #2 for the GEOM wait |
| 4 | `:365` | signal: `&(const struct vk_sync_signal){ .sync = geom_signal_sync, .signal_value = queue->job_value[PVR_JOB_TYPE_GEOM] }` |
| 5 | `:374` | **delete** `pvr_update_job_syncs(device, queue, geom_signal_sync, PVR_JOB_TYPE_GEOM)` — a persistent sync must not be stored or destroyed |
| 6 | `err_destroy_geom_sync:` label | **delete** the `vk_sync_destroy(&device->vk, geom_signal_sync)` — cannot destroy a persistent sync |
| 7 | `pvr_update_job_syncs` (`:259`) | must not destroy a sync that is one of `queue->job_sync[]` |

**Wait-value subtlety**: values must be monotonic per type across all submits to that queue, including the
split-submit path. Note the split path calls `pvr_arch_render_job_submit` **twice** but passes signals
**only on the second call** (`NULL, NULL` on the first), so one point advance per job is correct.

## Mandatory gate after step 2

`git checkout --` is the escape hatch. Then rebuild and run **all** of:

* `bda` PASS(0) · `vk13` PASS · `pctest` PASS(0) · `vk16` PASS
* `vkrender` 512 and 2048 → all pixels correct
* **`glmark2-es2 --validate` → 27 scenes**
* a real composited run at 640×480

**A wrong timeline point surfaces as wrong rendering or a hang, so this suite is a real check, not a
formality.** Then measure the win: client `sys` time per frame (baseline ~4.97 s over ~200 frames) and
syncobj ioctls/frame (baseline ~190).

## Why it was not executed in this round

The previous attempt at doing this in one pass **aborted after making a real type error** — assigning
`PVR_JOB_TYPE_FRAG` to `geom_signal_sync` and vice versa, because both creates precede the second render
submit — and left a state where surviving destroys could free the *persistent* syncobjs, i.e. a
use-after-free on the queue's own ordering objects. **The failure mode of getting this wrong is a GPU hang
or a corruption, not a slow frame.**

**Do step 2 for GEOM alone, verify with the full suite, then repeat for FRAG, COMPUTE, TRANSFER, QUERY, then
the event paths, then delete the per-job syncs.** One type per step, verified each time.
