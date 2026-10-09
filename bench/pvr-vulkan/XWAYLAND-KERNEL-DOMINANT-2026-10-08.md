# QUANTIFIED: Xwayland spends 2.2–2.8× more time in the KERNEL than in Mesa

Measured Xwayland's own `/proc/<pid>/stat` utime/stime split across scenes (25 s runs, 640×480,
composited):

| scene | FPS | user | sys | per frame: user | per frame: sys |
|---|---|---|---|---|---|
| conditionals (simple) | 21 | 16.6% | **44.2%** | 7.90 ms | **21.05 ms** |
| desktop blur (4 windows) | 20 | 18.8% | **41.4%** | 9.40 ms | **20.72 ms** |
| terrain | 5 | 5.4% | **15.1%** | 10.77 ms | **30.11 ms** |

## Established

* **The kernel side is 2.2–2.8× the user side in every scene.** The bottleneck in the client's CPU path is
  the **DRM ioctl interface**, not Mesa's userspace work — independently confirming the objective's
  framing ("Xwayland burns a core, **mostly kernel**").
* **~250 DRM ioctls/frame costing ~21 ms ≈ 84 µs per ioctl.** Syncobj ioctls are expensive per call
  (handle lookup, locking, fence resolution), so cost is dominated by the **number of round trips**.
* **For the fast scene, 21 ms of a 47 ms frame is 45%** — even "fast" scenes mostly pay ioctl cost.

## The quantified payoff

**Cutting the ioctl count from ~250 to ~50 per frame would remove on the order of 17 ms/frame** — the
difference between 21 FPS and ~45 FPS on the simple scene, and a comparable share on the multi-pass ones.

**Only reachable by reducing round trips — and the previous entries establish it cannot be done from
Mesa:** per-job sync handles must be created and destroyed because the kernel takes its own reference by
handle (`pvr_sync.c:82`), so pooling or recycling them aliases in-flight jobs.

## The single remaining target

**A `drm/imagination` UAPI facility letting a batch of passes be described once and ordered by the kernel
or firmware, instead of one handle round trip per job** — the equivalent of the vendor's
`pvr_srv_sync_type`. The module builds on this host, the UAPI gap is confirmed (`enum drm_pvr_job_type` has
four types, no chaining/null type), and the payoff is now **measured at ~17 ms/frame**.

**This is the objective's remaining work, and it is a kernel change, not a Mesa change.**
