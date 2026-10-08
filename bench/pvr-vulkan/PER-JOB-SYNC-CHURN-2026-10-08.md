# The per-job sync churn, precisely located — and why I did not ship a pool this round

## The code path

`pvr_arch_queue.c` creates a fresh **DRM syncobj** for every job's signal (**10 `vk_sync_create` sites**):

```c
vk_sync_create(&device->vk, &device->pdevice->ws->syncobj_type, 0U, 0UL, &geom_signal_sync);
vk_sync_create(&device->vk, &device->pdevice->ws->syncobj_type, 0U, 0UL, &frag_signal_sync);
```

and `pvr_update_job_syncs()` **destroys** the previous ones (**19 `vk_sync_destroy` sites**):

```c
if (queue->next_job_wait_sync[type])   { vk_sync_destroy(...); ... = NULL; }
if (queue->last_job_signal_sync[type]) { vk_sync_destroy(...); }
queue->last_job_signal_sync[type] = new_signal_sync;
```

**Each job costs up to 2 syncobj creates and 2 destroys** — 4 ioctls per job, plus kernel object
allocation/teardown. **A 28-job frame (desktop blur) therefore issues on the order of 100 syncobj ioctls per
frame** — where the objective's "~250 DRM ioctls per client frame" comes from.

## A pool is implementable — the type supports it

`vk_drm_syncobj.c` exposes `.features = ... | VK_SYNC_FEATURE_CPU_RESET` and `.reset =
vk_drm_syncobj_reset`, so a pooled syncobj **can be reset and reused** without a new object.

## Why I did not ship it this round

The lifetime of those two slots is managed across **~8 separate sites** (lines 234–240, 264–275, 578–581,
750–753, 959–966, plus uses at 335/363/434/464/500/561/717/932/994), and the slots are consumed by
*subsequent* submits. **Returning a sync to the free list one submit too early silently breaks
synchronisation, and the failure mode is a GPU hang, not a wrong pixel** — exactly the risk class the
objective's safety note flags.

**A pool is the right fix and it is bounded, but it needs the lifetime worked out site by site and verified
under the full correctness suite before it goes in.** Shipping it blind would trade a measured 22–65%
opportunity for a hang risk — a bad trade.

## Also: a claim of mine that needs revisiting

The earlier conclusion that **"the vendor does no explicit-sync waits"** rests on `ACQ_TRACE` and
`WSIREL_TRACE` — **Mesa environment variables**. The vendor client runs `zink` on `libVK_IMG`, which does
**not** honour Mesa's env vars, **so those traces being silent on the vendor proves nothing about the
vendor's waits.** That conclusion is **withdrawn**; the vendor's synchronisation behaviour is currently
unmeasured, and the present-gap comparison must not lean on it.

## Next

Implement the sync pool with the lifetime verified site by site, then measure `desktop blur` and `terrain`
before/after. The payoff ceiling is the per-job sync cost on 20–46-job frames.
