# Kernel-vs-userspace bisect: Mesa's vendor path targets a different ABI, and its winsys was bit-rotted

The open stack pairs **Mesa userspace + mainline `powervr`**; the vendor pairs **vendor userspace +
`pvrsrvkm`**. The clean bisect is Mesa userspace on the vendor kernel, and Mesa *has* a `pvrsrvkm`
winsys selected by DRM driver name at `pvr_winsys.c:98-105`.

## 1. The winsys does not compile (bit-rotted)

With `-Dimagination-srv=true`:

```
pvr_macros.h:58: error: conflicting types for 'pvr_rogue_srv_winsys_render_submit';
  have 'VkResult(..., const struct vk_sync_signal *, ...)'
  previous declaration ... 'struct vk_sync *, struct vk_sync *'
```

The submit API moved to `struct vk_sync_signal *`; this winsys was never updated. **Ported it** (4
call sites, 3 headers, `pvr_srv.c` forward declarations) - it now builds clean. Committed locally.

## 2. The runtime path cannot work: different ABI

`pvr_is_driver_compatible()` requires name `pvrsrvkm` and version **1.17**
(`pvr_srv_bridge.h:140`). The installed module reports, via `drmGetVersion()` on
`/dev/dri/renderD128`:

```
name = 'pvr'   version = 24.2.6603887
```

**Both differ.** With the winsys compiled in, `vkEnumeratePhysicalDevices` still returns
`VK_ERROR_INITIALIZATION_FAILED (-3)`.

**So the bisect is not available by this route** - it would need a 24.2 bridge for
`pvr_srv_bridge.h`, which is a project rather than a probe and would only answer the question, not
improve the open path.

## What it establishes

* Mesa carries **dead, unmaintained** code for a vendor ABI it no longer matches; **the `pvrsrvkm`
  winsys does not build on `main` today** - worth reporting upstream independently.
* The kernel/userspace split cannot be made with the installed blobs, so the per-pixel 2.45x must be
  pursued on the open path itself.
* The open path's kernel driver reports DRM name `powervr` and is selected by the other winsys; both
  paths exist in Mesa but only the mainline one is live.

## Next

Comparative **CSB dump** of the same `vkrender` draw under both drivers - the fixed-function state
Mesa programs (ISP/PBE/tile setup in `pvr_arch_job_render.c` / `pvr_arch_cmd_buffer.c`) and how the
mainline kernel translates it into firmware commands.
