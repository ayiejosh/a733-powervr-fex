# The present gap is Mesa's WSI explicit-sync wait - the vendor has none

Ran the whole stack on the vendor side (weston with the vendor ICD, client with the vendor ICD) and
instrumented the same traces used on the open side:

```
VENDOR 640x480: FPS: 1021  FrameTime: 0.980 ms
[acq] / [rel]: NO OUTPUT AT ALL
```

**The vendor client performs no explicit-sync release or acquire waits at all.** Those traces are Mesa
WSI code (`wsi_common_drm.c`) and never fire, because with the vendor Vulkan the WSI in use is the
vendor's, not Mesa's.

## The comparison

| | open (Mesa WSI) | vendor (vendor WSI) |
|---|---|---|
| frame time at 640x480 windowed | 22 ms (45 FPS) | **0.980 ms (1021 FPS)** |
| explicit-sync release wait | **12.5 ms** | **none** |
| acquire wait | 9.2-9.9 ms | none |

**So the present gap is not a slow compositor and not a slow GPU - Mesa's WSI waits on an explicit-sync
release every frame (12.5 ms) and the vendor's WSI does not.**

## What it closes and opens

* **Closes "weston composites slowly."** Weston is the same binary in both runs; if its composite were
  the cost the vendor run would pay it too. It does not - 0.980 ms total.
* **Closes "the GPU is slow at compositing"** for the same reason.
* **Opens the real question, in Mesa's scope:** why does the explicit-sync release take 12.5 ms, and
  can the wait be shortened or avoided? The release exists so the client does not reuse a buffer the
  compositor still holds; the vendor achieves the same correctness without a per-frame wait.
  Candidates: waiting on a signal that arrives later than necessary, buffer count, or the sync type
  used for the release - the same DRM-syncobj architecture as target (3).
* `ZINK_EXTRA_IMAGES` was measured earlier with no effect, so adding buffers alone does not help.

## Where this leaves the objective

**The largest single term (present, 22.6x) is now localised to a specific Mesa WSI wait** - better than
"the compositor is slow". It is also where the objective's original "787 vs 31" lives, and it is in
Mesa's scope, unlike the per-pass syncobj cost which needs a kernel UAPI change.
