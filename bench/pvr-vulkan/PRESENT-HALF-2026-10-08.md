# The present half is the explicit-sync release wait; ~8 ms of it is unexplained

## Measured (open stack, 640x480, ACQ_TRACE/WSIREL_TRACE)

```
[rel] explicit-sync release waits  avg=12.4-12.6 ms   max=100.6 ms   images=3
[acq] AcquireNextImageKHR         avg=9.2-9.9 ms      max=100.7 ms
FPS 60, FrameTime 16.68 ms
```

**The release wait is 12.5 ms of a 16.7 ms frame - 75%** - and it reconciles with the off-screen
measurement exactly:

```
client render (off-screen, measured)   4.08 ms
release wait                          12.5 ms
windowed frame                        16.7 ms     (4.08 + 12.5 = 16.6) OK
```

So the decomposition's "present 5.4x" **is** this wait; the acquire wait is the same thing seen from
the client side.

## How much is weston's own slow composite?

Weston composites through the same slow driver in this configuration, so its composite is also ~4.4x
slow:

```
composite work = client surface = 0.307 Mpix
vendor rate 328 Mpix/s -> 0.94 ms
open   rate  75 Mpix/s -> 4.08 ms
```

**That accounts for only ~4 ms of the 12.5 ms. ~8 ms remains unexplained**, and it is not the
compositor's render time.

## Remaining candidates (present/sync path, not renderer)

1. The explicit-sync round trip itself - commit -> weston -> signal -> client wake, including Mesa's
   WSI and the open kernel module's syncobj handling. The create/destroy churn (target 1) lives here:
   ~209 ioctls/frame.
2. Weston's repaint scheduling - composite on commit, or wait for its own tick.
3. A per-frame buffer handoff in the WSI absent from the off-screen path.

## Cheap discriminator now available

Run the **client** on the open driver while **weston** uses the vendor Vulkan (or vice versa): the two
processes take their ICD from their own environment, so they can be put on different drivers without
moving the kernel module. If the release wait collapses when weston is fast, the ~8 ms is
weston-side; if it stays, it is the client's WSI or the open kernel module's sync path.
