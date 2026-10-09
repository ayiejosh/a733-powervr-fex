# Does the A733 VPU expose motion vectors? — answered

**Question posed by the LSFG/Mako research as its highest-value unknown** ("if MVs are available the board has a
cheap motion source nothing else on ARM can match"). Answered from the library's own DWARF, no guessing.

## Method

`readelf --debug-dump=info /lib/aarch64-linux-gnu/libvdecoder.so` — **the library ships `debug_info` and is NOT
stripped** (as are `libvideoengine.so` and `libVE.so`), so struct layouts are directly readable.

## The answer

**`VIDEO_FRM_MV_INFO` exists as a struct. Its members and offsets:**

| offset | members |
|---|---|
| 12 | `nMaxMv_x`, `nMaxMv_y` |
| 14 | `nMinMv_x`, `nMinMv_y` |
| 16 | `nAvgMv_x`, `nAvgMv_y` |
| 18 | `nAvgMv`, `nMinMv`, `nMaxMv`, `SkipRatio` |
| (separate) | `nMvInfo` |

**~20 bytes, all 16-bit fields. These are PER-FRAME SUMMARY STATISTICS, not a motion field.** A search of the
entire DWARF for any dense array (`mv_buf`, `pMv`, `MvArray`, `MvNum`, `nMvSize`, `MvInfoBuf`) returns **nothing**.

**So: the hardware can tell you *how much* the frame moved and *how static* it was — not *where each block moved*.**

## Supporting evidence

* `vdecoder.h` has **zero** MV references — the public API never exposes any of this.
* No MV symbols in `libvdecoder`, `libvideoengine`, `libVE`, `libcdc_base` or `libvencoder`.
* The binary strings contain the field names because the library is unstripped.
* A `ProcInfo` mechanism exists with named functions and a control flag:
  `updateProcInfo`, `CdcVeProcInfoUpdate`, `CdcVeProcInfoReset`, `SbmUpdateProcInfo`, `FbmUpdateProcInfo`,
  and the flags `bSetProcInfoEnable`, `nSetProcInfoFreq`, `setProcInfo`, `stopProcInfo`.
  **That flag is the likely switch for enabling the summary output.**

## What this is actually worth

**Viable for coarse gating, not for dense warping:**

* **static-frame detection** — `SkipRatio` high and `nAvgMv ≈ 0` means the frame barely changed: **you can skip
  interpolation entirely and just repeat or blend.** For a frame-generation scheme this removes the cost on the
  easy majority of frames.
* **global pan/zoom** — `nAvgMv_x/y` is the camera/scroll motion, enough to drive a cheap global warp.
* **scene-change / complexity signal** — `nMaxMv` vs `SkipRatio` distinguishes a cut from motion.

**NOT viable for ANVIL-style flow:** that needs a per-macroblock field to densify and correct with a learned
residual. **There is no such field to read.**

## Cost to obtain the summaries

**Cheap, because the library is unstripped**: the struct layout is known (above), the accessors are named
(`updateProcInfo`, `CdcVeProcInfoUpdate`), and `bSetProcInfoEnable` likely turns them on. **A small program
against `libvdecoder` should be able to read them** — no disassembly required.

## Caveat

**Not yet executed.** This is a static analysis of a shipped binary; reading the fields at runtime is the next
step and has not been attempted (the VE must be touched one session at a time — it Oopses the kernel under
concurrent use).
