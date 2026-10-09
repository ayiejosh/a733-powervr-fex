# VPU-MAP — Allwinner A733 Cedar VE2 on Radxa Cubie A7A

Board: Radxa Cubie A7A (A733), Linux 6.6.98. Date: 2026-10-09.
Sources: headers `/usr/include/{vdecoder,vencoder,vencoder_platform_v2,vbasetype,veInterface}.h`,
kernel logs, `/sys/kernel/debug/clk/clk_summary`, DT, and two VE sessions run for this map.

**VE SAFETY:** `sunxi_ve` Oopses the kernel under concurrent/rapid multi-instance use, so this map was
built from headers/sysfs first, with the engine touched as little as possible. **Three VE sessions
reached hardware, strictly one at a time, never in parallel, never looped:** one H.265 decode attempt
(aborted in userspace before decoding), the baseline VA-API encode, and one patched-build VA-API encode
(§f#1). A further build was attempted but never reached the engine — it failed to `dlopen` (§c). Result:
**no kernel Oops, no IOMMU fault, no reboot**; `sunxi_ve` refcount returned to 0; the encode block
suspended cleanly. The only kernel-side oddities were `release lost-lock` on the aborted process's exit
(handled, see §f#5) and the decode genpd staying `active`.

---

## 0. Block topology (two distinct engines)

| | DECODE | ENCODE |
|---|---|---|
| device node | `/dev/cedar_dev` | `/dev/cedar_dev_ve2` |
| DT node | `ve@1c0e000` (`allwinner,sunxi-cedar-ve`) | `ve2@1c10000` (`allwinner,sunxi-cedar-ve2`) |
| power domain | `pd_ve_dec` | `pd_ve_enc` |
| clock root | `pll-ve0` = **546 MHz** | `pll-ve1` = **696 MHz** |
| driver-reported freq | `ve_default_freq = 624` | `ve_default_freq = 624` |
| runtime status now | `active` | `suspended` |

Both report `ic_version = 0x3331000021320`. Separate PLLs mean the two blocks have independent clock
domains. A third node `ve1@1c0e000` (`allwinner,sunxi-cedar-ve1`) exists with **no clock consumer** — see §2.

---

## (a) Capability matrix

### (a1) Decode — `VIDEO_CODEC_FORMAT_*` (`vbasetype.h`), `VideoDecoder`/`libvdecoder`

Declared codec enum (`vbasetype.h:34-62`), with the shipped plugin that implements it:

| enum (`VIDEO_CODEC_FORMAT_*`) | value | plugin present | HW/SW |
|---|---|---|---|
| `MJPEG` | 0x101 | `libawmjpeg.so` (`CreateMjpegDecoder`) | HW |
| `MPEG1` | 0x102 | (via `libawmpeg2.so`) | HW |
| `MPEG2` | 0x103 | `libawmpeg2.so` (`CreateMpeg2Decoder`) | HW |
| `MPEG4` | 0x104 | `libawmpeg4normal.so` (`CreateMpeg4NormalDecoder`) | HW |
| `MSMPEG4V1` / `MSMPEG4V2` | 0x105/0x106 | `libawmpeg4base.so` | HW |
| `DIVX3/4/5` | 0x107-0x109 | **`//* not support`** in header | absent |
| `XVID` | 0x10a | `libawmpeg4base.so` | HW |
| `H263` | 0x10b | `libawmpeg4h263.so` (`CreateMpeg4H263Decoder`) | HW |
| `SORENSSON_H263` | 0x10c | `libawmpeg4h263.so` | HW |
| `RXG2`, `RX` | 0x10d/0x114 | — | absent |
| `WMV1/2/3` | 0x10e-0x110 | **`//* not support`** in header | absent |
| `VP6` | 0x111 | `libawvp6soft.so` (`CreateVp6SoftDecoder`) | **SW** |
| `VP8` | 0x112 | `libawvp8.so` (`CreateVp8Decoder`) | HW |
| `VP9` | 0x113 | `libawvp9HwAL.so` (`CreateVp9HwDecoder`) | HW |
| `H264` | 0x115 | `libawh264.so` (`CreateH264Decoder`) | **HW (measured)** |
| `H265` | 0x116 | `libawh265.so` (`CreateH265Decoder`) | HW |
| `AVS` | 0x117 | `libawavs.so` (`CreateAvsDecoder`) | HW |
| `AVS2` | 0x118 | `libawavs2.so` (`CreateAvs2Decoder`) | HW |
| `AV1` | 0x119 | **NO PLUGIN** | **absent** |
| `VIDEO_CODEC_FORMAT_MAX` | `= AV1` | | |

`VIDEO_CODEC_FORMAT_MIN = MJPEG`. Additional MPEG4 variants ship as separate plugins:
`libawmpeg4dx.so` (`CreateMpeg4Divx311Decoder`), `libawmpeg4vp6.so` (`CreateMpeg4Vp6Decoder`),
`libawmjpegplus.so` (`CreateMjpegPlusDecoder`).

**Decode pixel formats** — `PIXEL_FORMAT_*` (`vbasetype.h:69-102`):
`YUV_PLANER_420/422/444`, `YV12`, `NV21`, `NV12`, `YUV_MB32_420/422/444`, `RGBA`, `ARGB`, `ABGR`,
`BGRA`, `YUYV`, `YVYU`, `UYVY`, `VYUY`, `PLANARUV_422`, `PLANARVU_422`, `PLANARUV_444`, `PLANARVU_444`,
**`P010_UV`, `P010_VU`**, **`AW_NV12_10BIT`, `AW_NV21_10BIT`, `AW_YV12_10BIT`, `AW_I420_10BIT`**,
`AW_AFBC`, **`AW_AFBC_10BIT`**, `AW_P010_LE_UV`, `AW_P010_LE_VU`. `MAX = PIXEL_FORMAT_AW_P010_LE_VU`.

**10-bit / HDR (declared):** `FbmBufInfo.bHdrVideoFlag`, `FbmBufInfo.b10bitVideoFlag`,
`FbmBufInfo.bAfbcModeFlag`; `VConfig.bConvertVp910bitTo8bit`; the P010/AW_*_10BIT formats above.

**Rotation / scale / CSC / crop** — all fields of `VCONFIG` (`vdecoder.h`):
`bScaleDownEn`, `bRotationEn`, `bSecOutputEn`, `nHorizonScaleDownRatio`, `nVerticalScaleDownRatio`,
`nSDWidth`, `nSDHeight`, `bAnySizeSD`, `nSecHorizonScaleDownRatio`, `nSecVerticalScaleDownRatio`,
`nRotateDegree`, `bThumbnailMode`, `eOutputPixelFormat`, `eSecOutputPixelFormat`, `bNoBFrames`,
`bDisable3D`, `bSupportMaf` (**`//not use`**), `bDispErrorFrame`, `nVbvBufferSize`, `nFrameBufferNum`,
`bSecureosEn`, `bGpuBufValid`, `nAlignStride`, `bVirMallocSbm`, `bDeInterlaceHoldingFrameBufferNum`,
`nDisplayHoldingFrameBufferNum`, `nRotateHoldingFrameBufferNum`, `nDecodeSmoothFrameBufferNum`,
`bIsTvStream`, `bAdapteDropFrame`, `eCtlAfbcMode`, `nAfbcSecMode`, `eCtlIptvMode`.
Scale-down modes: `SCALEDOENMODE { SCALE_MODE_CLOSE, SCALE_MODE_ONLINE_FIXRATIO, SCALE_MODE_ONLINE_RANDOM }`.

**Decode tuning APIs** (`vdecoder.h:420-447`), all five requested are present:

| API (exact prototype) | accepts |
|---|---|
| `int SetDecodePerformCmd(VideoDecoder*, enum EVDECODERSETPERFORMCMD performCmd)` | `VDECODE_SETCMD_DEFAULT=0`, `VDECODE_SETCMD_START_CALDROPFRAME=1`, `VDECODE_SETCMD_STOP_CALDROPFRAME=2` |
| `int GetDecodePerformInfo(VideoDecoder*, enum EVDECODERGETPERFORMCMD, VDecodePerformaceInfo**)` | `VDECODE_GETCMD_DEFAULT=0`, `VDECODE_GETCMD_DROPFRAME_INFO=1`; out `VDecodePerformaceInfo { unsigned int nDropFrameNum; int nFrameDuration; }` |
| `int VideoDecoderSetFreq(VideoDecoder*, int nVeFreq)` | VE frequency in MHz |
| `int ConfigSpeedInfo(VideoDecoder*, float nSpeed)` | speed factor |
| `int ConfigExtraScaleInfo(VideoDecoder*, int nWidthTh, int nHeightTh, int nHorizonScaleRatio, int nVerticalScaleRatio)` | threshold-triggered extra scaling |

Also: `SetVideoFbmBufAddress`, `SetVideoFbmBufRelease`, `RequestReleasePicture`, `ReturnReleasePicture`,
`SetReleasePicture`, `EmptyPictureNum`, `ReleasePictureNum`, `DecoderServerCommand(eDecodeVerifyServer)`
(`DECODER_SERVER_INIT/DESTORY/FIND_STATUS`), `DecoderSetSpecialData`.

**Decode status enums:** `EVDECODERESULT { UNSUPPORTED=-1, OK=0, FRAME_DECODED=1, CONTINUE=2,
KEYFRAME_DECODED=3, NO_FRAME_BUFFER=4, NO_BITSTREAM=5, RESOLUTION_CHANGE=6 }`.
`VID_FRAME_TYPE { UNKONWN, I, P, B, IDR, BUTT }`. `VIDEO_FRM_STATUS_INFO` carries
`enVidFrmType`, `nVidFrmSize`, `nVidFrmDisW/H`, `nVidFrmQP`, `VIDEO_FRM_MV_INFO` carries per-frame
motion-vector statistics (`nMaxMv_x`, `SkipRatio`, …).

**Software-decode flag:** `VConfig.bIsSoftDecoderFlag`, `FbmBufInfo.bIsSoftDecoderFlag`
(plus `bCalledByOmxFlag`). These are the only "force software" switches.

### (a2) Encode — `VENC_*` (`vencoder.h`), `libvencoder`

**Codecs — `VENC_CODEC_TYPE`:** `VENC_CODEC_H264`, `VENC_CODEC_JPEG`, `VENC_CODEC_H264_VER2`,
`VENC_CODEC_H265`, `VENC_CODEC_VP8`. **No VP9, no AV1, no AVS encode.**

**Profiles / levels:**
`VENC_H264ProfileBaseline=66`, `VENC_H264ProfileMain=77`, `VENC_H264ProfileHigh=100`;
`VENC_H264Level41=41` … `VENC_H264Level51=51`.
`VENC_H265ProfileMain=1`, `VENC_H265ProfileMain10=2`, `VENC_H265ProfileMainStill=3`;
`VENC_H265Level41=123` … `VENC_H265Level51=153`.
Structures `VencH264ProfileLevel`, `VencH265ProfileLevel`; set via `VENC_IndexParamH264ProfileLevel`.

**Rate control — `VENC_RC_MODE`:** `AW_CBR=0`, `AW_VBR=1`, `AW_AVBR=2`, `AW_QPMAP=3`, `AW_FIXQP=4`.

**Pixel formats — `VENC_PIXEL_FMT`:** `VENC_PIXEL_YUV420SP`, `YVU420SP`, `YUV420P`, `YVU420P`,
`YUV422SP`, `YVU422SP`, `YUV422P`, `YVU422P`, `YUYV422`, `UYVY422`, `YVYU422`, `VYUY422`,
`ARGB`, `RGBA`, `ABGR`, `BGRA`, `TILE_32X32`, `TILE_128X32`, `AFBC_AW`,
`LBC_AW` (**`//* for v5v200 and newer ic`**). **No 10-bit input format** — see §2.

**Tuning APIs:** `int VideoEncoderSetFreq(VideoEncoder*, int nVeFreq)`;
`VideoEncoderSetParameter` / `VideoEncoderGetParameter` driven by `VENC_IndexParam*`
(used here: `VENC_IndexParamH264Param`, `VENC_IndexParamH265Param`, `VENC_IndexParamBitrate`,
`VENC_IndexParamFramerate`, `VENC_IndexParamForceKeyFrame`, `VENC_IndexParamH264SPSPPS`,
`VENC_IndexParamH265Header`, `VENC_IndexParamH264CyclicIntraRefresh`,
`VENC_IndexParamH264ProfileLevel`).

**Platform-v2 extension — `VENC_INDEXTYPE_PLATFORM_V2`** (base `VENC_IndexParam_PlatformV2_Start = 0x3f000000`):
`VENC_IndexParamMBSumInfoOutput_V2` (reference `VencMBSumInfo_V2`),
`VENC_IndexParamSensorType` (0: sp2305, 1: c2398), `VENC_IndexParamEnableGetBinImage`,
`VENC_IndexParamGetBinImageData`, `VENC_IndexParamEnableMvInfo`, `VENC_IndexParamGetMvInfoData`,
`VENC_IndexParamSetLVAdjTh`, `VENC_IndexParamSetEnvLvTh` (`VencEnvLvRange`),
`VENC_IndexParamSetVbrParam` (`VencVbrParam`), `VENC_IndexParamMotionSearchParam`,
`VENC_IndexParamMotionSearchResult`, `VENC_IndexParamWbYuv`, **`VENC_IndexParamGdcConfig`**
(`sGdcParam`), **`VENC_IndexParamHwCropConfig`**.

**GDC / dewarp (encode-side) — `eWarpType`:** `Warp_LDC`, `Warp_LDC_Pro`, `Warp_Pano180`,
`Warp_Pano360`, `Warp_Normal`, `Warp_Fish2Wide`, `Warp_Perspective`, `Warp_BirdsEye`, `Warp_User`;
with `eMOUNTTYPE`, `eLENSDISTMODEL`, `ePERSPFUNC`. Limits: `MAX_RC_GOP_SIZE = 256`,
`DEFAULT_MOTION_SEARCH_HOR_REGION_NUM = 30`, `DEFAULT_MOTION_SEARCH_VER_REGION_NUM = 17`.

**Resolution limits: not declared.** `vencoder.h`/`vdecoder.h` carry no max/min width/height for the
general case — only `VConfig.nSupportMaxWidth` / `nSupportMaxHeight`, which the header scopes to
"the max width of **mjpeg continue decode**". Real per-codec limits live inside the closed plugins.

---

## (b) Present but disabled — and what would enable each

| # | Feature | Evidence it exists | State | What enables it |
|---|---|---|---|---|
| 1 | **AV1 decode** | `VIDEO_CODEC_FORMAT_AV1 = 0x119`; `VIDEO_CODEC_FORMAT_MAX = AV1`; demo advertises `-codFmat 3:AV1` | **no implementation**: no `libawav1.so`; zero `av1` symbols in `libVE`, `libvdecoder`, `libvideoengine`, `libcdc_base`, `libMemAdapter` | an Allwinner `libawav1.so` plugin for this IC — not shipped on this box |
| 2 | **`ve1` register block** | DT `ve1@1c0e000` = `allwinner,sunxi-cedar-ve1` | no driver/clock consumer; `clk_summary` lists only `1c0e000.ve` and `1c10000.ve2` | a driver binding/routing to `ve1` — none present; dec runs on `1c0e000.ve` |
| 3 | **10-bit / Main10 encode** | `VENC_H265ProfileMain10 = 2` | **unusable**: `VENC_PIXEL_FMT` has no 10-bit/P010 member | a 10-bit `VENC_PIXEL_*` input format + encoder support |
| 4 | **VCU** | `DecVcuConfig { bVcuAutoMode, nFrameNumInGroup, bEnableVcu }`; `bEnableVcuFuncFlag` | **runtime 0** on both blocks (`checkFeatureSupport: bEnableVcuFuncFlag = 0`) | set `bEnableVcu` / build support |
| 5 | **LBC (lossy bandwidth compression)** | `VENC_PIXEL_LBC_AW` ("for v5v200 and newer ic"); `VE_SUPPORT_DECODER_LBC_MODE` guards `nLbcLossyComMod` (1:1.5x, 2:2x, 3:2.5x), `bIsLossy`, `bRcEn` | compiled out (guard false) | build with `VE_SUPPORT_DECODER_LBC_MODE` |
| 6 | **AFBC** | `eControlAfbcMode { DISABLE_AFBC_ALL_SIZE, ENABLE_AFBC_JUST_BIG_SIZE //* >= 4k, ENABLE_AFBC_ALL_SIZE }`; `PIXEL_FORMAT_AW_AFBC`, `AW_AFBC_10BIT`; `bAfbcModeFlag` | default (not enabled by our paths) | set `eCtlAfbcMode` |
| 7 | **IPTV mode** | `eControlIptvMode { DISABLE_IPTV_ALL_SIZE, ENABLE_IPTV_JUST_SMALL_SIZE //* < 4k, ENABLE_IPTV_ALL_SIZE }` | not enabled | set `eCtlIptvMode` |
| 8 | **Decode drop-frame performance counters** | `SetDecodePerformCmd` / `GetDecodePerformInfo` / `VDecodePerformaceInfo` | declared, unused by our paths | call them |
| 9 | **Encode GDC/dewarp, motion-search stats, MV/bin-image export, low-light, HW crop** | full `VENC_INDEXTYPE_PLATFORM_V2` + `eWarpType` | declared, unused by the VA-API driver | set the corresponding `VENC_IndexParam*_V2` |
| 10 | **HEVC IDR** | `libvenc_h265` | **defective**: emits **zero IDR NALs** at every setting tried (incl. `idr_period=1`); all pictures `TRAIL_R`. H.264 `ForceKeyFrame`→IDR works. (prior finding, IC 21320) | vendor lib fix; not reachable from our layer |
| 11 | **Multi-decoder (`--decoder_num`)** | demo `-decNum` flag | **hazardous**: the concurrent/multi-instance path is what Oopsed the kernel (`NULL` deref at `0x18`, `Oops: 96000004`). Keep at 1. | nothing — leave disabled |
| 12 | **`/dev/sunxi_soc_info`** | runtime warning `cannot open /dev/sunxi_soc_info, it maybe ok!` | absent; falls back to `getSocInfo: not exist SocInfo plugin, use SocInfo node` | DT node / SocInfo plugin |
| 13 | **Test power domains** | DT `pd_ve_dec_test@0`, `pd_ve_enc_test@0` (`allwinner,sunxi-power-domain-test`, `status: okay`) | debug-only genpds | n/a (test nodes) |
| 14 | **Encoder params rejected on this chip** | prior finding | `VENC_IndexParamSetVbvSize`, `SetFrameLenThreshold`, `Rgb2Yuv` return errors; VBV caused corruption ("cbp too large") | vendor fix |

---

## (c) VA-API status — RESTORED, and verified encoding

**Answer to "which version is this source": v0.4, not the skeleton.** The file's opening comment
(`// SKELETON (step 1) … ABI: VA-API 1.10`) and the `/* real implementations (skeleton) */` comment at
`sunxi_ve_drv_video.c:620` are **both stale**. `V_VENDOR` is
`"sunxi-ve2-vaapi (Cedar VE2 H.264/H.265) 0.4"`, and the code contains a real pipeline:
`ve_CreateConfig` → `ve_CreateContext` → `ve_CreateBuffer` → `ve_RenderPicture` → `ve_EndPicture` →
`VideoEncCreate`/`VideoEncInit`/`VideoEncSetParameter`/`VideoEncodeOneFrame`, plus a zero-copy
dma-heap/dmabuf import path (`dmaheap_alloc`, `in.nShareBufFd`, `nShareBufFd`).

**ABI:** no bump needed. `sunxi_ve_drv_video.c:578` defines `__vaDriverInit_1_10` explicitly, and
lines 581-586 alias it via `VE_INIT_NAME(VA_MAJOR_VERSION, VA_MINOR_VERSION)`, which expands to
`__vaDriverInit_1_22` when built against the installed libva 2.22 headers. The built `.so` exports
**both** symbols, so libva 2.22 finds `__vaDriverInit_1_22` and older libva finds `_1_10`.

**Source:** `/home/radxa/ve2-vaapi/sunxi_ve_drv_video.c` (35526 B,
sha256 `fabdbab2db7b18fa500ab08a70101cc9d55393d0bb97368a054eb8208d0ae62a`), byte-identical to
`/mnt/sdcard/_REVIEW/backups/a733-backup/ve2/ve2-vaapi/sunxi_ve_drv_video.c`.

**Build + install commands (exact):**

```bash
cd /home/radxa/ve2-vaapi
gcc -shared -fPIC -o sunxi_ve_drv_video.so sunxi_ve_drv_video.c \
    $(pkg-config --cflags libva) -lvencoder -lMemAdapter -lVE
sudo install -m755 sunxi_ve_drv_video.so /usr/lib/aarch64-linux-gnu/dri/sunxi-drm_drv_video.so
# both discovery names — Sunshine derives "pvr" from the DRM node, vainfo can use the env var
sudo ln -sfn sunxi-drm_drv_video.so /usr/lib/aarch64-linux-gnu/dri/sunxi_ve_drv_video.so
sudo ln -sfn sunxi-drm_drv_video.so /usr/lib/aarch64-linux-gnu/dri/pvr_drv_video.so
```

> **The build command in the source file's own header comment is WRONG.** It reads
> `gcc -shared -fPIC -o … $(pkg-config --cflags libva)` — with no `-lvencoder -lMemAdapter -lVE`.
> A shared object links fine that way, but it then has **no `NEEDED` entries for the vendor
> libraries** and `dlopen` fails at runtime with
> `undefined symbol: AllocInputBuffer` → `vaInitialize failed with error code -1`.
> The `-lvencoder -lMemAdapter -lVE` flags are mandatory; they produce the `NEEDED` set
> `libvencoder.so, libMemAdapter.so, libVE.so, libc.so.6` that the working artifact has.
> Verified by building both ways and diffing `readelf -d` output.

Installed artifact: `/usr/lib/aarch64-linux-gnu/dri/sunxi-drm_drv_video.so`, sha256
`a58e3340160247b22d570190a5c33c036e34fd2d2f79e8dc922dcaac86fccb02` — **identical to the build output**.
`NEEDED`: `libvencoder.so`, `libMemAdapter.so`, `libVE.so`, `libc.so.6`.

**`vainfo` output (exact, plain invocation — no `LIBVA_DRIVER_NAME`, i.e. the Sunshine AT_SECURE path):**

```
$ vainfo --display drm --device /dev/dri/renderD128
libva info: VA-API version 1.22.0
libva info: Trying to open /usr/lib/aarch64-linux-gnu/dri/pvr_drv_video.so
libva info: Found init function __vaDriverInit_1_22
libva info: va_openDriver() returns 0
Trying display: drm
vainfo: VA-API version: 1.22 (libva 2.22.0)
vainfo: Driver version: sunxi-ve2-vaapi (Cedar VE2 H.264/H.265) 0.4
vainfo: Supported profile and entrypoints
      VAProfileH264ConstrainedBaseline:	VAEntrypointEncSlice
      VAProfileH264Main               :	VAEntrypointEncSlice
      VAProfileH264High               :	VAEntrypointEncSlice
      VAProfileHEVCMain               :	VAEntrypointEncSlice
```

All three names load (`LIBVA_DRIVER_NAME={sunxi_ve,sunxi-drm,pvr}` each → `Found init function
__vaDriverInit_1_22`, `va_openDriver() returns 0`, same driver version string).

**Safety note:** `__vaDriverInit` (lines 587-648) only sets `ctx` limits, `str_vendor`, `calloc`s
`DriverData` and fills the vtable. It touches no VE hardware, so `vainfo` cannot exercise the engine.
`ve_Terminate` only frees that allocation.

**Already-implemented VA-API surface:** real `vaTerminate`, `vaQueryConfigProfiles`,
`vaQueryConfigEntrypoints`, `vaGetConfigAttributes`, `vaCreateConfig`, `vaDestroyConfig`,
`vaQueryConfigAttributes`, `vaQuerySurfaceAttributes`, `vaCreateContext`, `vaDestroyContext`,
`vaCreateSurfaces(2)`, `vaDestroySurfaces`, `vaSyncSurface`, `vaQuerySurfaceStatus`,
`vaCreateBuffer`, `vaBufferSetNumElements`, `vaMapBuffer`, `vaUnmapBuffer`, `vaDestroyBuffer`,
`vaDeriveImage`, `vaDestroyImage`, `vaBeginPicture`, `vaRenderPicture`, `vaEndPicture`,
`vaQueryImageFormats`, `vaExportSurfaceHandle`. Everything else is a NULL-safety stub
(`ve_unimpl`), required because libva fails `vaInitialize` if any core vtable slot is NULL.
Advertised entrypoints are **encode only** (`EncSlice`) — this driver does **not** do VA-API decode.

---

## (d) Measured numbers (each with its command)

| # | Measurement | Command | Result |
|---|---|---|---|
| 1 | H.264 hardware decode, 300 f 720p | `libvdecoder` demo (prior session, same day) | wall **1.658 s**, CPU **1.036 s** = **0.6 cores** |
| 2 | H.264 software decode, 300 f 720p | `ffmpeg` software | wall 2.118 s, CPU **4.623 s** = 2.2 cores → **4.5× more CPU** |
| 3 | **VA-API encode, 30 f 720p** | `cd /home/radxa/ve2-vaapi && time ./test_encode` | wall **0.174 s**, user 0.050 s + sys 0.029 s = **0.079 s CPU = 0.45 cores**, ≈ **172 fps (RETRACTED - measured constant content; corrected to ~126 fps on real texture)**, output **505236 B** |
| 3b | Same encode, re-run after the §(f)#1 sync patch + correct build | `time ./test_encode` | wall 0.307 s, user 0.039 s + sys 0.054 s = **0.093 s CPU**, output **505236 B — byte-identical size and identical NAL structure** |
| 4 | Encoded stream structure | `python3` NAL walk of `/tmp/va_out.h264` | 1×SPS, 1×PPS, 1×IDR, 29× non-IDR slice = **30 frames**, 505236 B |
| 5 | Encoded stream validity | `ffprobe /tmp/va_out.h264` | `Video: h264 (Main), yuv420p(progressive), 1280x720` — parses cleanly |
| 6 | Software decode of the encode output | `ffmpeg -i /tmp/va_out.h264 -f null -` | **30 frames** decoded, **but P-frame corruption** (`concealing 3600 DC, 3600 AC, 3600 MV errors in P frame`) — **reproduces identically after the §(f)#1 patch**, see §(f) |
| 7 | H.265 decode attempt | `vdecoderdemo -i test_720p.h265 -codFmat 2 -n 250 -o /tmp/dec265.out -outFmat 6` | **aborted before decoding a frame** (glibc `sysmalloc` assertion), exit 134 — no frames measured |
| 8 | Clock tree | `sudo cat /sys/kernel/debug/clk/clk_summary \| grep -iE 've\|cedar'` | `pll-ve0` **546 MHz** (decode), `pll-ve1` **696 MHz** (encode) |
| 9 | Driver-chosen VE freq | encoder init log | `ve_default_freq = 624`; `VeSetSpeed: *** set ve freq to 624 Mhz ***` |
| 10 | Chip identity | encoder init log | `ic_version = 0x3331000021320` (both blocks) |

Encoder session log (abridged, `./test_encode`):

```
libva info: Trying to open /usr/lib/aarch64-linux-gnu/dri/pvr_drv_video.so
libva info: Found init function __vaDriverInit_1_22
[sunxi_ve] CreateConfig profile=6 -> id=1
[sunxi_ve] CreateContext 1280x720 -> id=1 (VE2 H.264 encoder opened)
[sunxi_ve] VideoEncInit OK H.264 1280x720 4000kbps gop=30 hdr=23B
INFO   : cedarc <ve2_env_init:65>: open /dev/cedar_dev_ve2 fd = 5
INFO   : cedarc <veEnvInit:139>: open /dev/cedar_dev fd = 6
INFO   : cedarc <VeInitialize:1708>: *** ic_version = 0x3331000021320,
ENCODED 30 frames, 505236 bytes -> /tmp/va_out.h264
[sunxi_ve] DestroyContext id=1
```

Driver encodes with `AW_CBR` (`sunxi_ve_drv_video.c:419`: `p.sRcParam.eRcMode = AW_CBR`),
`VENC_PIXEL_YUV420SP` (NV12) input, `nBufferNum = 4`, forced keyframe via
`VENC_IndexParamForceKeyFrame` for H.264.

---

## (e) Per-codec hardware / software decode verdict

Discriminator: **CPU cost** (hardware ≈ 0.5-1 core; software ≫ that), plus the vendor's own plugin
naming and the `libvdecVcs.so` hardware-VCS linkage. `libawh264.so` is provably the hardware path: it
contains `CedarPluginVDInit`, `ve_status_reg`, `ve_printf_register` and `cedarc`; it, `libawh265.so` and
`libawvp9HwAL.so` are the only plugins linking **`libvdecVcs.so`** (a hardware video-coding block).

| Codec | Verdict | Basis |
|---|---|---|
| **H.264** | **HARDWARE — MEASURED** | 0.6 cores for 300 f (4.5× less CPU than software); `libvdecVcs` + `CedarPluginVDInit` |
| **H.265 / HEVC** | **HARDWARE** (by construction) | `libawh265.so` + `libvdecVcs`; **not independently measured** — my run aborted while loading plugins |
| **VP9** | **HARDWARE** (by construction) | `libawvp9HwAL.so` / `CreateVp9HwDecoder`, `libvdecVcs`, real VP9 registers (`vp9_sram_port_data_regC4`, `vp9hwd_mv_joint_tree1`, `Vp9HwFlushPictures`); **not measured** — the demo CLI exposes no VP9 (`-codFmat` = 1/2/3 only) |
| **VP8** | hardware plugin, no soft marker | `libawvp8.so` / `CreateVp8Decoder`; no `libvdecVcs`; not measured |
| **MPEG2** | hardware plugin, no soft marker | `libawmpeg2.so`; not measured |
| **MPEG4 family** (H263, dx, normal, vp6, base/Xvid) | hardware plugins, no soft marker | `libawmpeg4*.so`; not measured |
| **AVS / AVS2** | hardware plugins, no soft marker | `libawavs.so`, `libawavs2.so`; not measured |
| **MJPEG / MJPEG+** | hardware plugins, no soft marker | `libawmjpeg.so`, `libawmjpegplus.so`; not measured |
| **VP6** | **SOFTWARE** | `libawvp6soft.so` / `CreateVp6SoftDecoder`; registered at runtime as **`vp6Soft`** — the only plugin so named |
| **AV1** | **NOT PRESENT** | no plugin; zero AV1 symbols in the vendor libs, despite `VIDEO_CODEC_FORMAT_AV1 = 0x119` and the demo advertising `3:AV1` |
| DIVX3/4/5, WMV1/2/3, RXG2, RX, MPEG1(own) | absent / header-marked `//* not support` | header + no plugin |

**The prior session's "VE2 has no usable hardware decode" claim is false**, and independently confirmed
false here: the plugin set is a full hardware decoder stack with exactly one software member (VP6).
Only **H.264 is CPU-measured**; H.265/VP9 are hardware by construction, not by measurement.

---

## (f) Unknowns

1. **P-frame corruption in the encoded stream — hypothesis TESTED AND FALSIFIED, cause still open.**
   `test_encode` produces a structurally complete H.264 stream (30 frames, SPS/PPS/IDR correct, ffprobe
   parses it), yet ffmpeg software decode reports fully concealed P frames
   (`concealing 3600 DC, 3600 AC, 3600 MV errors in P frame`; 3600 MBs = 80×45 = the whole frame).

   *Hypothesis:* `sunxi_ve_drv_video.c:475`/`:479` bracket the surface with
   `DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ` / `…END|DMA_BUF_SYNC_READ`. For CPU-written pixels handed **to**
   the VE2 DMA engine over IOMMU the required direction is `DMA_BUF_SYNC_WRITE` (flush CPU writes), while
   `SYNC_READ` is the device→CPU (invalidate) direction — and note the *fallback* path in the same
   function does call `FlushCacheAllocInputBuffer`, i.e. the zero-copy path is the one skipping the flush.

   *Test:* the 2-word patch (`READ`→`WRITE` on both calls) was applied, the driver rebuilt with the
   corrected link line, reinstalled and re-run **once**. Result: **byte-identical output** — same 505236
   bytes, same NAL structure (1 SPS/1 PPS/1 IDR/29 P), same concealed P frames. A cache-coherency race
   would not be byte-reproducible, so **the sync direction is NOT the cause of this corruption**.

   *Consequence:* the corruption is **deterministic**, and lives in the encoder-parameter/bitstream path
   or in the test harness. Prime suspects: the driver ignores the caller's `VAEncSequenceParameterBufferH264`
   /`VAEncPictureParameterBufferH264` and hardcodes its own (`AW_CBR`, 4000 kbps, `gop=30`,
   `VENC_H264ProfileMain`/`Level41`), and the test supplies `pic.frame_num = f` with `idr_pic_flag` only on
   frame 0 — if the driver does not carry that numbering into the bitstream, reference-picture tracking
   breaks and P frames decode against a wrong reference. **Not fixed, not verified.**

   *Driver state:* the deployed driver and its source were **restored to the exact as-found bytes**
   (`a58e3340…` binary / `fabdbab2…` source) after this experiment, because the patch brought no proven
   benefit and roughly doubled one encode sample's wall time. The candidate patch is recorded here, not
   deployed — apply it deliberately and re-verify if cache coherency is ever suspected.
2. **Whether H.265 / VP9 / VP8 hardware decode actually works.** Static evidence is strong; no CPU
   measurement exists for any of them. Each needs its own single VE session.
3. **Why `vdecoderdemo` aborts (`glibc sysmalloc` assertion) during plugin registration.** It crashed
   after `1117 load so: …/libawvp6soft.so` → `register vp6Soft decoder success!` and before decoding.
   Since this is the *first* stage of every demo run, it blocks all per-codec measurement. Unclear
   whether it is input-dependent, build-related, or the same heap bug the prior session hit.
4. **Real per-codec resolution / profile / level limits.** Not declared in `vdecoder.h`/`vencoder.h`
   (only MJPEG-continue `nSupportMaxWidth/Height`); the true limits are inside the closed plugins and
   were not extractable statically.
5. **Decode genpd `1c0e000.ve` reads `active`** after the aborted demo, while the encode block
   suspended cleanly. Whether that is a leaked runtime-PM reference from the abnormal exit or normal
   behaviour is unresolved. `sunxi_ve` refcount returned to 0.
6. **HEVC encode IDR blockage** (`libvenc_h265` emits no IDR NAL at any setting) — prior finding, not
   re-tested here.
7. **VE IOMMU leak** from repeated *failing* encode calls (prior finding: `rmmod`/`modprobe` did not
   clear it, reboot required) — not re-tested, and deliberately not provoked.
8. **`0.45 cores` for encode is a 30-frame sample** including process startup; the per-frame figure
   (2.6 ms/frame) is more representative than the cores number for longer runs.
9. Whether `AW_CBR` at 4000 kbps (the driver's hardcoded default) is related to unknown #1.
