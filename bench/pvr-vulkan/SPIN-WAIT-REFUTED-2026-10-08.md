# The spin-wait hypothesis is REFUTED — it is not the client's kernel CPU

## The hypothesis

`vk_drm_syncobj.c` contains a **spin loop** for `VK_SYNC_WAIT_PENDING` on non-timeline syncs, with the
comment:

> *"Sadly, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE was never implemented for drivers that don't support
> timelines. Instead, we have to spin on DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE until it succeeds."*

A `sched_yield()` loop burning CPU on repeated ioctls looked like the obvious source of the client's
measured **~23 ms/frame of system time**.

## The test

Made the spin sleep 50 µs instead of `sched_yield()`, behind `PVR_SPIN_SLEEP`, rebuilt, and **verified the
patch was genuinely in the loaded library** (`strings` found the env-var name; the ICD points at the rebuilt
`build/src/imagination/vulkan/libvulkan_powervr_mesa.so`). Then A/B'd:

| variant | real | user | **sys** | FPS |
|---|---|---|---|---|
| spin (default) | 10.618 s | 2.070 s | **4.972 s** | 20 |
| **sleep 50 µs** | 10.452 s | 2.122 s | **4.939 s** | 20 |

**No change. Hypothesis refuted; patch reverted.**

## What this rules out

**The `spin_wait_for_sync_file()` busy-loop is not the client's kernel CPU.** So the client's waits do not
take the `VK_SYNC_WAIT_PENDING`-on-binary-sync path in any quantity — the condition at `vk_drm_syncobj.c`
must rarely be satisfied by this driver's traffic.

## Where that leaves the 84%

**Still attributable to the ~190 syncobj ioctls per frame** (52 CREATE + 52 DESTROY + 72 TRANSFER, measured
in the earlier timeline attempt) — i.e. to the **sheer number of kernel round trips**, each costing the
handle lookups and allocations catalogued in `pvr_sync_signal_array_add()` (two lookups, up to three
allocations, an xarray insert).

**The fix is unchanged: stop paying a round trip per job** — which only a driver-native or timeline
representation can do.

## The correct next step is already planned

`SYNC-TIMELINE-ATTEMPT-2026-10-08.md`:

1. ~~Add persistent timeline syncs, unused~~ — **landed** (`job_sync[]` created, verified
   behaviour-identical)
2. **Convert one job type at a time**, starting with GEOM, then FRAG / COMPUTE / TRANSFER / QUERY — each
   path's create → advance the timeline point, its destroy removed, its wait/signal values set to
   `job_value-1` / `job_value`; **verify after each** (a wrong point shows as wrong rendering or a hang, so
   the probe suite is a real gate)
3. The event paths last
4. Drop the per-job syncs entirely

**Baseline to beat: ~190 syncobj ioctls/frame, ~23 ms of client system time per frame.** The plan's own
warning applies: a half-applied state is dangerous, so each step must leave `pvr_arch_queue.c`
self-consistent.
