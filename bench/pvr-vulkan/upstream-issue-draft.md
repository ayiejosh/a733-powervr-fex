# Upstream issue draft — drm/imagination: per-pass TA→3D transition dominates render cost

**Status: drafted, NOT posted.** Posting is public under your GitHub account, so it
waits for an explicit go-ahead.

Suggested destination: `drm/imagination` (the mainline PowerVR kernel driver). The
firmware trace is driver-visible evidence, not speculation, so it belongs with the
kernel driver rather than Mesa.

---

## Title

`Per-render-pass TA→3D transition costs ~176 us of firmware time, independent of the work drawn`

## Body

### Summary

On a PowerVR BXM-4-64 (BVNC 36.56.104.183), an **empty** render pass costs ~310 us of
GPU time on the mainline `drm/imagination` driver, and ~176 us of that is the
firmware's TA→3D transition alone. The vendor's closed DDK completes a *full* pass
with a real draw in 0.351 ms, so an empty pass on the open driver costs roughly what
a complete pass costs on the vendor's.

### Hardware / software

- Radxa Cubie A7A, Allwinner A733 (arm64), kernel `6.6.98-5-aw2511`
- GPU BVNC 36.56.104.183, "PowerVR B-Series BXM-4-64 MC1", deviceID `0x36104183`
- `drm/imagination` backported to 6.6 (not the stock in-tree version)
- Mesa `libvulkan_powervr_mesa.so` over `/dev/dri/renderD128`
- GPU clock confirmed 1.104 GHz under sustained load, so DVFS is not a factor

### How to reproduce the trace

The firmware trace is off by default and the buffers are zero-length until enabled,
so nothing is recorded unless asked:

```bash
# TRACE|MAIN|MTS|CSW|RTD|HWR|HWP = 0x1+0x2+0x4+0x10+0x80+0x400+0x800
echo 0xC97 > /sys/kernel/debug/dri/1/pvr_params/fw_trace_mask
# run any workload with a render pass, then:
cat /sys/kernel/debug/dri/1/pvr_fw/trace_0
```

Mask bits are `ROGUE_FWIF_LOG_TYPE_*` in `pvr_rogue_fwif.h`.

### Measured per-frame breakdown

Steady state (frames 2+), timestamps in trace units, calibrated at ~0.195 us/unit
against a measured `gpu_wait` of 0.624 ms for 5 frames:

| stage | units | us |
|---|---|---|
| `CONTEXT_PB_BASE` → `Kick TA` | 182 | 35 |
| `Kick TA` → `TA finished` | 181 | 35 |
| `TA finished` → `Perform TPC flush` | 49 | 10 |
| **`TPC flush` → 3D setup** | **903** | **176** |
| 3D setup chain → `Kick 3D` | 272 | 53 |
| `Kick 3D` → `3D finished` | 178 | 35 |
| **empty render pass total** | | **~310** |

The 176 us gap contains: `Perform TPC flush`, the UFO PR-check, two context switches
(`DM=3`), a `KCCB Slot`/`cCCB Woff` round trip, `CONTEXT_PB_BASE` reprogramming, and
the freelist handoff.

Two captures reproduce within a few percent (903/911/903/922 units across four
steady-state frames on the second run).

### Observations that may be useful

1. **The freelist store/load is frame-1 only.** Frame 1 shows
   `Store Freelist type 0 → PMDM0`, `Load Freelist type 0 → PMDM1` (and the same for
   type 1), driven by `CONTEXT_PB_BASE set to 0x3, FL different between TA/3D:
   local: 1, global: 1`. From frame 2 the firmware reports `local: 0, global: 0` and
   skips it. So it is not the recurring cost — the 176 us gap persists without it.

2. **The unconditional partial-render job produces no kick.** The UMD submits
   geometry + a `FRAGMENT|PARTIAL_RENDER` job + the fragment job, which reads as
   three hardware jobs, but the firmware emits one `Kick TA` and one `Kick 3D`, and
   `Kick 3D ... Partial render:0`. So the per-pass job count is effectively two.

3. **An empty pass still walks the full setup.** Even with no draw and no copy, the
   pass pays the TA kick, the full TA→3D transition, and the 3D setup chain. Nothing
   appears to short-circuit a pass with no attachments or no draw.

### Why it matters

For a 512x512 offscreen render+readback loop, the open stack is 1.721 ms/frame
against the vendor's 0.668 ms/frame. The gap decomposes as `record` +0.313,
`submit` +0.151, `gpu_wait` +0.581, and the `gpu_wait` term is this per-pass
structure. Reducing the TA→3D transition would move it; nothing in the Mesa UMD
reaches it.

### What I'd like to know

- Is the TA→3D transition cost expected to be this large, or is something in this
  bring-up (BVNC quirk handling, single-core detection, context setup) inflating it?
- Is there a configuration that lets TA and 3D share freelist/context state so the
  transition is cheaper, or is the store/load inherent?
- Is a pass with no attachments / no draw supposed to short-circuit before the TA
  kick?

Happy to run further traces or test patches on the board.
