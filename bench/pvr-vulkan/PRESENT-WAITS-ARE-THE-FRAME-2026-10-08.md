# The two WSI waits ARE the windowed frame - and what that means

| component | ms |
|---|---|
| measured frame at 640x480 windowed (45 FPS) | 22.2 |
| release wait (measured) | 12.5 |
| acquire wait (measured) | 9.5 |
| **render + everything else** | **0.2** |

**The entire windowed frame is the two WSI waits.** The render is 0.2 ms, so on this workload the
per-tile render deficit (3.68x on 0.3 Mpix) is sub-millisecond and irrelevant - the whole present gap is
the waits.

## The honest caveat, which reframes the objective's central metric

The waits exist because **Mesa's WSI correctly waits for the compositor**: the client renders in 0.2 ms
while weston repaints on vsync (~16.7 ms), so the client outruns the compositor, exhausts its buffers,
and waits. **That is correct behaviour** - it is what prevents overwriting a buffer the compositor still
holds.

**The vendor's WSI does not wait at all** (traces never fire, 1021 FPS). Its client renders 1021 frames/s
into a 60 Hz display, so the overwhelming majority are never shown.

**So the objective's "787 FPS vendor vs ~31 open" measures how fast a client can render when it does
NOT synchronise with the compositor, against one that does.** Closing that specific gap means either:

1. **Matching the vendor's non-waiting behaviour** - faster numbers, but it trades away the buffer-reuse
   guarantee. Not obviously a fix; arguably a regression.
2. **Acknowledging the metric is partly artificial** - for anything actually displayed on a 60 Hz
   output both are capped at 60 FPS, and the open stack's present path is not what limits real output.

## Not explained by this

The waits do not explain the **render** gap: at 2048x2048 (no compositor, no WSI) the open driver is
3.68x slower per tile, independent of all of this. **The render per-tile deficit is real and separate.**

## Where this leaves the objective

* **Render per-tile 3.68x** - real, measured compositor-free, not yet localised to an operation.
* **Per-pass syncobj 74x** - root-caused to the kernel UAPI, ~5% payoff on a real client.
* **Present 22.6x** - **100% the WSI waits**, correct behaviour on Mesa's side, absent on the vendor's.
  This term is not a defect in the same sense as the other two.
