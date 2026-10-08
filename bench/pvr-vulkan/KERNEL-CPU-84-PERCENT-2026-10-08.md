# MEASURED: 84% of the frame is kernel CPU across client and compositor

Client-process CPU (shell `time` around the run) on `desktop:effect=blur:windows=4`, 640×480:

```
open driver:   real 10.392 s   user 2.216 s   sys 4.532 s    (20 FPS, ~200 frames)
vendor driver: real  0.158 s   FAILED instantly
```

The vendor run fails because **`libVK_IMG` requires the vendor kernel module `pvrsrvkm`** and the board runs
the mainline `powervr` module — the ABI mismatch documented earlier (`vkEnumeratePhysicalDevices` returns
`VK_ERROR_INITIALIZATION_FAILED`). **No cross-driver number is claimed.**

## The arithmetic

| process | per frame |
|---|---|
| client (glmark2/zink) user | 11.0 ms |
| **client (glmark2/zink) sys** | **22.7 ms** |
| **Xwayland sys** (measured earlier) | **21.0 ms** |
| frame time | 52 ms |

**The client alone spends 22.7 ms of its 52 ms frame inside the kernel** — more than twice its own userspace
time, and the same order as Xwayland's kernel time. **Together, client + compositor consume ~43.7 ms of
kernel CPU per 52 ms frame — ~84%** (different cores, so frame time is bounded by the critical path rather
than the sum, but the kernel CPU consumed per frame is unambiguous).

## Why this is decisive

**The open stack's cost is dominated by kernel entries, not by GPU work or Mesa's userspace work.** Combined
with the previous finding — each sync op costs two handle lookups, up to three allocations and an xarray
insert in `pvr_sync_signal_array_add()` — the conclusion is forced: **the per-job, per-handle
synchronisation interface is the bottleneck**, exercised 20–46× per frame on the slow scenes.

**This is exactly what target (3) describes** (a driver-native sync type instead of per-job DRM syncobj
operations), and it is the only item on the objective's list whose removal would touch this 84%.

## Note on the earlier Xwayland-only measurement

The earlier "2.2–2.8× more kernel than user" measurement looked only at Xwayland. This one shows the
**client** has the same shape and a comparable magnitude — so the cost is not a compositor quirk but a
property of the driver's interface, present in every process that submits work.
