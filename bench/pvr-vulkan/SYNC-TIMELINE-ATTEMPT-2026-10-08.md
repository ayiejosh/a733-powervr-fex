# Timeline-syncobj queue: attempt, abort, and the corrected plan

## Why

Target (1): Xwayland burns 56.8% of a core issuing ~250 DRM ioctls per client frame, ~190 of them
syncobj, measured as 56.8 TRANSFER + 41.6 CREATE + 41.7 DESTROY. The queue creates and destroys a
`vk_sync` per job. The vendor uses a driver-native sync type (`pvr_srv_sync`) at **0 ioctls/op**
where the open path costs 1.

## What is already done and landed

`53ccc4b` - **step 1 of 3, behaviour-preserving**: sync values plumbed through the winsys submit
API. The API carried bare `struct vk_sync *`, so a timeline point could not be expressed;
`pvr_drm_job_render.c` hardcoded `.value = 0` at 10 sites and asserted against timeline syncs at 8.
Now `struct vk_sync_wait` / `struct vk_sync_signal` are carried throughout. Values are all still
zero, which is exactly correct for a binary syncobj, so no behaviour changed (13 probes + validate
27/0 confirm).

## What I attempted this round and ABORTED

The full queue change in one pass: persistent timeline syncobj per job type, created in
`pvr_queue_init` and destroyed in `pvr_queue_finish`, with `job_value[type]` advancing per job so
each job waits point N and signals point N+1.

**Reverted.** Two concrete reasons:

1. **I made a real type error.** A string-anchor transformation assigned `PVR_JOB_TYPE_FRAG` to
   `geom_signal_sync` and vice versa, because both creates precede the second render submit. I
   caught it, but it shows the approach is unreliable at this scale.
2. **The half-applied state was actively dangerous, not merely incomplete.** `pvr_update_job_syncs`
   had stopped destroying, while four sites still created per-job syncs (leak), and the surviving
   destroy calls would have freed the *persistent* syncobjs - a use-after-free on the queue's own
   ordering objects. Leaving that on disk was not acceptable.

**The 72-site surface is the problem, not the design.** Changing create/destroy/value sites across
9 creates, 17 destroys and ~6 submits by string matching is fragile; several destroys are event
syncs (`sub_cmd->event->sync`) that must survive, and one create writes straight into
`queue->next_job_wait_sync[i]`.

## Corrected plan: four small verified commits instead of one big one

1. **Add the persistent syncs, unused.** `job_sync[]` + `job_value[]` in `pvr_queue.h`, created in
   `pvr_queue_init`, destroyed in `pvr_queue_finish`. Nothing else changes; the queue still uses
   per-job syncs. Build + probes + validate. Behaviour-identical.
2. **One job type at a time.** Switch GEOM, then FRAG, then COMPUTE, then TRANSFER, then QUERY:
   each path's create -> `pvr_queue_advance_job_sync()`, its destroy removed, its wait/signal values
   set to `job_value[type]-1` / `job_value[type]`. Verify after each - a wrong timeline point shows
   up as wrong rendering or a hang, so the probe suite is a real check.
3. **The event paths last** (barrier, set_or_reset, wait, queue_waits) - these are per-`stage` and
   one writes directly into the alias array, so they are the fiddliest.
4. **Drop the per-job syncs entirely** once no path creates one.

Edits to be made by line number, not by string anchor, and `git checkout --` kept as the escape
hatch. Each step must leave `pvr_arch_queue.c` self-consistent: never a state where some paths
create per-job syncs and others use the persistent ones, and never a destroy that can reach a
persistent syncobj.

## Measured baseline to beat

`53ccc4b`: validate 27/0, all 13 probes pass. Xwayland syncobj ioctls/frame: ~190
(56.8 TRANSFER, 41.6 CREATE, 41.7 DESTROY). The change should remove ~83 (CREATE+DESTROY) first,
then the TRANSFER chaining once the null-job path can use the timeline directly.

---

# The real ordering: the winsys must learn timelines FIRST

Migrating GEOM alone (the plan's step 2) **segfaulted glmark2 immediately** - `--validate` returned
0 success / 0 failure because the process died, and the mrt/varyings/inatt/vattrib probes produced
no output. Reverted.

**Why, and this is the finding:** the queue handed a timeline syncobj to the powervr winsys, which
cannot express one:

```
pvr_drm_job_render.c    5x assert(!(... VK_SYNC_IS_TIMELINE))
                        5x .flags = ... | DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ
                        5x .value = 0
pvr_drm_job_compute.c   2x assert, 2x SYNCOBJ flag, 2x .value = 0
pvr_drm_job_transfer.c  2x assert, 2x SYNCOBJ flag, 2x .value = 0
```

The asserts are compiled out in a release build, so the sync was submitted as a **binary** syncobj at
**value 0** while the queue's timeline points advanced independently - the ordering was lost and the
driver crashed. `DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ` exists in the UAPI
(`/usr/include/drm/pvr_drm.h:1058`, value 1) and the **null-job path already uses it correctly**
(`pvr_drm_job_null.c` passes `signal_sync->signal_value` and `waits[i].wait_value`), so the pattern
to copy is in-tree.

## Corrected order

1. ~~winsys value plumbing~~ - **done** (`53ccc4b`).
2. ~~persistent syncobjs exist, unused~~ - **done** (`de755dc`).
3. **NEW: teach the three winsys submit paths timelines.** For each sync op, choose the flag and
   value from the sync type:
   ```
   timeline:  DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ, .value = <the point>
   binary:    DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ,          .value = 0
   ```
   and drop the 9 asserts. This is still **behaviour-preserving** while every queue sync is binary
   (values 0), so it can be verified on its own before anything migrates.
4. **Then** migrate the job types, one at a time, as before.

## What the failed attempt still bought

* It proved the migration path is wrong until step 3 lands - cheaper to learn from a revert than
  from a corrupted frame later.
* It located the exact code (3 files, 9 asserts, ~20 hardcoded zero values) and confirmed the UAPI
  flag and the in-tree reference implementation both exist.
* `de755dc` is a real, green, independent step.

**Lesson, recorded because it cost two rounds: `struct vk_sync_wait`/`vk_sync_signal` carrying a
value is necessary but not sufficient - every consumer of that value must be updated too, and the
winsys is a consumer. Grep for the value's consumers, not just its producers, before migrating a
producer.**
