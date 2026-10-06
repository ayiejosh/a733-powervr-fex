# Where the open driver's per-pass GPU time actually goes (2026-10-06)

Firmware-level trace of one `vkrender 512 512 5` run, `MODE=empty` (no draw, no copy),
open stack (`powervr` + Mesa pvr). Raw trace: `fwtrace-2026-10-06-open-empty.txt`.

> **Correction, same day.** This first capture enabled 7 of the 16 log groups
> (`0xC97`), and the 176 us TA→3D gap showed almost nothing. I read that as the
> firmware being idle and waiting on the kernel. **That was wrong.** With all groups
> enabled (`0x80007FFF`, `fwtrace-2026-10-06-open-empty-allgroups.txt`) the gap is
> **41 firmware operations**, not idle time. See "What the gap actually contains"
> below. The mask matters: a partial mask makes firmware work look like silence.
>

## How to reproduce

The mainline driver exposes the firmware trace buffer and its mask:

```bash
# /sys/kernel/debug/dri/1/pvr_params/fw_trace_mask
#   TRACE|MAIN|MTS|CSW|RTD|HWR|HWP = 0x1+0x2+0x4+0x10+0x80+0x400+0x800 = 0xC97
sudo sh -c 'echo 0xC97 > /sys/kernel/debug/dri/1/pvr_params/fw_trace_mask'
PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 VK_ICD_FILENAMES=/home/radxa/pvr_gen_icd.json \
  PVR_TIMING=1 MODE=empty ./vkrender 512 5
sudo cat /sys/kernel/debug/dri/1/pvr_fw/trace_0
```

`fw_trace_mask` is 0 by default and the trace buffers are 0 bytes, so nothing is
recorded unless it is turned on. Mask bits are in the kernel's `pvr_rogue_fwif.h`
(`ROGUE_FWIF_LOG_TYPE_*`).

## Timestamp calibration

The trace's own unit is not documented, so it is calibrated against a measurement
instead of assumed: the same run reported `gpu_wait=0.624 ms` for 5 frames, and the
trace spans ~3200 units/frame. That gives **≈0.195 µs per unit**, and the stage
breakdown below sums to 680 µs against a measured 624 ms/frame — consistent.

## Per-frame stage breakdown (steady state, frames 2-5)

| stage | units | µs | note |
|---|---|---|---|
| `CONTEXT_PB_BASE` → `Kick TA` | 182 | 35 | geometry kick |
| `Kick TA` → `TA finished` | 181 | 35 | TA actually runs |
| `TA finished` → `Perform TPC flush` | 49 | 10 | |
| **`TPC flush` → 3D setup** | **903** | **176** | **largest single item** |
| 3D setup chain → `Kick 3D` | 272 | 53 | stack pointers, tiles in flight |
| `Kick 3D` → `3D finished` | 178 | 35 | |
| `3D finished` → next frame's setup | ~1900 | ~370 | host turnaround, GPU idle |

**The whole empty render pass is ~310 µs of GPU time** (the first six rows). For
comparison the *vendor* does a full pass with a real draw in 0.351 ms total — so the
open driver's empty pass costs about what the vendor's complete pass costs.

## What the trace corrected

Two things I had wrong from reading source alone:

1. **Two kicks per pass, not three.** The winsys builds a geometry job, an
   unconditional partial-render (PR) job, and a fragment job
   (`pvr_drm_job_render.c:490-520`), which reads as three hardware jobs. The firmware
   shows one `Kick TA` and one `Kick 3D`, and `Kick 3D ... Partial render:0` — the PR
   job produces no kick and no partial render. The source comment at
   `pvr_drm_job_render.c:589-592` says as much; the trace confirms it.
2. **`Load Freelist` is not per-frame.** It appears 4 times in the whole trace, all in
   the first frame's setup. What *is* per-frame in frame 1 only is the freelist
   **store/load pair** for the TA→3D handoff, driven by
   `FL different between TA/3D: local: 1, global: 1`. From frame 2 on the firmware
   reports `local: 0, global: 0` and skips it.

## The TA→3D gap is not UMD-controllable

The 176 µs gap between `TA finished` and the 3D setup is firmware bookkeeping:
`Perform TPC flush`, the UFO PR-check, two context switches (`DM=3`), a
`KCCB Slot`/`cCCB Woff` round trip, `CONTEXT_PB_BASE` reprogramming, and the freelist
handoff.

The UMD does not choose TA vs 3D freelists. `pvr_drm_job_render.c:343-346` fills the
context's `free_list_handles[2]` as:

```c
.free_list_handles = {
   [PVR_DRM_FREE_LIST_LOCAL]  = drm_free_list->handle,
   [PVR_DRM_FREE_LIST_GLOBAL] = parent_free_list_handle,
},
```

That is LOCAL vs GLOBAL, not TA vs 3D — the array is indexed by memory class
(`PVR_DRM_HWRT_FREE_LIST_LOCAL`/`_GLOBAL` in the UAPI). The TA/3D differentiation is
the kernel's and the firmware's.

## Consequence for "make the open driver beat the vendor"

Measured, 512²×60, warmed:

| | open | vendor | delta |
|---|---|---|---|
| `record` | 0.343 | 0.030 | +0.313 |
| `submit` | 0.197 | 0.046 | +0.151 |
| `gpu_wait` | 1.181 | 0.600 | **+0.581** |
| **total** | **1.721** | **0.668** | **+1.05** |

`record`+`submit` = 0.464 ms is UMD-addressable (the EOT-cache fix already took
0.13 ms of it). **Even driving that entire CPU-side term to the vendor's 0.076 ms
would leave the open stack at ~1.26 ms/frame against the vendor's 0.67** — because
`gpu_wait` is untouched at 1.18 ms and is now dominated by the per-pass structure
above.

So: **beating the vendor is not reachable from Mesa.** The remaining gap is the
TA→3D transition plus the inter-frame turnaround, which is `drm/imagination` and
firmware behaviour. The useful next step is an upstream issue with this trace, not
more UMD tuning.

## What the gap actually contains (all 16 log groups)

The TA→3D gap is **41 firmware operations**, every one of them real work. The
distinct ones, with the per-step deltas in trace units (~0.195 µs each):

```
  +35  Is TA: 1, finished: 1 on HW 0 ... FL different between TA/3D: global:1, local:2
  +32  UFL-TA-Base / FL-TA-Base
  +31  ALIST0 SP = 0, MLIST0 SP = 0
  +25  TA RTData finished on HW context 0
  +26  Perform TPC flush
  +79  UFO Updates for FWCtx
  +30  UFO Update
  +68  Deactivate MemCtx=0xc002c000          <-- same MemCtx as below
  +24  Ungrab reg set 1 refcount now 0
  +70  UFO Checks for FWCtx
  +30  UFO PR-Check
  +78  Ready-to-run debug OSid = 0, DM = 3
  +24  Client command header DM = 3
  +84  Check Pow state: Int: 0x2, Ext: 0x1, Fence Counters: Check: 23 - Update: 25
  +28  Initiate powoff query for RD-DMs
  +90  Kick MTS Irq task DM=0
  +39  KCCB Slot / cCCB Woff update
  +68  Ready-to-run debug OSid = 0, DM = 3
  +39  PM running primary config (Core 0)
  +33  Activate MemCtx=0xc002c000 DM=3 secure=0   <-- same MemCtx as above
  +28  Setup register set=1 DM=3, PC address=...
  +29  Grab reg set 1 refcount now 1
  +92  Store Freelist type 0
  +50  Load  Freelist type 0
  +46  Store Freelist type 1
  +47  Load  Freelist type 1
  +37  CONTEXT_PB_BASE set to 0x0, FL different between TA/3D: local: 0, global: 0
  +48  Loading stack-pointers for 1 (0:MidTA,1:3D) on context 1
  +43  3D Buffers: FWCtx ... on ctx 1
  +23  3D RTData ready on HW context 1
 +109  Updating Tiles In Flight (Dusts=1, PartitionMask=0x00000005, ISPCtl=0x80015000)
  +22    Phantom 0: USCTiles=6
  +37  Is TA: 0, finished: 0 on HW 1 ... FL different between TA/3D: global:0, local:0
  +29  UFL-3D-Base / FL-3D-Base
  +26  ALIST1 SP = 0, MLIST1 SP = 0
```

So the per-pass cost is a **full context teardown and rebuild between the TA and the
3D core**: the same `MemCtx=0xc002c000` is deactivated and then reactivated, the
register set is ungrabbed and regrabbed, a power-off query runs, an MTS IRQ round
trip happens, and both freelists are stored and reloaded.

## Runtime PM is not the lever

`Initiate powoff query` looked actionable, so it was tested: forcing the GPU's
runtime PM to `on` (`/sys/bus/platform/devices/1800000.gpu/power/control`) changes
nothing.

```
power/control = auto : gpu_wait 0.820 0.808 0.828 ms
power/control = on   : gpu_wait 0.809 0.824 0.826 ms
```

The power-off query is a firmware-internal decision, not driven by Linux runtime PM.

## What this settles

The 176 µs is 41 firmware operations that no layer I can modify controls: not the
Mesa UMD (it does not choose TA/3D contexts or freelists), not Linux PM (measured
above), and not the kernel driver's scheduling (the gap is full of firmware work,
not idle time). Combined with the earlier rounds, the render gap is firmware
per-pass context management.
