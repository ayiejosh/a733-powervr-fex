# VK_LAYER_PVR_strip

Source for the "feature-strip layer" described in
[`../zink-trixie.md`](../zink-trixie.md) — an implicit Vulkan layer that
makes zink accept the closed PowerVR BXM-4-64 Vulkan driver, which is
missing two features zink hard-requires:

- `VkPhysicalDeviceFeatures.geometryShader` (blob reports `false`)
- `VK_EXT_robustness2` / `nullDescriptor` (blob doesn't know the extension
  at all)

It fakes both being present at query time (`vkGetPhysicalDeviceFeatures[2]`,
`vkEnumerateDeviceExtensionProperties`), then strips them back out of the
`VkDeviceCreateInfo` before forwarding to the real driver at
`vkCreateDevice`, so the blob is never actually asked to enable a feature
it doesn't have.

Faking real GS pipelines this way would crash the blob — this only fakes
the two feature bits zink's *initialization* checks for; GS-using GL
content isn't a target here.

## Build

```sh
gcc -shared -fPIC -fvisibility=hidden -O2 -o libVkLayer_PVR_strip.so vk_layer_pvr_strip.c
```

`VkLayer_PVR_strip.json`'s `library_path` is relative (`./libVkLayer_PVR_strip.so`),
so it resolves next to wherever you build the `.so` — point `VK_LAYER_PATH`
at this directory, same env block as `../zink-trixie.md`.

## Verified

Pixel-readback test (green triangle → FBO → readback: correct colors,
GPU-accelerated), same proof standard as this repo's own verification
method. Independently implemented and tested on an Orange Pi Zero 3W
(Armbian) — full writeup:
https://github.com/davidhfrankelcodes/pvr-a733-armbian
(`ARMBIAN-REPLICATION.md` §8).
