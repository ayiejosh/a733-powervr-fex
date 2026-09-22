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

> **Both `VK_LAYER_PATH` and `VK_INSTANCE_LAYERS` are load-bearing.** `VK_LAYER_PATH` only
> makes the manifest *discoverable*; `enable_environment` is honoured for **implicit**
> manifests only, so an explicit layer that is merely on the search path is never enabled.
> Measured: `PVR_FAKE_GS=1` + `VK_LAYER_PATH` set, no `VK_INSTANCE_LAYERS` → every query
> returns exactly what it does with no layer installed at all.

### Why the feature-strip layer is needed
zink **hard-requires** `geometryShader`, which the blob reports as false ->
`zink: Imagination proprietary driver w/o geometryShader is unsupported`. The layer reports
`geometryShader=true` so zink proceeds, then **strips** it at `CreateDevice` so the blob
accepts the device. (Faking GS this way means GS-using GL would crash the blob — rare for
2D/desktop GL.) The same layer is used by the DXVK path.

Source: [`vk-feature-strip/`](vk-feature-strip/) (published 2026-09-22; it was described here
for months without the source being in the repo).

### Two switches — only one of them is for this page

| env var | effect | use it? |
|---|---|---|
| `PVR_FAKE_GS=1` | reports `geometryShader=true`, strips it at `CreateDevice` | **yes — this is the one the recipe above sets.** Safe: Mesa only *queries* the bit, and nothing enables real GS pipelines. |
| `PVR_FAKE_R2=1` | additionally advertises `VK_EXT_robustness2` and reports `nullDescriptor=true` | **no, not for GL work.** It gets a newer zink past its *screen-creation* check, but the blob then segfaults inside `libVK_IMG.so` the first time Mesa binds a null descriptor (measured: `#0 libVK_IMG.so`, `#2 libgallium-25.0.7.so`, no kernel fault). |

> ⚠️ **One name, one layer.** `VK_LAYER_PVR_strip` is also the name older builds of this layer
> use — including the reference implementation this page pointed at before the source was
> published, which gates on `PVR_STRIP_ENABLE`/`PVR_STRIP_DISABLE` instead of `PVR_FAKE_GS`.
> When two manifests claim one name the loader picks one and **silently ignores the other's
> env vars**, so the layer looks like it does nothing. Remove the other manifest (or scope
> with `VK_LAYER_PATH` + `XDG_DATA_HOME`), and check what actually loaded with
> `VK_LOADER_DEBUG=layer`.

### ❌ Not a path to newer zink: Mesa ≥ 26

zink builds from Mesa 26 refuse to initialise without `nullDescriptor`
(`Zink requires the nullDescriptor feature of KHR/EXT robustness2`), and this blob has no
`VK_EXT_robustness2` at all. `PVR_FAKE_R2=1` gets past that check — then crashes on first use,
for the reason in the table above. And it is not only the feature bit: Mesa's `db` descriptor
mode wants `VK_EXT_descriptor_buffer`, resizable BAR and `VK_EXT_non_seamless_cube_map`, and
the blob exposes **none** of them (**114 device extensions; `descriptor_buffer` absent** —
enumerated 2026-09-22). Newer zink on this GPU is blocked by **driver capability**, not by a
missing lie in a layer; a newer `pvrsrvkm`/DDK is the only lever. Don't spend an evening on it.

## What works

| bench | result |
|---|---|
| **glmark2-es2 `--off-screen`**, full suite | zink **581**, vendor GLES **826** — at the 1104 MHz GPU clock (`../overlays/`). The same pair read **454** / **659** at the stock 600 MHz default. |
| GLES2 surfaceless render (green triangle -> FBO -> readback) | correct pixels, GPU-accelerated |
| eglinfo | EGL 1.5 Mesa, zink -> PowerVR backend confirmed |

> The **661** this table used to list was measured in June 2026 at the stock 600 MHz GPU clock
> and an older userspace state — it is not comparable with the rows above, and it is not the
> current ceiling of this path.

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
