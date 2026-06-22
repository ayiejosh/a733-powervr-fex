# GPU OpenGL via zink -> PowerVR Vulkan (off-screen only) — trixie / 6.6

On the trixie stack the working route to **GPU OpenGL** is **zink** (GL-over-Vulkan) on
the **system Mesa 25.0.7** driving the closed PowerVR Vulkan ICD. It is **off-screen
only** (EGL/surfaceless) — windowed and desktop GL do **not** work (see the wall at the
bottom). For windowed GPU graphics use **D3D via DXVK-Sarek** instead (`dxvk/`).

> This replaces the bullseye branch's "build Mesa 25.3 from source + patch zink" recipe:
> on trixie the **system Mesa 25.0.7 already has** the `zink` + `kmsro` + `renderonly`
> path. The conflict to manage is the *other* Mesa (see dual-Mesa note).

## The dual-Mesa conflict (read first)

Two Mesa installs coexist and are **version-incompatible**:
- `/usr/local` — **IMG Mesa 24.0.1** (`pvr_dri`, X11-only), drives the desktop, has
  ldconfig priority. Keep it; the desktop needs it.
- system `/usr/lib/aarch64-linux-gnu` — **Mesa 25.0.7**, has `zink` + `kmsro`.

Because `/usr/local` shadows the system Mesa, the zink path must be run under a **scoped
env** that flips `LD_LIBRARY_PATH` to the system libs. Don't remove `/usr/local` and don't
touch ldconfig — scope per app. The `glrun` launcher does exactly this.

## The recipe (off-screen GL)

`glrun <glapp>` runs an EGL/off-screen GL app on the GPU. The scoped env it sets:

```sh
LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu   # system Mesa, not /usr/local
LIBGL_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri
GALLIUM_DRIVER=zink
MESA_LOADER_DRIVER_OVERRIDE=zink
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json              # closed PowerVR Vulkan
# the feature-strip layer (makes zink accept the blob):
VK_LAYER_PATH=<layer-dir>
VK_INSTANCE_LAYERS=VK_LAYER_PVR_strip
PVR_FAKE_GS=1
# and: unset LIBGL_ALWAYS_SOFTWARE (the desktop sets it; it would force zink-CPU)
```

`GL_RENDERER` then reports `zink Vulkan 1.3 (PowerVR B-Series BXM-4-64 MC1)` — the real
GPU, not llvmpipe.

### Why the feature-strip layer is needed
zink **hard-requires** `geometryShader`, which the blob reports as false ->
`zink: Imagination proprietary driver w/o geometryShader is unsupported`. The implicit
layer reports `geometryShader=true` so zink proceeds, then **strips** it at
`CreateDevice` so the blob accepts the device. (Faking GS this way means GS-using GL would
crash the blob — rare for 2D/desktop GL.) The same layer is used by the DXVK path.

## What works

| bench | result |
|---|---|
| **glmark2-es2 `--off-screen`** | **661** (~662 fps; build 646 / texture 826 / shading 485) |
| GLES2 surfaceless render (green triangle -> FBO -> readback) | correct pixels, GPU-accelerated |
| eglinfo | EGL 1.5 Mesa, zink -> PowerVR backend confirmed |

Ceiling: **GL 2.1 / GLES 2.0** (hardware). Faked features are stripped at `CreateDevice`
(the blob rejects them), so the enabled-feature set caps GL at 2.1 — real gaps are
`fillModeNonSolid`, `descriptorIndexing`, `robustness2`. zink warns `PowerVR lacks
fillModeNonSolid` -> non-solid/wireframe fill is unreliable; solid-fill scenes render
correctly. The full glmark2 suite is functional but too slow to finish in 240s
(shader-compile bound).

## ❌ The wall: windowed / desktop GL does NOT work

- **GLX-windowed GL** (e.g. `glxgears`): the X server's GLX visual negotiation isn't wired
  to zink (`couldn't get an RGB Double-buffered visual`) — an X11/GLX issue, not a zink
  fault. Off-screen/EGL is the only viable path.
- **A GPU-composited desktop** (X11 or Wayland) is **HARD-BLOCKED at the kernel**: the
  off-screen kmsro bridge on `card0` is proven (a `SCANOUT|RENDER` gbm buffer + zink/
  PowerVR succeeds), but a **live compositor** driving the GPU to scan out HDMI
  **deadlocks the kernel** (`pvrsrvkm` mutex spin-on-owner in IRQ -> power-cycle). The
  full Wayland/zink desktop recipe is built and **inert** (autologin stays on software
  X11); it is not usable until `pvrsrvkm` is fixed. See `../kernel/` and
  `../docs/FINDINGS.md`.

**Bottom line:** use `glrun` for off-screen/EGL GL apps; use `dxvk/` (`d3drun`) for
windowed GPU graphics. Don't attempt a live GPU desktop — it hangs the board.
