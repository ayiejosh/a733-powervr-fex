# Target (1) "pool them" is UNSOUND — proved from the kernel's reference handling

## The proof

The kernel resolves each job's sync handles and **takes its own references**:

```c
/* pvr_sync.c */
sig_sync->syncobj = drm_syncobj_find(file, handle);   /* line 82  — reference taken */
sig_sync->fence   = dma_fence_get(done_fence);        /* line 178 — fence reference taken */
...
drm_syncobj_put(sig_sync->syncobj);                   /* line 41  — released at teardown */
dma_fence_put(sig_sync->fence);                       /* line 43 */
```

**The kernel looks the syncobj up by handle and keeps a reference for as long as the job needs it.**

**Therefore:** if userspace keeps the handle and hands the **same object** to a later job, the kernel holds
**one sync object referenced by two jobs with different meanings** — a silent synchronisation break whose
failure mode is a **GPU hang**, not a wrong pixel.

**This is exactly why `vk_sync_destroy` is correct today**: it drops the *userspace* handle while the
kernel's reference keeps the object alive. **Pooling removes precisely that property**, and resetting a
pooled object does not fix it — the hazard is aliasing across in-flight jobs, not stale state.

## Consequences

* **Target (1)'s "pool them" is unsound.** Third unsound variant in that target, after the null-job temp
  syncobj and the reset-instead-of-destroy idea — **all three fail for the same reason: the userspace
  handle is not the only reference to the object.**
* **"Timeline-back them" was measured worse** (3404 → 3604 ioctls, waits +50%, +399 RESET).
* **Target (1) is therefore closed as not viable from userspace.** The per-job sync cost cannot be removed
  by changing how Mesa represents or recycles `vk_sync`.

## What this confirms

**Target (3) is the real fix — and the same fix as the per-pass bottleneck**: a **driver-native sync type**
(`pvr_srv_sync_type` on the vendor side) where the kernel/driver manages synchronisation internally and
**does not require a per-job handle round trip**. That is a `drm/imagination` UAPI change — in scope, and
the **only identified route** to both the 74× per-pass overhead and the ~100 syncobj ioctls/frame.
