# The 23x decomposes into render (4.4x) x present (5.4x)

`glmark2 --off-screen` removes the present/compositor path from the same client, so render and
present separate cleanly - with a vendor control to make it interpretable.

| config | 640x480 | 1920x1080 |
|---|---|---|
| vendor **off-screen** | 1070 | 313 |
| vendor **windowed** | 1016 | 297 |
| open **off-screen** | **245** | **107** |
| open **windowed** | 45 | 12 |

## Two independent defects that multiply

| term | 640x480 | 1920x1080 | meaning |
|---|---|---|---|
| vendor present cost | 1.05x | 1.05x | essentially free |
| **open render vs vendor render** | **4.4x** | **2.9x** | driver draw throughput |
| **open present vs open off-screen** | **5.4x** | **8.9x** | WSI/present path |

**4.4 x 5.4 = 23.8x = the measured windowed gap (22.6-24.8x).**

So the gap is not one thing. It is:
1. **Render ~3-4.4x down** - matches the long-standing "raw render 2.5-4x down" ground truth.
2. **Present 5.4-8.9x down** - an operation the vendor path performs for ~free.

This supersedes the single-term framing that attributed the whole 23x to present.

## Why earlier rounds missed it

Both halves are comparable in size, so every measurement inside the open stack looked
self-consistent (7 ms render, 7 ms compositor floor, 209 ioctls/frame) with no single dominant
cause. The split needed a vendor control: "open windowed is slower than open off-screen" is a fact
about the open stack; "open pays 5.4x for present while the vendor pays 1.05x" is the finding.

## Instruments

* `glmark2-es2 --off-screen -s WxH -b <scene>` - cheapest render/present split, same client.
* Vendor env: `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json`, `VK_LAYER_PATH=/home/radxa/gpu-experiment`,
  `VK_INSTANCE_LAYERS=VK_LAYER_PVR_strip`, `PVR_FAKE_GS=1`, `MESA_LOADER_DRIVER_OVERRIDE=zink`, driver `pvrsrvkm`.
* Open env: `VK_ICD_FILENAMES=/home/radxa/pvr_gen_icd.json`, driver `powervr`.

## Next

Attack the larger of the two per configuration. At 640x480 present (5.4x) is bigger; at 1080p present
(8.9x) dominates. Both are in the present/WSI path, which is where the earlier direct KMS-vs-composited
finding also pointed (KMS 55.8 fps vs composited 13 fps at 1080p with the open driver).
