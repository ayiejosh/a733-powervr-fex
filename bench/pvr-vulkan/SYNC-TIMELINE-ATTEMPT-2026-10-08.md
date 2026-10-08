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
