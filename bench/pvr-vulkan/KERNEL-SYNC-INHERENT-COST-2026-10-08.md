# The kernel-side cost is inherent to a handle-based sync interface — and honest status

## What the kernel runs per sync op

`pvr_sync_signal_array_add()`:

```c
sig_sync = kzalloc(sizeof(*sig_sync), GFP_KERNEL);           /* allocation                */
if (point > 0)
    sig_sync->chain = dma_fence_chain_alloc();               /* + allocation              */
sig_sync->syncobj = drm_syncobj_find(file, handle);          /* handle lookup (locked)    */
if (!drm_syncobj_find_fence(file, handle, point, 0, &cur_fence))
    sig_sync->fence = cur_fence;                             /* + second lookup + resolve */
xa_alloc(array, &id, sig_sync, xa_limit_32b, GFP_KERNEL);     /* + xarray allocation       */
```

**Per sync op: two handle lookups, up to three allocations, an xarray insert.** Multiplied across 20–46 jobs
per frame, that is where the measured **~21 ms of kernel CPU per frame** goes — and it is **inherent to a
handle-based synchronisation interface**, not an implementation slip that can be tuned away.

**This is the strongest confirmation yet that target (3) is the fix and target (1) is not:** the cost is
proportional to the number of *handle-mediated operations*, so it can only be removed by not mediating
through handles — which is what the vendor's `pvr_srv_sync_type` does and what `drm/imagination` lacks.

---

# Honest status of the objective

**The objective is not met.** What the session produced:

1. **A real, committed fix** — PCO's unroll threshold (`c2bde57`): **1.44–1.87×** on loop-bound shaders,
   **1.27×** on a shader-heavy client, **no change on the default suite**, correctness fully green.
2. **The dominant bottleneck identified and quantified** — the per-job kernel interface: **21–30 ms** of
   Xwayland kernel CPU per frame, **20–46 jobs per frame** on the slow scenes, **~17 ms/frame recoverable**.
3. **The Mesa-side fixes disproved on evidence, not assumption** — every pooling/recycling variant of
   target (1) is unsound because **the userspace handle is not the only reference to a sync object**
   (`pvr_sync.c:82`), and the failure mode is a **GPU hang**.
4. **The remaining fix scoped to a single, verified, in-scope change** — a `drm/imagination` UAPI facility
   for batched job ordering / a driver-native sync type. The module **builds on this host**; the UAPI gap
   is **confirmed** (`enum drm_pvr_job_type` has four types, no chaining/null type).
5. **Seven measurement rules** earned by catching my own errors, and **eleven withdrawn claims** on the
   record.

## What remains

**A kernel UAPI change**, sized at roughly:

- a new job-chaining or driver-sync facility in `pvr_drm.h` + `pvr_job.c` / `pvr_sync.c`;
- the Mesa side switched over to it;
- a module reload with **weston and Xwayland down** (guard respected) to test.

**It should be started with the context budget to finish and verify it** — a half-applied kernel change to a
GPU driver is a worse outcome than a documented, verified, working baseline.

## Final state

| | |
|---|---|
| Mesa | clean, `c2bde57`, **37 commits ahead of `main`**, never pushed |
| Bench | clean, **200 commits ahead**, never pushed |
| Driver | `powervr` (open), one GPU module loaded |
| Compositor | weston + Xwayland up, kwin absent |
| Correctness | **27 glmark2 scenes + bda/vk13/pctest/vk16/vkrender all PASS** |
| Guard | `gpu-fw-guard` **active** |
