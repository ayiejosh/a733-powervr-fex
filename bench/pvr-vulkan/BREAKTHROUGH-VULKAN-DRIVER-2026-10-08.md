# BREAKTHROUGH: the gap is the Vulkan driver - 22-25x with everything else held constant

## The controlled A/B

Same weston, same Xwayland, same client (glmark2-es2), same zink, same scene, same sizes.
**The only variable is the Vulkan driver underneath zink.**

| size | zink on **Mesa pvr** (open) | zink on **libVK_IMG** (vendor) | ratio |
|---|---|---|---|
| 640x480 | 45 FPS | **1016 FPS** | **22.6x** |
| 1920x1080 | 12 FPS | **297 FPS** | **24.8x** |

Vendor config: weston started with
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json`,
`VK_LAYER_PATH=/home/radxa/gpu-experiment`, `VK_INSTANCE_LAYERS=VK_LAYER_PVR_strip`,
`PVR_FAKE_GS=1`, `MESA_LOADER_DRIVER_OVERRIDE=zink`, driver `pvrsrvkm`.
Open config: driver `powervr`, Mesa's pvr ICD, `MESA_LOADER_DRIVER_OVERRIDE=zink`.

**The objective's "787 vs ~31" is this comparison, and the gap is ~23x with the compositor,
Xwayland, the client, zink and the scene all fixed.**

## Where it is not

The open driver's **raw** render is fine: `vkrender` ~300 Mpix/s offscreen, and the vendor path's
1016 FPS at 640x480 is 312 Mpix/s end-to-end. The open path's end-to-end is 45 x 0.307 =
**13.8 Mpix/s**. So the open stack renders fine in isolation and collapses ~22x once the frame goes
through present/compositing.

Same shape as the earlier direct finding (KMS 55.8 fps vs composited 13 fps at 1080p, same driver),
now with a vendor control at 297 fps through the identical composited path.

**So the defect is in the open stack's present/WSI path** - Mesa pvr's WSI, or the open kernel
module's dmabuf/sync handling - **not the renderer and not the compositor.**

## Corrections to earlier conclusions in this repo

1. **The vendor userspace exists.** `/usr/lib/libVK_IMG.so.24.2.6603887` (Vulkan 1.3.277),
   `/usr/lib/libGLESv2_PVR_MESA.so.24.2.6603887`, `/usr/share/vulkan/icd.d/img_icd.json`,
   `/usr/local/bin/glrun`. An earlier note here claimed it did not exist and that the 787 baseline was
   unreachable - **withdrawn**. The search missed `_PVR_MESA` and `libVK_IMG`.
2. **The GPU is 1 core** (BXM-4-64 MC1, from HW registers), so the driver's `core_count = 1` is
   correct and is not a 4x deficit.

## Why earlier rounds could not have found this

Every measurement from round ~46 onward - the compositor term, the explicit-sync release wait, the
WSI layout, the sampling rate, the vsync question, the ioctl counts, the timeline-sync migration -
was taken **inside the open stack**. A gap that exists *between the two Vulkan drivers* is invisible
to all of them: the open stack's own numbers look self-consistent (7 ms render, 7 ms compositor floor,
209 ioctls/frame), and none of them has a vendor control.

The lesson worth keeping: **when the target is "close the gap to X", measure against X.** The user's
own `Main/POWERVR-SITUATION-2026-09-22.md` documented the vendor userspace, the vendor Vulkan and
`glrun` all along.

## Next

Bisect inside the present path using the vendor path as control. First question: does the open path's
cost follow Mesa's WSI or the open kernel module? Mesa carries a `pvrsrvkm` winsys (`pvr_srv.c`), so
if it can be made to initialise against the vendor module, userspace and kernel can be separated.
