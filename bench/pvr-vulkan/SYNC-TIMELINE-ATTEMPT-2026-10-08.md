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

---

# Blocker 2: vk_sync_as_drm_syncobj() refuses timeline syncs by design

After step 3 (`e9d1b2a`, winsys timeline support) the GEOM migration still segfaulted. Instrumenting
located it exactly:

```
[rsub] si=... gw=(nil) gww=0 gs=... gss=0xaaaaeb0ff200
[op] op=... sync=0xaaaaeb0ff200 flags=0
SIGSEGV: x0=0, ldr w0, [x0, #16]      <- reading .syncobj of a NULL vk_drm_syncobj
```

`sync` was valid. The NULL comes from the accessor itself:

```c
static inline struct vk_drm_syncobj *
vk_sync_as_drm_syncobj(struct vk_sync *sync)
{
   if (!vk_sync_type_is_drm_syncobj(sync->type))
      return NULL;                      /* <-- for a timeline sync */
   return container_of(sync, struct vk_drm_syncobj, base);
}
```

`vk_sync_type_is_drm_syncobj()` compares `type->finish` against the **binary** `vk_drm_syncobj_finish`.
The queue's persistent syncobj is created from `ws->timeline_syncobj_type.sync`, whose finish differs,
so the accessor returns NULL and dereferencing it faults at `.syncobj` (offset 16, i.e. just past the
16-byte `vk_sync base`). **The refusal is by design, not a bug in Mesa**: the timeline type is a
wrapper - `struct vk_sync_timeline_type { struct vk_sync_type sync; const struct vk_sync_type
*point_sync_type; }` - and `vk_sync_timeline_get_type()` wraps the point type, so a timeline sync
does not expose a bare syncobj handle through this accessor. Panfrost, the in-tree timeline user,
passes a raw handle and `timeline_value` and never goes through it.

## Consequence for the plan

The winsys must get the underlying syncobj handle **and** the point some other way, or the queue
must not use `vk_sync_timeline` at all. Options, cheapest first:

1. **Check what handle the timeline object actually owns.** `vk_sync_timeline` allocates per-point
   `vk_sync_timeline_point` objects with `point_sync_type`; the base syncobj the kernel needs is
   likely reachable from `vk_sync_timeline_state` or the first point. If so, the winsys needs a
   small accessor rather than the binary one - but this depends on the timeline internals being
   stable, so read them first.
2. **Skip `vk_sync_timeline` and manage the timeline syncobj directly in the queue**: create one
   `ws->syncobj_type` syncobj per job type and pass
   `DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ` with the incremented point. A DRM timeline
   syncobj is a timeline even though its Mesa sync type is the binary one, so
   `vk_sync_as_drm_syncobj()` keeps working and the existing winsys helper is enough. **This is the
   lazier option** - it reuses the type the driver already handles and needs no timeline internals.

Option 2 looks correct and much smaller: the kernel decides timeline vs binary from the
`HANDLE_TYPE_*` flag, not from Mesa's sync type. Worth trying first.

## Fixed along the way (committed, real bugs)

`f660241` - a signal struct whose `.sync` is NULL now means "no signal" in all three submit paths.
The step-1 API change made these parameters structs, and the queue passes
`&(const struct vk_sync_signal){ .sync = frag_signal_sync }` unconditionally, so with no fragment
work the pointer was non-NULL while the sync inside was NULL - a latent segfault in
`pvr_drm_winsys_render_submit` that this round's instrumentation exposed.

## Round tally for this thread

* `53ccc4b` winsys value plumbing (behaviour-preserving) - landed
* `de755dc` persistent syncobjs, unused (behaviour-preserving) - landed
* `e9d1b2a` winsys timeline support (behaviour-preserving) - landed
* `f660241` NULL-signal guard - landed, a real latent crash fix
* GEOM migration - attempted twice, reverted twice, both times for a concrete diagnosed reason

---

# Blocker 2 SOLVED, blocker 3 found: timeline waits deadlock

## The handle problem has a clean answer

`vk_sync_as_drm_syncobj()` refuses timeline syncs because `vk_sync_type_is_drm_syncobj()` compares
`type->finish` against the binary finish. But Mesa's shared DRM syncobj type advertises the feature
conditionally:

```c
/* vk_drm_syncobj_get_type_from_provider() */
if (sync->timeline_wait) {
   type.get_value = vk_drm_syncobj_get_value;
   type.features |= VK_SYNC_FEATURE_TIMELINE;
}
```

So a sync created from the **plain `syncobj_type`** with `VK_SYNC_IS_TIMELINE` is a legitimate
timeline sync (`vk_sync_init`'s assert passes), its object is a `struct vk_drm_syncobj`, and
`vk_sync_as_drm_syncobj()` works. **The `timeline_syncobj_type` wrapper is the wrong type to use
here** - it is a different type whose finish differs, which is what made the accessor return NULL.

One-line change, no point lifetime management:

```c
vk_sync_create(&device->vk, &device->pdevice->ws->syncobj_type, VK_SYNC_IS_TIMELINE, 0UL, &sync);
```

Also checked and rejected: `vk_sync_timeline` pools its per-point objects via `state->free_points`,
so it would work in principle, but the winsys would have to manage point refcounts
(`vk_sync_timeline_get_point` / `alloc_point` / `point_install`) and that API is only used by the
CPU-wait path today. The syncobj-type route is strictly lazier.

## Blocker 3: the migration hangs

With that in place the GEOM migration no longer crashes - **it deadlocks**. `glmark2 --validate` hung
(had to be killed; the GPU itself stayed healthy - `bda` still passed afterwards), and `mrt` /
`varyings` hung too.

First cause identified and fixed: **the queue's first job of each type waits on timeline point 0**,
and the syncobj is not created signaled, so that wait never completes. Skipping a timeline wait at
point 0 (`pvr_drm_sync_wait_is_noop()`) removed that deadlock.

**It still hung after that**, so there is a second cause not yet isolated. Candidates, in the order
worth testing:

1. **Value bookkeeping off by one.** `pvr_queue_advance_job_sync()` increments then the submit uses
   `job_value - 1` as the wait and `job_value` as the signal, i.e. job N waits point N-1 and signals
   point N. That looks right, but it has not been verified against a trace - instrument the actual
   `(wait_value, signal_value)` pairs per submission and check they chain.
2. **Timeline points and the PR job.** The render submit signals the geometry sync and then waits on
   an internal `geom_to_pr_syncobj`; if the timeline signal and the wait for the same point land in
   the same submission the ordering assumption may not hold.
3. **`DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ` on a syncobj the kernel does not consider a
   timeline.** The UAPI flag exists, but whether the pvr kernel driver accepts a point on a syncobj
   that was created without timeline intent needs checking in the kernel source.

**Reverted, tree green** at `f660241` (validate 27/0, all probes pass).

## Honest status of the sync-churn target after four rounds

Landed and independently useful: `53ccc4b` (value plumbing), `de755dc` (persistent syncobjs),
`e9d1b2a` (winsys timeline support), `f660241` (NULL-signal crash fix).

Not landed: the actual churn removal, because the timeline path deadlocks. The next step is **not**
more queue editing - it is to instrument the submitted `(wait_value, signal_value)` pairs and get a
trace of the chain, and to check the kernel's handling of timeline points on pvr syncobjs. Until
those are measured, further migration attempts are guesses.

**Cost of this thread so far: 4 rounds, 4 landed commits, 3 diagnosed blockers, 0 performance
change.** Worth stating plainly - the target's ~10 ms/frame remains unclaimed.
