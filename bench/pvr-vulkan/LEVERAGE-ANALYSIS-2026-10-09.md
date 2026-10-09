# LEVERAGE ANALYSIS — the render is 1.5% of the real workload

## Method

Harness on the vendor across sizes (fresh); open figures from earlier this session, same probe. Then the
recoverable cost **per term for the real workload** (640×480 through zink, composited) — the objective's
actual case, not the synthetic 2048 probe.

## Vendor cost curve (`vkrender`, full draws)

| size | ms/frame | Mpix/s |
|---|---|---|
| 256 | 0.612 | 107.1 |
| 512 | 0.836 | 313.6 |
| 1024 | 1.776 | 590.5 |
| 2048 | 5.661 | 740.9 |
| 4096 | 23.678 | 708.6 |

**Open vs vendor:** 512 → 1.848 vs 0.836 (**2.21×**); 2048 → 13.706 vs 5.661 (**2.42×**).

**The render ratio is flat across sizes** — there is **no small-surface edge to exploit**; the deficit is
uniform.

## Where the recoverable milliseconds actually are (real client, 640×480)

| term | open | vendor | excess | **share** |
|---|---|---|---|---|
| client render | 0.9 ms | 0.4 ms | **0.5 ms** | **1.5%** |
| present (release wait) | 12.5 ms | ~0 | **12.5 ms** | **38.5%** |
| kernel / sync | ~20.0 ms | ~0.5 ms | **~19.5 ms** | **60.0%** |

**98.5% of the recoverable cost is NOT the render.** The four PCO fixes address the 1.5% — **correctly, and
they are real, but they are not where the leverage is.**

## The two levers, and what each needs

**1. Kernel / sync — 60%.** ~190 syncobj ioctls/frame, **84% of frame time in the kernel**. **Proved
unreachable from Mesa**: the kernel resolves sync objects by handle and holds its own reference
(`pvr_sync.c:82`), so pooling aliases in-flight jobs and the failure mode is a **GPU hang**. **Needs a
`drm/imagination` UAPI change** — the module builds here, the gap is confirmed, and the migration plan exists
with one aborted attempt and a corrected ordering (the event/barrier paths own the same array slots, so it
cannot be converted piecewise).

**2. Present — 38.5%.** The release wait is **the render deficit applied to weston's compositor**: the wait
scales with the surface (12.5 ms at 640×480 → 66 ms at 1080p), and weston composites the **4K output in
~2 passes** at the open driver's per-surface rate (**57.5 ms predicted vs 66 measured**). **This lever is the
same per-surface deficit, viewed through the compositor.**

## The focused conclusion

**There are exactly two levers, and neither is the shader:**

- **per-surface render deficit (2.17×)** — reaches the client twice: directly (small) and through the
  compositor (large). **Not reachable from source**: every driver-visible config read is correct or maximal,
  and the firmware is a different image that is not interchangeable.
- **kernel/sync path (60%)** — needs a UAPI change; all Mesa-side variants are proven unsound.

**The shader work was worth doing and is done. It is not the leverage.** Further effort goes to the
kernel/sync path or stops — not to more codegen.
