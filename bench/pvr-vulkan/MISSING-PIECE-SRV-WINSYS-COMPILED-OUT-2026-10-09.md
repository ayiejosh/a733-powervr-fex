# FOUND: the open driver has a complete pvrsrvkm winsys, compiled out behind `PVR_SUPPORT_SERVICES_DRIVER`

## What the vendor has that the open driver lacks

**Mesa already contains a full winsys for the vendor kernel driver** —
`src/imagination/vulkan/winsys/pvrsrvkm/`, **7555 lines**, wired into `meson.build:117-122`:

- **`pvr_srv_sync.h`** — `extern const struct vk_sync_type pvr_srv_sync_type;` — **the driver-native sync**,
  i.e. the exact mechanism the vendor uses at **0 ioctls/op** where the open path costs 1.
  **This is the 60% lever's mechanism, already written.**
- `pvr_srv_job_{render,compute,transfer,null}.c` — the job paths
- `pvr_srv_bo.c`, `pvr_srv_bridge.c` — 144 `pvr_srv_` calls into the vendor bridge

## Why it doesn't engage

`pvr_instance.c` selects the winsys by DRM driver name:

```c
   is_pvr = !strcmp(version->name, PVR_DRM_DRIVER_NAME);
#if defined(PVR_SUPPORT_SERVICES_DRIVER)
   is_pvr |= !strcmp(version->name, PVR_SRV_DRIVER_NAME);
#endif
```

**The vendor kernel presents `name=pvr  version=24.2.6603887`** (read via raw `DRM_IOCTL_VERSION`), and
**`PVR_SUPPORT_SERVICES_DRIVER` is not defined in this build** — so the services path is compiled out.

**Measured — open Mesa ICD against the vendor kernel:**

```
  instance extensions : 21
  physical devices    : 0
  FAIL: vkEnumeratePhysicalDevices(...) -> VkResult -3
```

**The driver loads, finds the render node, reports zero devices.** With the mainline `powervr` kernel bound, the
same ICD reports one device and passes every probe — so the failure is **specific to the vendor-kernel path**.

## What this means

**The largest remaining lever may not need to be written at all — it may need to be ENABLED.** The
driver-native sync, the srv job paths and the vendor bridge all exist behind **one define.**

**Next step: build with `PVR_SUPPORT_SERVICES_DRIVER` defined, then test the open Mesa ICD against `pvrsrvkm` —
no driver switch needed, the desktop already runs on it.** Gate: probes → weston → client.

**And this is a direct answer to "what is missing": not a feature, not an extension, not a flag — a build
configuration.**
