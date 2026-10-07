# Draft upstream issue: per-pass TA->3D overhead in drm/imagination (PowerVR BXM-4-64, A733)

Status: **draft, not filed.** Prepared 2026-10-08 from measurements on a Radxa Cubie A7A
(Allwinner A733, PowerVR B-Series BXM-4-64 MC1, BVNC 36.56.104.183).

## Summary

The mainline `powervr` Vulkan driver (Mesa `src/imagination`, kernel `drm/imagination`) is
**~2.3x slower per frame than the vendor `pvrsrvkm` driver** on the same hardware and the same
workload, and the difference is concentrated in the **TA->3D transition / inter-frame turnaround**,
not in userspace. Measured from inside the firmware trace, per frame at 512x512x60:

| | open | vendor | delta |
|---|---|---|---|
| `record` (userspace) | 0.343 ms | 0.030-0.049 ms | +0.31 |
| `submit` (userspace) | 0.197 ms | 0.046-0.060 ms | +0.15 |
| **`gpu_wait`** | **1.181 ms** | **0.600-0.667 ms** | **+0.58** |
| **total** | **1.721 ms** | **0.668-0.768 ms** | **+1.05** |

Vendor throughput at that size is 341-364 Mpix/s (three runs, repeatable).

## Why this is not a userspace problem

Even driving the entire CPU-side term (`record` + `submit` = 0.464 ms) down to the vendor's 0.076 ms
would leave the open stack at **~1.26 ms/frame against the vendor's 0.67**, because `gpu_wait` is
untouched at 1.18 ms and dominates. The remaining cost is the per-pass structure.

## What the TA->3D gap contains

The trace shows **41 firmware operations** between the TA finishing and the 3D job starting, every one
of them real work. The distinct ones, with per-step deltas in trace units (~0.195 us each):

```
  +35  Is TA: 1, finished: 1 on HW 0 ... FL different between TA/3D: global:1, local:2
  +32  UFL-TA-Base / FL-TA-Base
  +31  ALIST0 SP = 0, MLIST0 SP = 0
  +25  TA RTData finished on HW context 0
  +26  Perform TPC flush
  +79  UFO Updates for FWCtx
  +30  UFO Update
  +68  Deactivate MemCtx=0xc002c000
  +24  Ungrab reg set 1 refcount now 0
  +70  UFO Checks for FWCtx
  +30  UFO PR-Check
  +78  Ready-to-run debug OSid = 0, DM = 3
```

Notable: **`Deactivate MemCtx` and `Ungrab reg set 1 refcount now 0`** suggest the memory context and
register set are being torn down and re-acquired per transition rather than being held across passes.

## What was checked and is NOT the cause

Measured on the same board, each eliminating a candidate:

* **not the compositor** - weston's own composite is 0.58 ms (its debug timeline);
* **not the WSI present path** - `eglSwapBuffers` is 4.3-4.9 ms;
* **not Xwayland** - the vendor driver reaches 787 FPS through the *same* weston + Xwayland + zink;
* **not the swapchain image tiling** - a controlled test (`TILING=linear|optimal`, same scene/code)
  gives 211 vs 218 Mpix/s, no difference;
* **not the userspace job structure** - the driver issues 1 submit and exactly **3.00 jobs/submit**
  both windowed and off-screen, and sustains 183 submits/s off-screen;
* **not the unconditional partial-render job** - `pvr_drm_job_render.c` documents that the firmware
  performs no PRs when they are not needed, and geometry/PR/fragment are a single submit, so it does
  not add a TA->3D transition;
* **not a missing hardware feature in this path** - 32-bit float linear filtering is genuinely
  unsupported by the hardware (probed: sampling a 2x1 R32_SFLOAT image {0.0,1.0} at u=0.5 returns an
  endpoint, not 0.5), so the driver's exclusion of it is correct.

## Reproduction

The bench harness used lives alongside this file:

```
# per-frame userspace/firmware split, open vs vendor ICD
render-gap-vendor.txt            # vendor column, 512x512x60, three repeatable runs
results-2026-10-06-fw-trace.md   # open column + the 41-operation breakdown
# firmware trace, if a fresh capture is wanted:
echo 0x103 > /sys/kernel/debug/dri/1/pvr_params/fw_trace_mask
cat /sys/kernel/debug/dri/1/pvr_fw/trace_0
```

**Caveat on that trace**, learned the hard way: `pvr_fw/trace_*` is a persistent ring buffer that is
never cleared, and neither timestamps nor job refs provide a usable epoch separator, so it is reliable
for *counts* and for event *content* but not for per-run attribution without a fresh capture.

## Ask

Any guidance on reducing the per-pass TA->3D turnaround in `drm/imagination` - in particular whether
the memory-context deactivate/re-acquire and the register-set grab/ungrab in that window can be held
across consecutive render passes - would be the highest-value change available for this hardware.
