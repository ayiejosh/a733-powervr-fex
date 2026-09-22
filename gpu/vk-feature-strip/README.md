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
| `PVR_FAKE_GS=1` | off | fakes `geometryShader`; strips it at device creation | **safe.** Mesa only *queries* this bit; nothing turns on real GS pipelines. This is what `glrun` in this repo sets. |
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

### Two manifest rules that cost a debug session each (libvulkan1 1.4.309)

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
Faking real GS pipelines crashing the blob is documented by this repo and was **not** re-tested
(a driver hang on a headless board means a power cycle).

## Known limits

- Single instance / single device (one static `g_inst`/`g_dev`, no dispatch-table map keyed by
  handle) — fine for `eglinfo`, `glmark2-es2`, single-instance apps; not general-purpose
  correct for multi-instance applications.
- With `PVR_FAKE_GS=1`, GL content that actually uses geometry shaders will crash the blob
  (rare for 2D/desktop GL; the fake is a query-only lie).
