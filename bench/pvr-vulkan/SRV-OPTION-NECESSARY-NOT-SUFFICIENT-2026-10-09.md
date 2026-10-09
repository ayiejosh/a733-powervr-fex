# `imagination-srv` compiles the srv winsys in — but the device still fails

## What was done

```
meson configure build -Dimagination-srv=true      # option imagination-srv = True
ninja -C build                                     # 77 targets, 0 errors
```

**Define verified:** `PVR_SUPPORT_SERVICES_DRIVER` appears **69×** in `compile_commands.json`.

## The result

**Open Mesa ICD + vendor kernel (`pvrsrvkm`, bound throughout — no driver switch):**

```
  instance extensions : 21
  instance layers     : 3
  physical devices    : 0
  FAIL: vkEnumeratePhysicalDevices(...) -> VkResult -3
```

**Still zero devices.** The hypothesis is **half-confirmed**:

| | |
|---|---|
| **Confirmed** | `imagination-srv` **is** the switch that compiles the vendor-kernel winsys in |
| **Falsified** | the build option **alone is not sufficient** — the device still doesn't come up |

## Where the failure must be

**Not the DRM-name selector** — that's inside `#if defined(PVR_SUPPORT_SERVICES_DRIVER)`, and the define is now present.

**The device-init path.** Evidence: the srv winsys has **no `dev_query` reference** (grep finds none in
`pvr_srv.c`), while the mainline winsys calls **`DRM_IOCTL_PVR_DEV_QUERY`** in three places. So the srv path
must get device info **through the vendor bridge** — and that flow doesn't complete.

**Next step:** trace `pvr_physical_device_init` under the srv winsys to find the failing call. **`MESA_DEBUG=1`
gave no extra output**, so the diagnostic must come from the code path itself — a temporary log at each
early-return — not from the environment.

## Status

| before | after |
|---|---|
| code absent, cause unknown | **code compiles in via one option** |
| failure assumed to be the selector | **selector ruled out; failure is device-init** |

**Still the cheapest thing to pursue: no driver switch, no GPU risk, and the code is already written.**
