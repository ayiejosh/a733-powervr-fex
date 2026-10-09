# DEFINITIVE: the vendor allocates an FBCDC heap (2 MiB) and the mainline driver does not

Answering "probe and dissect how the vendor achieves faster speed". The vendor userspace is a local
artifact (`/usr/lib/libVK_IMG.so.24.2.6603887`) and the **DDK source is on disk** (`/home/radxa/re/ti-ddk`).

## The vendor binary contains FBCDC controls

```
GetFBCSurfaceSize2D()
GetTwiddledMiptreePageCount: GetFBCSurfaceSize2D() failed
DisableFBCDC
DisableD32FBCDC
DisableSwapchainFBCDC
ForceFBCDCHeaderClearing
VK FBCDC scratch buffer
VK_EXT_image_compression_control
VkPhysicalDeviceImageCompressionControlFeaturesEXT
```

**FBCDC frame-buffer compression, including a `Swapchain`-specific variant, plus the image compression
control extension.**

## The DDK allocates the heap

```
services/server/devices/volcanic/rgxfwutils.c:2117
    sFBCDCStateTableBase.uiAddr      = RGX_FBCDC_HEAP_BASE;
    sFBCDCLargeStateTableBase.uiAddr = ...
    ui32TFBCCompressionControl       = ...
services/server/devices/volcanic/rgxinit.c:4269
    { RGX_FBCDC_HEAP_IDENT, RGX_FBCDC_HEAP_BASE, RGX_FBCDC_HEAP_SIZE, 0, 0, ...
include/volcanic/rgxheapconfig.h:223
    #define RGX_FBCDC_HEAP_BASE IMG_UINT64_C(0xEC00000000)
    #define RGX_FBCDC_HEAP_SIZE RGX_HEAP_SIZE_2MiB
```

**A 2 MiB FBCDC heap for compression state tables, handed to the firmware.**

## The gap, measured

| | mainline `powervr` | vendor `pvrsrvkm` |
|---|---|---|
| FBCDC feature declared | yes (`has_fbcdc`, `fbcdc_algorithm`, `fbcdc_architecture`) | yes |
| uses `fbcdc_algorithm` | **only in a feature check** (`pvr_device.c:783-787`) | yes |
| **FBCDC heap allocated** | **NO - zero references** | **yes - 2 MiB `RGX_FBCDC_HEAP`** |

Mesa's pvr driver likewise declares `has_fbcdc_algorithm = true`, `fbcdc_algorithm = 50` for `bxm-4-64`,
with no compression stream to write into.

## Why this is the answer

It explains **every** property measured this session:

* **Uniform fill collapses to ~0 PBE cost on the vendor** (0.04 ms; 98% reduction vs the open driver's
  35%) - uniform data compresses to nothing. Measured directly.
* **bytes/pixel flat** (r8 -> rgba16): compression is about *compressibility*, not width.
* **format-independent**, **load/store irrelevant** (compression is in the PBE), **layout-independent**.
* **The apparent "firmware boundary" is a missing 2 MiB heap.**

## Corrects objective target (4)

> "FBCDC render-target compression: deprioritised on evidence - the available source has no FBD
> structure or compression-stream allocation."

**Both halves are wrong.** The available source *does* reference the FBD structure (the DDK is on disk
and contains `RGX_FBCDC_HEAP`), and the missing piece is in the **mainline** source - which is exactly
where a fix would go. The deprioritisation rested on looking only at the open driver and concluding the
feature was absent from the platform, when the hardware has it and the vendor uses it.

## Next: a real fix, not a probe

Implement **FBCDC heap allocation in the mainline `powervr` module** following the DDK layout
(`RGX_FBCDC_HEAP_BASE`/`SIZE`, state-table bases, `ui32TFBCCompressionControl`), then have Mesa's pvr
driver emit compression streams for render targets. **First named, evidenced implementation target for
the dominant performance term.**
