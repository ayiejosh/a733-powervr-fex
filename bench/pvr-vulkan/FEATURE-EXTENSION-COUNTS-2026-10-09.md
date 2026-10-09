# Feature and extension counts: vendor vs open vs CPU

Measured with a new `enumvk.c`, **each under its own driver**:

| | **vendor** `pvrsrvkm` | **open** Mesa pvr | CPU lavapipe |
|---|---|---|---|
| **device extensions** | **114** | **106** | 156 |
| **instance extensions** | 14 | **21** | 17 |
| **device features set** | **39** / 1760 bits | **27** / 1760 | 45 |
| API version | 1.3.277 | 1.3.363 | 1.4.305 |
| driver version | 6603887 | 109060195 | 1 |

## Named differences

**Open reports OFF what vendor reports ON:**
- `shaderInt64` — **0 open, 1 vendor**
- `dualSrcBlend` — **0 open, 1 vendor**
- `textureCompressionASTC_LDR` — **0 open, 1 vendor**

**Common to both:** `depthClamp`, `depthBiasClamp`, `wideLines`, `independentBlend`, `samplerAnisotropy`,
`textureCompressionETC2`.
**Absent in both:** `geometryShader`, `tessellationShader`, `fillModeNonSolid`, `multiViewport`,
`shaderFloat64`, `textureCompressionBC`.

*(`shaderFloat16` is a Vulkan 1.2 feature, so it isn't in the base bitset counted here — the vendor reports it
ON, per the earlier `vk16` finding.)*

## ⚠️ The per-extension diff is INVALID

**I generated the vendor's extension list while the OPEN driver was bound**, so the vendor ICD failed to
initialise (`vkEnumeratePhysicalDevices → -3`) and returned nothing. The diff read **"106 open-only, 0
shared"** — nonsense. **The counts are valid** (each taken under its own driver); **the set comparison is not**
and needs re-running with the vendor list captured while `pvrsrvkm` is bound.

**Same ICD-mismatch trap recorded earlier** — a hand-rolled script that doesn't set the ICD from the bound
driver produces confident wrong answers. **`harness.py` does; ad-hoc scripts don't.**

## What the counts say

**The open driver is not feature-starved in aggregate** — 106 device extensions vs 114, and *more* instance
extensions (21 vs 14). **The gap is specific features reported off** — `shaderInt64`, `dualSrcBlend`, **ASTC** —
**a compatibility item, not a performance one**, consistent with the earlier `shaderFloat16` difference.

## Kernel side (not yet measured)

**The user also asked about the kernel.** The two kernel modules expose different UAPIs: the mainline `powervr`
uses the DRM ioctl surface (`DRM_IOCTL_PVR_*`), while `pvrsrvkm` has its own driver-native interface including
`pvr_srv_sync` at 0 ioctls/op. **That comparison has not been enumerated and is the obvious follow-up.**
