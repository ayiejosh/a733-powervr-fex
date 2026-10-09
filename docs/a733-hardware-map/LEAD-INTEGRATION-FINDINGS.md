# Lead integration findings — contradictions, connections, and the reopened paths

**Round 1 of the full-hardware-map goal (`goal-201ac446`). These are the Lead's own findings while five
workstreams run. Everything here was measured or read directly.**

## 1. FALSIFIED: "VE2 has no usable hardware decode"

**A prior session recorded, as definitive and "proven"** (`knowledge/sunshine-ve2.md`, 2026-06-05):

> *"VE2 HAS NO USABLE HARDWARE DECODE — proven. H264 AND H265 both rejected at
> `libvideoengine.so::checkQualification` → `VideoEngineCreate:434 unsupported format`
> (chipId=42, ic_version 0x…21320, decIpVersion 33310)"*

**It is wrong.** Measured today, 300 frames of 1280x720 H.264:

| path | wall | **CPU** | cores |
|---|---|---|---|
| **`libvdecoder` (VPU)** | 1.658 s | **1.036 s** | **0.6** |
| ffmpeg software | 2.118 s | 4.623 s | 2.2 |

**4.5x less CPU → the hardware is decoding.** Independently confirmed twice:
`demoDecoder finish. decode frame: 37, display frame: 30`, 40089600 B out, `cedar_dev` interrupts advancing.

**And the decoder registers a full codec list at init:**

```
h264 ✓   h265 ✓   Vp9Hw ✓   mpeg2 ✓   mpeg4 (H263/dx/normal/Vp6) ✓
vp8 ✓    mjpeg ✓  mjpegplus ✓  avs ✓  avs2 ✓     vp6Soft ← the ONLY one named "Soft"
```

**Root cause of the prior false negative:** it tested the low-level `GetVeOpsS(VE_DEC_MODE=1)` /
`libvideoengine` path directly, with `bIsSoftDecoderFlag=1` and `bCalledByOmxFlag=1` variations, and treated the
rejection as a hardware limit. **The vendor's `libvdecoder` wraps that API correctly and works** — the same
mistake this session kept making: concluding from a failed call rather than from the device.

**Consequence: the prior session pivoted away from decode and wrote "a VAAPI decode driver would either fail
(H264/H265) or wrap a software VP9 decoder no better than libvpx". Both halves are now wrong, so a VA-API
DECODE driver is a live option, not a dead end.**

## 2. The working VA-API ENCODE driver existed and is now gone

**A prior project got VA-API H.264 encode working** (Sunshine selected `h264_vaapi [vaapi]`, ~74 fps @1080p,
~0.3 core) and deployed it as **`/usr/lib/aarch64-linux-gnu/dri/pvr_drv_video.so`** — **not**
`sunxi-drm_drv_video.so`. Reason: Sunshine carries `cap_sys_admin+p` → `AT_SECURE` → `libva`'s `secure_getenv`
returns NULL → it cannot honour `LIBVA_DRIVER_NAME` and derives the name from the DRM node ("pvr").

**Current state: all three candidate names are ABSENT.** The deployed artifact was lost; the **source survives**
at `backups/a733-backup/ve2/ve2-vaapi/sunxi_ve_drv_video.c`. **So "restore the VA driver" is a previously
achieved outcome, not a research project.**

## 3. The connection map — and the chain my earlier "no" missed

```
                 ┌──────────── dmabuf / dma_heap (the glue) ────────────┐
                 │                                                      │
  VPU decode ────┴─► YUV frame ──► DE video plane ──► CSC + scaler (hw) ──► scanout
   (0.6 cores)       (7 planes accept YUY/YCbCr)      (6 scalers idle)     zero CPU copy
                                            │
  VPU encode ◄── BGR0/NV12 ◄── libyuv NEON conversion ◄── compositor output
   (0.4 cores)                  (prior fix: 387fps/2.58ms, 30x)
```

**Every stage of that chain is now measured as available EXCEPT the glue, and the glue is dmabuf.**

## 4. The prior work's own "find the 20%" example — a middle stage capping the pipeline

**This is exactly the failure mode the goal asks about**, recorded in `sunshine-ve2.md`:

| stage | measured | verdict |
|---|---|---|
| screen capture (`x11grab`) | 60 fps sustained | **not the bottleneck** |
| **BGR0→NV12 colour conversion (CPU libswscale)** | **~13 fps** | **THE bottleneck** |
| VE2 H.264 encode | 74 fps | not the bottleneck |

**Two fast stages straddling one slow one.** Fixed by replacing libswscale with **libyuv NEON
`ARGBToNV12Matrix`: 387 fps / 2.58 ms per frame, 30x faster.** **A textbook example of the goal's "component A
drops to 80% → investigate it" rule, and it was found by measuring every stage, not by reading code.**

## 5. Other prior findings that change the current picture

* **One VE engine is shared by decode and encode** — the prior work traced a Moonlight freeze to the single
  Cedar engine doing YouTube decode and stream encode at once, starving the encoder. **Concurrency is a real
  constraint, and it is measurable.**
* **HEVC encode is blocked on IC 21320**: `libvenc_h265` emits **zero IDR NALs** at every setting tried
  (including `idr_period=1`) — all pictures are TRAIL_R. H.264 `ForceKeyFrame`→IDR works fine.
* **VE IOMMU leak**: repeated failing `VideoEncodeOneFrame` calls leaked SMMU mappings; `rmmod`/`modprobe`
  `sunxi_ve` did **not** clear them (reboot required). **Do not hammer failing encode calls.**
* **`sunxi_ve` rejects some encoder params on this chip**: `VENC_IndexParamSetVbvSize` /
  `SetFrameLenThreshold` and `VENC_IndexParamRgb2Yuv` return errors, and VBV caused stream corruption
  ("cbp too large").
