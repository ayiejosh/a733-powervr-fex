# BREAKTHROUGH: the render deficit is a SURFACE cost, not a fill cost

`vkrender AREA=quarter` shrinks the **render area** without changing the surface, separating "cost
follows the pixels covered" from "full-surface work regardless". Same 2048x2048 target, both drivers:

| target 2048 | open | vendor |
|---|---|---|
| full draw (4.19 Mpix covered) | 14.870 ms | 5.663 ms |
| **quarter draw (1.05 Mpix covered)** | **13.899 ms** (-6.5%) | **4.187 ms** (-26%) |
| half draw | - | 4.450 ms |

Fitting `time = a + b * covered_Mpix`:

| term | open | vendor | ratio |
|---|---|---|---|
| **b: per drawn pixel** | 0.31 ms/Mpix | 0.47 ms/Mpix | **open is 1.5x FASTER** |
| **a: per surface** | **13.57 ms** | **3.69 ms** | **3.68x worse** |

## It inverts the previous model

**The entire render deficit is a full-surface cost.** The open driver's time barely moves when the
drawn area shrinks 4x (14.87 -> 13.90); the vendor's drops 26% (5.66 -> 4.19). And the open driver's
**per-drawn-pixel** rate is **better** than the vendor's.

**So the long-standing "raw render 2.5-4x down / fill-rate deficit" framing was wrong - it is not a
fill-rate problem.** Everything measured earlier as "per-Mpix" was really per-*surface*, because the
drawn area always equalled the surface. `AREA` separated them for the first time.

## Fits every earlier result

* **Bytes/pixel flat** (r8 -> rg16): a per-surface cost is format-independent.
* **Tile load/store DONT_CARE did not help**: not the load/store - other per-tile work.
* **Compute at vendor speed** (1.12x): compute has no tiles.
* **Copy at 1.40x**: touches the surface too and is close, so the expensive thing is specific to the
  *render* per-tile path, not surface traffic in general.
* **Empty pass 0.67 ms**: the per-pass kernel cost is small next to this 13.6 ms surface term.

## Where to look

**A per-tile operation in the render path running over the whole surface regardless of coverage,
costing ~3.68x the vendor's.** Candidates: the end-of-tile (EOT) program and its per-tile dispatch,
tile-buffer setup/teardown per tile, or an unconditional per-tile resolve/store. `MODE=empty` (no
draw) is cheap, so it needs *a render pass with a draw* - the EOT/PBE path, not the pass setup.

## Instruments

`AREA=half|quarter` + `MODE=empty|render|copy` + `SAMPLES=4` now separate four independent things:
**surface work, drawn work, pass setup, and per-sample work.** Any fix should move the surface term
(13.57 ms) toward 3.69 ms, not the fill term.
