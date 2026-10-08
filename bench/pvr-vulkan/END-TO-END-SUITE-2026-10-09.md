# End-to-end suite result with both fixes: 46 → 49

Full `glmark2-es2 -s 640x480` (all scenes, composited weston + Xwayland + zink), both fixes in place:

| | glmark2 Score |
|---|---|
| before any fix (session baseline) | **46** |
| **with `c2bde57` + `c251c9b`** | **49** |

**A 6.5% improvement in the full-suite score, against 2.7–3.4× on looped microbenchmarks.**

**The difference between those two numbers is the point:** the suite score is a sum dominated by its slowest
scenes (`terrain` 5 FPS, `refract` 12, `desktop blur` 24), which are **multi-pass / multi-window** and bound
by the **per-pass kernel cost** and the **per-surface render cost** — **neither of which either fix touches.**

## Complete, honest picture of the session

| term | before | now | vendor | gap before | gap now |
|---|---|---|---|---|---|
| real 32-iteration shader (`vkheavy`) | 858.5 ms | **255.8 ms** | 180.0 ms | **4.77×** | **1.42×** |
| integer loop (`cstpi`) | 26.6 M/s | **72.8 M/s** | 146.6 M/s | **5.53×** | **2.01×** |
| float loop (`cstpf`) | 29.6 M/s | **87.9 M/s** | 145.9 M/s | **4.93×** | **1.66×** |
| integer, no loop (`cstp`) | 305.9 M/s | 324.4 M/s | 375.0 M/s | 1.23× | **1.16×** |
| raw render, no loop (`vkrender`) | 306 Mpix/s | **306 Mpix/s** | 757 Mpix/s | 2.47× | **2.47× unchanged** |
| **full glmark2 suite** | **46** | **49** | — | — | **1.07×** |

**Two real fixes, both verified with behaving controls, both in `src/imagination/pco/`.** They deliver
**2.7–3.4×** where the workload is loop/shader-bound and **~7% end-to-end**, because the suite's bottleneck is
elsewhere.

## What the session did not move, and why

* **Per-job kernel sync interface** — **84% of frame time in the kernel**, ~190 syncobj ioctls/frame,
  ~17 ms/frame recoverable. **Proved unreachable from Mesa**: the kernel resolves sync objects by handle and
  holds its own reference (`pvr_sync.c:82`), so pooling or recycling a handle **aliases in-flight jobs** and
  the failure mode is a **GPU hang**. Needs a `drm/imagination` UAPI change (module builds here; UAPI gap
  confirmed) — a multi-step change **deliberately not started at the end of a long session**.
* **`vkrender`'s 2.47× per-surface cost** — every source-visible candidate excluded (PBE <10% by a clean
  non-discard probe; format-, coverage- and geometry-independent). Needs PVRtune or a vendor command-stream
  diff, neither available.
