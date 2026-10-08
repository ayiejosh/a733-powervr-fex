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

---

# Measured: the kernel is fine; the timeline SIGNAL itself fails the submit

Instead of more queue editing, the submitted ops were instrumented and the kernel source read.

## The kernel fully supports timeline syncobjs

`/home/radxa/kernel-src/powervr/pvr_sync.c`:

```c
pvr_check_sync_op():
   if (sync_op->flags & ~DRM_PVR_SYNC_OP_FLAGS_MASK) return -EINVAL;
   handle_type = flags & HANDLE_TYPE_MASK;
   if (handle_type != SYNCOBJ && handle_type != TIMELINE_SYNCOBJ) return -EINVAL;
   if (handle_type == SYNCOBJ && value != 0) return -EINVAL;      /* binary must be 0 */
   return 0;
```

and it allocates `dma_fence_chain` for points (`pvr_sync_signal_array_add`, `if (point > 0)
sig_sync->chain = dma_fence_chain_alloc()`). A wait first searches the same submission's signal
array, then falls back to `drm_syncobj_find_fence(handle, point, ...)`. **So the flag, the point and
the chaining are all implemented; my usage passes the flag mask (`0x80000001` within `0x8000000f`).**

## The failure, measured

With the GEOM migration plus op tracing (`SYNC_TRACE=1`) and a 15 s timeout:

```
[q] advance type=0 -> value=1 sync=0xaaab116175e0
[op] handle=2 flags=80000000 value=1 timeline=1     <- geom SIGNAL, timeline point 1
[op] handle=7 flags=80000000 value=0 timeline=0     <- frag SIGNAL, binary
(exactly 2 ops in total; no further submission)
```

and the client reports:

```
vkCreateGraphicsPipelines (8 colour attachments) -> 0
FAIL submit -> -4                                    <- VK_ERROR_DEVICE_LOST
```

**So this was never a silent hang: the very first submission fails with `VK_ERROR_DEVICE_LOST`, and
the client then blocks forever because it waits on a fence from a submit that never succeeded.** The
GPU itself is healthy throughout (`bda` passes immediately afterwards, no fault in `dmesg`).

The failing submission contains **only signals** - no wait at all - so the deadlock is not the
value chain and not point 0. It is the timeline signal on the first submit.

## Where to look next (narrowed)

The signal path in the kernel is `pvr_sync_signal_array_collect_ops()` ->
`pvr_sync_signal_array_get()` -> `pvr_sync_signal_array_add()`, which fails with -EINVAL only at
`drm_syncobj_find(file, handle)` and -ENOMEM at `dma_fence_chain_alloc()`. Both are worth
distinguishing, and the next measurement is the **errno**, which `pvr_ioctlf` formats into its
`vk_errorf` message - that message did not appear, which means the -4 is being produced above the
ioctl, so the next step is to find which layer maps it and print the ioctl errno directly.

## Tree state

Reverted and green at `f660241`: build clean, bda/vk13/pctest/vk16/mrt PASS, `glmark2 --validate`
27/0.

## Cost, stated plainly (5 rounds on this thread)

| round | outcome |
|---|---|
| 57 | `53ccc4b` value plumbing landed |
| 58 | aborted, recorded plan |
| 59 | `de755dc` persistent syncobjs landed |
| 60 | `e9d1b2a` winsys timeline support + `f660241` real crash fix landed |
| 61 | handle problem solved (`syncobj_type` + `VK_SYNC_IS_TIMELINE`), deadlock found |
| 62-63 | kernel read (supports timelines), failure measured as DEVICE_LOST on the signal |

**0 performance change so far.** The churn is still there. The honest read: this target is a real
driver work item, not a tuning knob, and it needs the ioctl errno before the next edit.

---

# ROOT CAUSE FOUND: the sync must be a vk_sync_timeline, not a flagged DRM syncobj

Traced `FAIL submit -> -4` to its source. It is **not** the submit ioctl:

1. `mrt.c:143` is `CK(vkQueueSubmit(...))`, so `-4` is what `vkQueueSubmit` returned.
2. `vk_queue.c:1288` - `vkQueueSubmit` returns `VK_ERROR_DEVICE_LOST` immediately when
   `vk_device_is_lost(device)` is already true.
3. So the device was lost *earlier*, and `vk_device.c:417` reports the recorded message.

The only sites that mark the device lost on this path are `vk_queue.c:600` and `:635`:

```c
result = vk_sync_wait_unwrap(queue->base.device, &submit->waits[i], &wait_point);
if (unlikely(result != VK_SUCCESS))
   result = vk_queue_set_lost(queue, "Failed to unwrap sync wait");
...
result = vk_sync_signal_unwrap(queue->base.device, &submit->signals[i], &signal_point);
if (unlikely(result != VK_SUCCESS))
   result = vk_queue_set_lost(queue, "Failed to unwrap sync signal");
```

and `vk_sync_signal_unwrap` / `vk_sync_wait_unwrap` only understand Mesa's **two wrapper types**:

```c
struct vk_sync_timeline *timeline = vk_sync_as_timeline(signal->sync);
if (timeline) { alloc_point(...); signal->sync = &(*point_out)->sync; signal->signal_value = 0; }

struct vk_sync_binary *binary = vk_sync_as_binary(signal->sync);
if (binary) { signal->sync = &binary->timeline; signal->signal_value = ++binary->next_point; }
```

## What this means

`vk_sync_as_timeline()` matches `type->init == vk_sync_timeline_init`; `vk_sync_as_binary()`
matches `type->init == vk_sync_binary_init`. A sync created from the plain DRM `syncobj_type` with
`VK_SYNC_IS_TIMELINE` set is **neither**, so it falls through both branches. **That is why the
previous round's "handle problem solved" conclusion was wrong**: the flag on a raw DRM syncobj
satisfies the kernel and `vk_sync_as_drm_syncobj()`, but violates the common layer's contract, and
the common layer is what the driver's `driver_submit` runs underneath.

So the earlier two constraints really do conflict, and the resolution is the third option:

**The queue must use a real `vk_sync_timeline`, and the winsys must resolve the point to a syncobj
handle** - which is exactly what the point API is for:

```c
struct vk_sync_timeline_point *point;
vk_sync_timeline_get_point(device, vk_sync_as_timeline(sync), value, &point);
/* point->sync is a vk_sync of point_sync_type, i.e. a struct vk_drm_syncobj */
handle = vk_sync_as_drm_syncobj(&point->sync)->syncobj;
```

for a wait, and `vk_sync_timeline_alloc_point()` (+ `point_unref` after submission) for a signal.
The common layer already extracts these points into `submit->_wait_points[]` /
`submit->_signal_points[]`, so the intended shape is that the driver consumes those rather than
resolving the timeline itself - worth checking how `pvr_driver_queue_submit` could use them.

## Also learned

`vk_sync_binary` gives **free** timeline emulation to any sync type built on it: unwrapping rewrites
a binary sync into `binary->timeline` at an auto-incremented point. The DRM syncobj type is not a
`vk_sync_binary` type, so pvr does not get this. Whether the DRM syncobj type *could* be layered on
`vk_sync_binary` is a separate, possibly much lazier question than rewriting the queue - it would
give timeline behaviour without the queue managing values at all. **Check that first next round.**

## Tree

Reverted and green at `f660241` (validate 27/0, all probes pass). No uncommitted changes.

---

# The design that works - and why I am stopping before implementing it

## The solution, finally, is smaller than everything I tried

The winsys never needs to know about timelines at all.

`vk_sync_timeline` allocates **one plain binary syncobj per point**, and pools them via
`state->free_points`, reusing a point instead of creating a syncobj when one is free
(`vk_sync_timeline_alloc_point_locked`: `if (list_is_empty(&state->free_points)) { vk_zalloc...
vk_sync_init(point_sync_type) } else { reuse, maybe vk_sync_reset }`). So the timeline machinery's
whole job is to **hand out a binary syncobj per point and reuse them**.

So the queue should:

1. create one `vk_sync_timeline` per job type from `ws->timeline_syncobj_type` (already registered),
2. per job, resolve its point - `vk_sync_timeline_get_point()` for a wait,
   `vk_sync_timeline_alloc_point()` for a signal - and pass **`&point->sync`**, an ordinary binary
   DRM syncobj, to the winsys,
3. unref the point once the ioctl has returned.

The kernel takes its own reference during submit (`pvr_sync_signal_array_update_fences`:
`sig_sync->fence = dma_fence_get(done_fence)`; the wait side likewise `dma_fence_get`), so step 3 is
safe and needs no lifetime tracking across submissions.

**This needs no winsys change, no timeline flag, no point lifetime protocol, and no kernel work.** It
is strictly smaller than every approach in this document - including the one that got as far as
`DEVICE_LOST`, whose whole problem was that I was trying to teach the winsys and the common layer
about timelines when the queue could simply have resolved them away first.

## Why I am not implementing it now

**The measured payoff does not justify the remaining risk, and I would rather say so than keep
spending rounds.** Target (1) is ~10 ms/frame of kernel time in Xwayland. The gap to close is ~32x.
Even a perfect implementation of this removes roughly 83 of ~190 syncobj ioctls per frame and does
not close the gap - and the restructure touches 26 create/destroy sites in `pvr_arch_queue.c`, which
I have now attempted and reverted four times, each time exposing another layer (winsys values, winsys
timelines, the common layer's unwrap contract, and finally the kernel's signal path).

That is a poor expected value, and four reverts is evidence about the approach, not about effort.

## What the rounds actually established, so the next attempt starts from here

| finding | status |
|---|---|
| raw DRM syncobj + `VK_SYNC_IS_TIMELINE` is invalid (common layer unwrap contract) | proven, `b8248b5` |
| `vk_sync_as_drm_syncobj()` refuses timeline syncs by design | proven |
| the kernel fully supports timeline syncobjs and fence chains | proven, read from source |
| the kernel takes its own fence reference at submit | proven, enables unref-after-ioctl |
| `vk_sync_timeline` pools binary syncobjs per point | proven, read from source |
| **the queue should resolve points itself and keep the winsys binary** | **the design, not implemented** |
| landed and independent | `53ccc4b`, `de755dc`, `e9d1b2a`, `f660241` |

## Where the performance actually is

Restating the measured ground truth so it is not lost: the vendor reaches ~787 FPS where the open
stack gets ~31 **through the same weston + Xwayland + client + zink**, raw render is only 2.5-4x
down, and **the KMS path matches the vendor** (pvranimate 396 vs ~380 Mpix/s). The windowed gap is
therefore the area-dependent copy in the Xwayland path, not the driver's job submission - a zero-copy
flip would fix it and is area-independent. **Target (1) is a small slice of a large problem, and the
large problem is architectural.**

Target (2) is measured closed (no effect at 800x600 or 1280x720; the earlier +19% was noise).

---

# IMPLEMENTED: the queue resolves its own points - got further than ever, then DEVICE_LOST

I implemented the design from the previous section: **the queue resolves timeline points itself and
hands the winsys ordinary binary syncobjs**, so no winsys change is needed at all.

## What was done (105 lines, one file: pvr_arch_queue.c)

* `job_sync[]` created from `timeline_syncobj_type.sync` - a **real** `vk_sync_timeline`.
* `pvr_queue_alloc_point()`: `queue->job_value[type]++` then
  `vk_sync_timeline_alloc_point(..., &point)`; returns `&point->sync` as the signal.
* `pvr_queue_wait_sync()`: `vk_sync_timeline_get_point(..., job_value[type] - 1, &point)`; returns
  `&point->sync`, or NULL when there is nothing to wait for.
* After a successful submit: `vk_sync_timeline_point_install()` for the signal point (consuming the
  reference), `vk_sync_timeline_point_unref()` for the wait point - exactly the pattern
  `vk_queue.c` uses for application signals, and safe because the kernel takes its own fence
  reference during the ioctl.
* The winsys is **untouched** and keeps seeing binary DRM syncobjs.

## Five real bugs found and fixed on the way

Each was a concrete, diagnosed defect rather than a guess:

1. **`point_install` with a NULL point** - my create-site replacement silently missed (it targeted a
   helper name that no longer existed after an earlier revert), so `geom_point` stayed NULL. Caught
   by instrumenting the value before the call: `install geom_point=(nil) value=0`.
2. **`pvr_update_job_syncs()` frees the persistent sync** - `last_job_signal_sync[GEOM]` now aliases
   `job_sync[GEOM]`, so destroying it freed the timeline. Fixed with a per-type exemption.
3. **`pvr_clear_last_submits_syncs()` double-free** - it destroys both alias arrays for *all* types.
   Second exemption.
4. **`err_destroy_geom_sync` frees an interior pointer** - `geom_signal_sync` is now
   `&point->sync`, a pointer *into* the timeline's point object, so `vk_sync_destroy()` called
   `vk_free()` on an interior pointer: `free(): invalid pointer`. Fixed by unref-ing the point.
5. **`pvr_queue_finish()` double-free** - it destroys `job_sync[i]` *and* both alias arrays.
   Third exemption.

After those, **the crash was gone and all 13 probes passed** - `bda`, `vk13`, `pctest`, `vk16`,
`wgsize`, `samplers`, `storageimages`, `mrt`, `varyings`, `inatt`, `ubos`, `stgbuf`, `vattrib`.

## The remaining failure

`glmark2-es2 --validate` then reported **0 success / 27 failure**, and the client log showed:

```
MESA: error: ZINK: vkQueueSubmit failed (VK_ERROR_DEVICE_LOST)
```

So the device is being marked lost again, on a different path than before. The prime suspect is the
**timeline mode**: `vk_sync_signal_unwrap()`/`vk_sync_wait_unwrap()` assert
`device->timeline_mode == VK_DEVICE_TIMELINE_MODE_EMULATED`, and registering a timeline sync type
changes what `vk_device_init` chooses. Worth reading `vk_device_init`'s timeline-mode selection
before editing anything else - the probes exercising compute/render pass, but the zink path goes
through application semaphores and the unwrap machinery, which is exactly what differs.

**Reverted; tree green at `f660241`** (validate 27/0, all probes pass).

## Where this leaves target (1)

Genuinely closer than the four previous attempts, which all died in the winsys or the common layer.
This attempt died in the queue's own teardown paths, and every one of those was mechanical and is
now fixed. The next session should start from the timeline-mode question above with the five fixes
in hand, not from scratch.

---

# The migration WORKS - and has no measurable effect. Plus two instrument fixes.

## It works

With the sixth defect fixed, the queue-side timeline migration is **correct**:

```
glmark2-es2 --validate:  success=27  failure=0
bda, vk13, pctest, vk16, wgsize, samplers, storageimages, mrt, varyings, inatt, ubos, stgbuf,
vattrib: all PASS
```

The winsys was never touched. Six real defects had to be fixed to get here, each found by
backtrace or instrumentation, not by reasoning:

| # | defect | how it surfaced |
|---|---|---|
| 1 | `point_install` called with a NULL point (a replacement silently missed) | instrumented the value: `install geom_point=(nil) value=0` |
| 2 | `pvr_update_job_syncs` freed the persistent sync the alias arrays point at | `free(): invalid pointer` |
| 3 | `pvr_clear_last_submits_syncs` double-freed the same | `free(): invalid pointer` |
| 4 | `err_destroy_geom_sync` called `vk_sync_destroy` on `&point->sync`, an interior pointer | `free(): invalid pointer` + backtrace |
| 5 | `pvr_queue_finish` double-freed the aliases | `free(): invalid pointer` |
| 6 | `pvr_process_event_cmd_barrier` destroyed `next_job_wait_sync[stage]`, also an interior pointer | backtrace: `pvr_process_event_cmd_barrier.isra` |

## It does not move the frame rate

Interleaved A/B, 4 pairs, same background load, 800x600:

```
pair 1: baseline 40   migration 43
pair 2: baseline 49   migration 46
pair 3: baseline 40   migration 30     <- background-load outlier
pair 4: baseline 45   migration 45
median: baseline ~42.5   migration ~44.5
```

**Indistinguishable.** Removing the per-job syncobj create/destroy for the geometry path does not
measurably change windowed FPS, which is consistent with the ground truth that the windowed gap is
the area-dependent Xwayland copy, not job submission.

Only 1 of 5 job types is migrated, so at most a fifth of the churn is gone even in principle.

**Not shipped.** It is correct but adds 105 lines plus six per-type exemption sites in exchange for
no measured gain - maintenance risk with no demonstrated benefit. Reverted; tree green at `f660241`.

## Instrument fix 1: single-sample FPS numbers on this board are worthless

This round I briefly concluded the migration was **3x slower** (15 FPS vs 39). That was wrong.
`FEXInterpreter` was sitting at **100% CPU** and `syncthing` at 32%, and a background `ninja` build
was also running. Once measured **interleaved** (baseline, migration, baseline, migration, ...) so
both configurations see the same load, the numbers were identical.

**Rule: never compare two configurations by measuring one after the other on this board.** Background
load from `FEXInterpreter`, `syncthing`, and stray builds moves the number by more than any change
being tested. Interleave, and check `ps -eo pcpu --sort=-pcpu` before believing a delta.

## Instrument fix 2: the release build has NDEBUG, so no assert() is live

```
buildtype = release, b_ndebug = if-release     ->  NDEBUG defined
```

Recorded in the recovery note in full. Consequences here: `get_timeline_mode()`'s
`assert(timeline_type == NULL)` cannot fire even though the pvr DRM winsys registers **two**
TIMELINE-advertising types (`syncobj_type`, which sets the feature whenever the provider has
`timeline_wait`, and the wrapper); the mode comes out EMULATED by iteration order rather than by
rule. Two instruments to use instead of reading source:

* **`MESA_VK_ABORT_ON_DEVICE_LOSS=true`** makes `_vk_queue_set_lost()` print the recorded lost-device
  message and abort - it names the failing call directly.
* **an assert-enabled build** (`meson setup build-assert -Db_ndebug=false -Dbuildtype=debugoptimized`)
  makes the violated invariant name itself.

## Status of target (1)

Implemented and proven correct; measured to have no effect on the metric that matters. The remaining
lever is the area-dependent Xwayland copy path, as recorded before.
