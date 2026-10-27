# The vendor GL path — native GL at vendor speed, without zink

```
glmark2-es2 800x600 windowed     score
  vendor native GL (this)          549
  zink -> vendor Vulkan            293
  zink -> open Mesa Vulkan         210   <- the open driver's GL
```

This is **Imagination's own GL stack driven through a Mesa frontend**. GL work is executed by the
vendor's `libGLESv2_PVR_MESA.so`, so there is no GL->Vulkan translation in the path. It is the fastest
GL on this board and it is **not** a step toward native GL on the open driver - see the last section.

## What it is, and what it is not

```
app -> Mesa libEGL/libGLESv2 (Mesa 22.1.7)          open source
     -> pvr_dri.so (Imagination PVR Gallium frontend) open source
       -> dlopen("libpvr_dri_support.so")            VENDOR BINARY
         -> libGLESv2_PVR_MESA.so 24.2@6603887       VENDOR BINARY - the actual GL implementation
           -> libsrv_um.so -> pvrsrvkm                VENDOR kernel driver
```

Everything below the frontend is the vendor's closed binary. **It requires `pvrsrvkm` bound**; it cannot
run on the open `powervr` module.

## How it was achieved

Source: [`cwt-vf2/mesa-pvr-vf2`](https://github.com/cwt-vf2/mesa-pvr-vf2) — 62 patches adding the PVR
frontend, derived from Imagination's upstreamable DRI work. All 62 apply cleanly to **Mesa 22.1.7**.

Three things had to be solved, and they are why this directory carries a full delta rather than a patch
against the vendor series:

1. **Mesa 26.3 was the wrong target.** The frontend hooks `__DriverAPIRec`, `.InitScreen` and
   `driver_api`; 26.3 has none of those - the DRI mechanism was restructured. So it builds against
   **22.1.7** instead. (The porting surface looks like 137 lines until you discover the API is gone.)
2. **Bundled zlib 1.2.11 vs gcc 14.** `gzlib.c`/`gzread.c`/`gzwrite.c` omit `<unistd.h>` and gcc 14
   makes implicit declarations errors. `apt` is broken on this board (unmet `libc6-dev`), so the
   subproject cannot be swapped for the system zlib - the three files are patched instead.
3. **Driver name resolution.** The loader's device->driver mapping never fires for this platform
   device, and Mesa's EGL then retries with software rendering, leaving `eglInitialize` at `0x3001`.
   Fixed by pointing the EGL platform fallbacks at `pvr` (`PVR_DRI_DRIVER_NAME`) **and** exporting
   `MESA_LOADER_DRIVER_OVERRIDE=pvr` in `gl-wrapper-env.sh`.

All three are inside the patch, except the `pvr` naming env which is in `gl-wrapper-env.sh`.

## One ordering trap in the build

The patch adds `pvr` to `meson_options.txt`, so it **must** apply before `meson setup`. But the bundled
zlib subproject does not exist until `meson setup` downloads it, so a plain `git apply` fails on those
three files. `build-vendor-gl.sh` therefore applies the patch with `--exclude='subprojects/zlib-*'` and
patches zlib afterwards. That exclusion is deliberate, not an oversight.

## Rebuild and verify

```sh
./build-vendor-gl.sh                      # clones, patches, builds, installs to ~/mesa-pvr/install
. ./gl-wrapper-env.sh
DISPLAY=:0 glmark2-es2 --size 800x600
```

**Verification that it is the vendor path and not zink**: `GL_RENDERER` must read
`PowerVR B-Series BXM-4-64` and `GL_VERSION` must read `OpenGL ES 3.2 build 24.2@6603887`. If it says
`zink Vulkan ...`, the environment did not take - most often the session-wide
`MESA_LOADER_DRIVER_OVERRIDE=zink` (from `system/desktop-gl-zink`) winning over this one.

Rendering was also verified beyond the score: a 60-line EGL/GLES2 client cleared a pbuffer and read the
pixel back as exactly `0 255 0 255`.

## Why this is not native GL for the open driver

The open driver ships **Vulkan only** (`libvulkan_powervr_mesa.so`). This path's GL implementation is the
vendor's binary GLES, which needs `libsrv_um.so` and `pvrsrvkm`. Pointing the same frontend at the open
driver would mean supplying a GL implementation to call - and anything built on the open Vulkan driver
*is* zink. The frontend proves Mesa's overhead is small (549 vs 549 native); the missing piece for open
native GL is the draw path in `src/gallium/drivers/pvr_native`, not this.
