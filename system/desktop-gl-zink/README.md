# Desktop GL on the GPU (zink -> PowerVR), replacing the forced software fallback

**Applied 2026-09-22.** `10-software-render.sh` is the live copy of
`~/.config/plasma-workspace/env/10-software-render.sh`; it runs at Plasma X11 session start.

## Why

The vendor DDK is GLES-only (`EGL_CLIENT_APIS: OpenGL_ES`) and the vendor Xorg exports no GLX, but
Qt's X11 EGL integration asks for `EGL_OPENGL_BIT` (desktop GL). On the vendor EGL that request
matches **zero** configs, so every Qt/KWin GL path died with *"Cannot find EGLConfig, returning null
config"*. System Mesa + zink answers the same request with **45** configs, backed by the PowerVR
through the IMG Vulkan ICD — so desktop GL works, on the GPU, without touching the driver.

The file used to force `LIBGL_ALWAYS_SOFTWARE=1` + `QT_QUICK_BACKEND=software`, a defence against the
historical kernel deadlock. That defence is no longer warranted: the X11 client path is stable
(40 s at 1080p60, 0 swap errors) and the compositor failure is a userspace issue, not a hang.

## What it does

| variable | why |
|---|---|
| `LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu` | system Mesa must win over the vendor stack in `/usr/local/lib` (ld.so.conf priority) |
| `LIBGL_DRIVERS_PATH` + `MESA_LOADER_DRIVER_OVERRIDE=zink` + `GALLIUM_DRIVER=zink` | route GL through zink |
| `VK_ICD_FILENAMES` + `VK_LAYER_PATH` + `VK_INSTANCE_LAYERS` | the IMG Vulkan ICD, plus our feature-strip layer |
| `PVR_FAKE_GS=1 PVR_FAKE_FILL=1` | zink refuses an IMG device without `geometryShader`; the layer fakes both for the capability check and strips them at device create |
| `LIBGL_KOPPER_DRI2=1` | as Radxa's own `task-a733-powervr` sets |
| `QT_XCB_GL_INTEGRATION=xcb_egl` | there is no GLX here; skip Qt's GLX attempts |
| `QT_QUICK_BACKEND=opengl` | the Plasma shell itself renders on the GPU |

## Measured (same window, same shader)

| workload | GPU via zink | llvmpipe | |
|---|---|---|---|
| 320x240, loop=16 | **345.5 fps** | 111.4 fps | 3.1x |
| 800x600, loop=64 | **33.5 fps** | <2 fps (120 frames did not finish in 60 s) | >17x |

The small-workload ratio is modest because the GPU path pays a ~2.9 ms per-frame sync floor; the
per-pixel advantage is where it shows.

## Revert

```sh
cp ~/.config/plasma-workspace/env/10-software-render.sh.bak-forced-software-20260922 \
   ~/.config/plasma-workspace/env/10-software-render.sh
systemctl --user unset-environment LD_LIBRARY_PATH LIBGL_DRIVERS_PATH MESA_LOADER_DRIVER_OVERRIDE \
  GALLIUM_DRIVER VK_ICD_FILENAMES VK_LAYER_PATH VK_INSTANCE_LAYERS PVR_FAKE_GS PVR_FAKE_FILL \
  LIBGL_KOPPER_DRI2 QT_XCB_GL_INTEGRATION QT_QUICK_BACKEND
systemctl --user set-environment LIBGL_ALWAYS_SOFTWARE=1 QT_QUICK_BACKEND=software   # or just log out/in
systemctl --user restart plasma-plasmashell.service plasma-kwin_x11.service
```

## Known trade-off

`PVR_FAKE_GS=1` is session-wide, so every Vulkan app is told the device supports geometry shaders;
the BXM blob rejects GS *pipelines*, so an app that trusts the flag and uses GS will fail. That is
the price of zink (it refuses an IMG device without it). To narrow it: drop `VK_INSTANCE_LAYERS` and
the `PVR_FAKE_*` from this script and set them per-application (as `glrun` does).

Not fixed by any of this: **KWin's own compositor** still cannot start (Qt's X11 EGL/kopper
integration fails to initialize even with zink: `failed to create dri2 screen`). The desktop keeps
compositing with `picom --backend xrender` on the CPU; its clients and its shell do not.
See `../../docs/GPU-RESEARCH-2026-09-22.md` §8.
