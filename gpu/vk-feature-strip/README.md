# VK_LAYER_PVR_strip

Source for the "feature-strip layer" described in
[`../zink-trixie.md`](../zink-trixie.md) — a Vulkan layer that makes zink accept the closed
PowerVR BXM-4-64 driver, which is missing two features zink wants:

- `VkPhysicalDeviceFeatures.geometryShader` (blob reports `false`)
- `VK_EXT_robustness2` / `nullDescriptor` (blob doesn't know the extension at all)

It fakes the bits being present at query time (`vkGetPhysicalDeviceFeatures[2]`,
`vkEnumerateDeviceExtensionProperties`), then strips them back out of the
`VkDeviceCreateInfo` before forwarding to the real driver at `vkCreateDevice`, so the blob is
never asked to enable a feature it doesn't have — and the app's own feature structs are
restored to whatever *it* asked for once the call returns.

## Two switches — and only one of them is safe

| env var | default | what it does | risk |
|---|---|---|---|
| `PVR_FAKE_GS=1` | off | fakes `geometryShader`; strips it at device creation | **safe only for apps that never draw quads.** Mesa does more than query this bit: zink generates a real GS pipeline whenever it lowers `GL_QUADS`, and the blob `abort()`s on any GS. This is what `glrun` in this repo sets — see **Known limits** for the failing set and the one-line check. |
| `PVR_FAKE_R2=1` | off | additionally advertises `VK_EXT_robustness2` and reports `nullDescriptor = VK_TRUE` | **crashes when used.** Satisfies the *screen-creation* check of newer zink, but the blob then segfaults inside `libVK_IMG.so` the first time Mesa actually binds a null descriptor. For getting a newer zink past initialisation only. |

The r2 path needs both: the layer enabled (`PVR_FAKE_GS=1`) **and** `PVR_FAKE_R2=1`.
`PVR_FAKE_R2` on its own changes nothing.

Measured on a Radxa Cubie A7A (Debian 13 trixie, kernel `6.6.98-5-aw2511`, DDK
24.2@6603887, Mesa 25.0.7), `glmark2-es2 --off-screen -b build:duration=2` through zink:

| configuration | result |
|---|---|
| no layer | no score — the blob rejects `geometryShader`, zink refuses to initialise |
| older reference layer (GS only) | **802** |
| this layer, `PVR_FAKE_GS=1` (default) | **806** / **835** |
| this layer, extension advertised but `nullDescriptor` left false | **804** |
| this layer, `PVR_FAKE_GS=1 PVR_FAKE_R2=1` | **SIGSEGV** — `#0 libVK_IMG.so`, `#2 libgallium-25.0.7.so`, no kernel fault |

That last row is why `PVR_FAKE_R2` is opt-in: advertising the *extension* is harmless,
reporting `nullDescriptor = VK_TRUE` is not. It is the feature bit, not the extension, that
makes zink take a path this driver cannot execute.

## Build

Works with both generations of Vulkan-Headers — the file maps the later `VK_KHR_robustness2`
spellings onto the `EXT` ones when the header doesn't define them (the trixie stack this repo
targets ships `VK_HEADER_VERSION 309`, which only has `EXT`):

```sh
gcc -shared -fPIC -fvisibility=hidden -O2 -o libVkLayer_PVR_strip.so vk_layer_pvr_strip.c
```

## Enable it

**Option 1 — per app (what `../zink-trixie.md` does, recommended).** `VK_LAYER_PATH`
registers the manifest as an *explicit* layer, which the loader never auto-enables, so the
layer must also be named in `VK_INSTANCE_LAYERS`:

```sh
VK_LAYER_PATH=<this dir> \
VK_INSTANCE_LAYERS=VK_LAYER_PVR_strip \
PVR_FAKE_GS=1 \
<your GL app>
```

> Pointing `VK_LAYER_PATH` at this directory **on its own does nothing** — `enable_environment`
> is only honoured for *implicit* manifests. Measured: manifest reachable only via
> `VK_LAYER_PATH`, `PVR_FAKE_GS=1` set → every query returns exactly what it does with no
> layer installed at all.

**Option 2 — implicit, enabled by the env var alone:**

```sh
./install.sh                     # builds, installs .so + manifest, checks for conflicts
PVR_FAKE_GS=1 <your GL app>      # no VK_LAYER_PATH / VK_INSTANCE_LAYERS needed
PVR_STRIP_DISABLE=1 <app>        # opt back out for one process
```

`install.sh` installs into `${XDG_DATA_HOME:-~/.local/share}/vulkan/implicit_layer.d/`, writes
the manifest with an absolute `library_path`, and **refuses to install if another manifest
already claims the name `VK_LAYER_PVR_strip`**.

### Three manifest rules that cost a debug session each (libvulkan1 1.4.309)

1. **An implicit manifest must have `disable_environment`.** Without it the loader skips the
   layer outright — no error at app level, just this line under `VK_LOADER_DEBUG=layer`:
   `Didn't find required layer object disable_environment in manifest JSON file, skipping this
   layer`. `enable_environment` alone is not enough. The manifest here therefore carries
   `disable_environment: {PVR_STRIP_DISABLE: 1}`.
2. **Do not list `VK_EXT_robustness2` under `device_extensions`.** The loader merges an
   implicit manifest's `device_extensions` into what apps see, *regardless of the runtime
   switch* — so with that field present, an app is told the extension exists even when
   `PVR_FAKE_R2` is unset, and then fails `vkCreateDevice` with
   `VK_ERROR_FEATURE_NOT_PRESENT` (measured: 115 device extensions vs 114). The layer
   advertises the extension itself, from `vkEnumerateDeviceExtensionProperties`, only when
   `PVR_FAKE_R2=1`.

3. **`description` must be at most 254 bytes.** At 255 and above the loader discards the whole
   manifest: it does not appear in the layer list at all, and requesting it by name fails with
   `Layer "VK_LAYER_PVR_strip" was not found but was requested by env var VK_INSTANCE_LAYERS!`
   — with no parse error anywhere. `VK_MAX_DESCRIPTION_SIZE` is 256, so the field looks
   256-safe while 255 is already fatal; the limit is not the constant. Measured with the layer
   built from this tree, only the description length varying, five runs each: 253 and 254
   inserted, 255/256/277 discarded. @davidhfrankelcodes hit the same failure on libvulkan1
   1.4.309 and bisected it field-by-field against a working manifest; the boundary above was
   measured here on 1.3.275, so stay under 254 on both. `VkLayer_PVR_strip.json` is now the
   single source for that string — `install.sh` derives the installed manifest from it — so
   the two copies cannot drift apart again.

## ⚠️ One name, one layer

`VK_LAYER_PVR_strip` is deliberately the name this repo's docs already reference, so no env
block has to change. The consequence: it **cannot coexist with an older build of the same
layer**, and at least one is in the wild (the reference implementation this repo used before,
which gates on `PVR_STRIP_ENABLE`/`PVR_STRIP_DISABLE` rather than `PVR_FAKE_GS`). When two
manifests claim one name, the loader picks one and silently ignores the other — including its
env vars, so the layer appears to do nothing. Install this *instead of* the old one.

## Verified / not verified

Verified on the hardware above (table + `vk_layer_probe.c` in the PR discussion): all four
query paths, the strip at device creation, restoration of the app's own structs, inertness
with no env var set, and the `PVR_FAKE_R2` crash. The `nullDescriptor` fake does get a zink
that requires it past screen creation — but on this blob it then crashes on first use, so it
is not yet a working end-to-end path for Mesa ≥ 26.

Not verified here: the original pixel-readback result on the contributor's Orange Pi Zero 3W
(the independent implementation is theirs; this repo's re-test was the probe + glmark2 above).

The GS crash is now measured rather than merely documented — see **Known limits**. It is a
user-space `SIGABRT`, not a driver hang: `pvrsrvkm` stays loaded, `dmesg` stays clean and no
power cycle is needed, so the earlier caution about re-testing it was unnecessary.

## Known limits

- Single instance / single device (one static `g_inst`/`g_dev`, no dispatch-table map keyed by
  handle) — fine for `eglinfo`, `glmark2-es2`, single-instance apps; not general-purpose
  correct for multi-instance applications.
- **FIXED in `vk_layer_pvr_strip.c` — see the note at the end of this list.** With
  `PVR_FAKE_GS=1`, zink built its own GS pipeline and the blob aborted. The fake is
  not a query-only lie. zink advertises `MESA_PRIM_QUADS` only when it sees `geometryShader`,
  and lowers `GL_QUADS` with a self-generated GS (NIR dump name `filled quad gs`); the blob has
  no GS pipeline support and its shader compiler calls `abort()` instead of returning an error.
  So the trigger is *not* GL content that uses geometry shaders — fixed-function 1.x/2.x quad
  demos are enough: `glxgears`, `eglgears_x11`, `glxdemo` and `peglgears` all aborted in
  issue #6.
  Check any app before shipping it:
  `ZINK_DEBUG=nir <app> 2>&1 | grep -c MESA_SHADER_GEOMETRY` — `0` means it runs, non-zero means
  zink compiled a GS for that program and it aborts here. There is no GS-free fallback:
  `PVR_STRIP_DISABLE=1` does not make those apps run, it makes zink refuse to initialise
  (`zink: Imagination proprietary driver w/o geometryShader is unsupported`). The safe set is
  apps for which zink never generates a GS — `glmark2`, `glmark2-es2`, `glxheads`,
  `es2gears_x11`.
- That abort is a plain user-space `SIGABRT` (exit 134) on the driver thread `gdrv0`, inside
  `BILParseStream()` in `libufwriter.so` called from `libVK_IMG.so`. The kernel stays up,
  `pvrsrvkm` stays loaded and `dmesg` stays clean — no power cycle needed. Measured on an
  Orange Pi Zero 3W (issue #6); reproduced here on the Radxa Cubie A7A with `peglgears`
  (exit 134, `MESA_SHADER_GEOMETRY` = 1, same `BILParseStream()` frame), so it is not
  board-specific. `glxgears` and `glxdemo` cannot obtain a GLX visual on this board at all,
  so they never reach zink here either way.
- **Root cause and fix.** zink computes its screen caps *after* `vkCreateDevice`, and this
  layer used to restore `geometryShader = true` into the caller's struct once the call
  returned. zink therefore re-read the feature as present, advertised `MESA_PRIM_QUADS`, took
  quad draws, generated the GS, and the blob aborted. The restore is gone: the device really is
  created without `geometryShader`, so reporting it back as enabled was a lie. With the stock,
  unpatched system zink this turns the abort into 182,040 frames in 5 s (36,407 FPS) with zero
  GS, and zink still initialises. `mesa/zink-quads-without-gs.patch` fixes the same failure
  independently by removing zink's need for a GS at all.
- `primtest.c` is the check for the above: it draws one primitive type through whatever GL
  driver is configured and reports whether it survived. One primitive per process, because an
  abort takes the whole process with it.
  `gcc -O2 -o primtest primtest.c -lEGL -lX11 -lGL`, then
  `primtest quads|quad_strip|polygon|line_loop|triangles`. Measured: quads and quad strips
  aborted with the pre-fix layer (exit 134) and pass with the patched zink; polygons and line
  loops passed either way.
- `primtest wireframe` probes a different capability. The blob reports
  `fillModeNonSolid = false`, and `glPolygonMode(GL_LINE)` has **no effect** - measured with
  pixel readback: filled 1682 pixels, lined 1682, identical. Crucially the same is true with
  `PVR_FAKE_FILL=1`, which reports the feature as present: faking it does not create the
  capability, it only makes zink stop warning and set `polygonMode = LINE` on a device that was
  created without the feature. So the fake hides a real limitation - an app asking for wireframe
  silently gets filled polygons.
  `mesa/zink-quads-without-gs.patch` fixes this too: zink expands a wireframe draw into an
  indexed line list itself, and polygon point mode into a point list. `primtest wireframe`
  covers vertex arrays and index buffers for triangles, strips and fans, plus multi-draw,
  primitive restart and point mode - nine cases, all ignored before and all working after
  (triangles 1682 filled vs 172 lined). `primtest tf` reports that transform feedback is not
  available on this stack at all, so the combination cannot be exercised here.
