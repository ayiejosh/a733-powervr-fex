# Direct A/B on one board: vendor ICD vs open ICD (2026-10-08)

Same board, same kernel, same weston, same Xwayland, same zink, same glmark2, same probes.
Vendor = `pvrsrvkm` + `img_icd.json` (client via `glrun`). Open = `powervr` + `libvulkan_powervr_mesa.so`.

## Raw render — `vkrender`, offscreen, no compositor

| size | open | vendor | ratio |
|---|---|---|---|
| 512² | 94.3 Mpix/s | **119.7** | 1.27x |
| 1024² | 208.0 | **395.4** | 1.90x |
| 2048² | 283.2 | **499.9** | 1.77x |
| 4096² | 309.7 | **574.9** | 1.86x |

**Raw render gap: ~1.8x** (not the 2.5-4x previously inferred by comparing vendor
`render-gap-vendor.txt` against a differently-shaped probe).

## Windowed GL through the identical weston + Xwayland + zink

| harness | open | vendor | ratio |
|---|---|---|---|
| `glmark2-es2 -s 800x600 -b build:use-vbo=false` | **38-40 FPS** | **1224 FPS** | **~32x** |

## What this proves

Raw render differs by **1.8x** but the windowed path differs by **32x**. So **~18x of the
windowed gap is not rendering at all** - it is the display loop: the client blocks 22.11 ms of a
27 ms frame in `AcquireNextImageKHR` waiting for the compositor's buffer release, and weston's
repaint cycle is 53.3 ms of which 36.96 ms is waiting for the client's commit.

**The open driver's windowed cost is therefore dominated by a round-trip that the vendor ICD does
not pay** - consistent with the vendor using its own WSI and driver-native sync
(`pvr_srv_sync_type`) rather than Mesa's `vk_drm_syncobj` + X11 Present release path.

## Cost of getting this data

The vendor driver rebooted the board during the windowed measurement (its 5th reboot here):
`sunxi_iommu_irq` WARNING at `sunxi-iommu-v2.c:405` and
`iommu_master de0_iommu: Runtime PM usage count underflow!` - the documented `pvrsrvkm`
PRIME/dmabuf bug. The numbers above were captured before the reset.
