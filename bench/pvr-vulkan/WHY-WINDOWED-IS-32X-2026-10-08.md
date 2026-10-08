# Why the windowed path is ~32x slower: Xwayland copies every frame

## The chain, each link measured

```
windowed gap ~32x  (open 38-40 FPS vs vendor 1224 FPS, same weston+Xwayland+zink+glmark2)
├── raw render 1.8x        (vkrender offscreen, 283 vs 500 Mpix/s at 2048^2)
└── windowed path ~18x
    └── Xwayland COPIES every presented frame
        └── the client's buffer is not flippable
            └── the driver exports DRM_FORMAT_MOD_LINEAR only
                └── PowerVR's only non-linear layout is FBCDC
                    (DRM_FORMAT_MOD_PVR_FBCDC_8x8_V*), whose allocation is closed userspace
```

## 1. The wait is real and it is the frame

```
[rel] explicit-sync release waits=300  avg=22.11 ms  max=62.18 ms  images=3
```
22.11 ms of a 27 ms frame is the client blocked in `AcquireNextImageKHR` ->
`wsi_drm_wait_for_explicit_sync_release` -> `timeline_wait(..., WAIT_AVAILABLE)`, waiting for
the compositor to release the buffer.

## 2. The wait scales with AREA, which a flip would not

| size | Mpix | fps | release wait |
|---|---|---|---|
| 800x600 | 0.48 | 42 | 19.06 ms |
| 1920x1080 | 2.07 | 13 | **68.43 ms** |
| 3840x2160 | 8.29 | 4 | (>50 waits, frames far apart) |

## 3. And so does Xwayland's own CPU per frame

| size | Mpix | fps | Xwayland CPU/frame | % of a core | stime |
|---|---|---|---|---|---|
| 800x600 | 0.48 | 42 | 16.63 ms | 69.9% | 46.4% |
| 1920x1080 | 2.07 | 13 | 44.30 ms | 57.6% | 47.6% |
| 3840x2160 | 8.29 | 4 | **115.97 ms** | 46.4% | 42.7% |

17.3x the pixels -> 7.0x the CPU, ~45% of it kernel time. **A zero-copy flip is independent of
area; this is per-pixel work, i.e. a copy.**

## 4. Why the buffer is not flippable

* The driver advertises `VK_EXT_image_drm_format_modifier` but offers **only**
  `DRM_FORMAT_MOD_LINEAR` (`pvr_formats.c:489,504`, with the comment "We support LINEAR only yet").
* zink's swapchain target reports **`modifiers=0 m0=0x0`** - no modifier is negotiated.
* weston reports **"DRM: supports GBM modifiers" / "dmabuf support: modifiers"**, so the
  compositor is willing; the driver simply has nothing flippable to offer.

## 5. This is the upstream-documented behaviour

Roman Gilg's GSoC work on Present in Xwayland (mentor Daniel Stone) states the constraints
directly:

> "it should be able to flip Glamor/GBM Pixmaps of Present supporting clients to a Wayland
> surface for **one full screen window without copies**. The restriction of only one window,
> which is full screen, is in line with what is possible with the currently available
> functionality by the Present code in the Xserver."

> "I have tried to queue events on msc timings, but it's not really straightforward ... we need
> to read out some sort of Vblank counter from the Wayland server instead of the kernel. The
> frame callback is a candidate for that, but **it needs at least ... always a previous buffer
> commit to signal back** ... So for now I don't allow queuing events, which means that Pixmap
> flips are applied always immediately."

Source: <https://marc.info/?l=freedesktop-xorg-devel&m=150086203520950&q=raw>

So: Xwayland's Present does not queue on MSC, and its frame-callback pacing needs a prior
commit - the client -> Xwayland -> weston -> release loop is **serial by construction**. When
the buffer is flippable that loop is cheap; when it must be copied, the copy cost is added to
every iteration and the client blocks on the release for the whole time.

## Consequence for the fix

The `vk_sync` ioctl churn (target 1) is real but secondary: it is ~10 ms/frame of Xwayland
kernel time. **The dominant term is the per-frame copy, and the way to remove it is to give the
driver a flippable tiled buffer layout - i.e. FBCDC.** My earlier "tiling does not matter"
result (`vkrender` LINEAR 218 vs OPTIMAL 211 Mpix/s) measured *rendering* and was correct, but
it did not cover *presenting*: there, the modifier decides whether Xwayland can flip at all.
