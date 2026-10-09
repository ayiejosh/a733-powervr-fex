# WSI images are LINEAR-only; fullscreen does not bypass compositing

## Fullscreen does not help

```
windowed   1080p: 13 FPS
fullscreen 1080p: 13 FPS
```

Weston composites either way - an X window made fullscreen is not a fullscreen opaque Wayland
surface, so there is no direct-scanout candidate. The composite pass cannot be dodged this way.

## The images are LINEAR

`pvr_wsi_init()` sets `supports_modifiers = true`, but only one modifier is ever produced:

```
pvr_formats.c:489   mp->drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
pvr_image.c         /* Only support LINEAR now */
                    *modifier = DRM_FORMAT_MOD_INVALID;   /* linear */
                    assert(image->vk.drm_format_mod == DRM_FORMAT_MOD_LINEAR);
```

**No tiled swapchain image exists, so there is no detile path.** The hypothesis inverts: the penalty
is not a slow detile of a tiled buffer, it is that **linear sampling is itself slow** on this tiling
GPU, and weston's composite samples the client's linear image. The client's own render *writes* its
linear image rather than sampling it - a different access pattern, plausibly far cheaper - which
would explain 321 Mpix/s rendering vs ~25 Mpix/s compositing on the same driver.

## Blocked, not missing

Advertising a tiled modifier would let weston sample a tiled texture, but tiling is blocked by the
kernel UAPI: every `DRM_FORMAT_MOD_PVR_*` is FBCDC-based and mainline has no FBD allocation ioctl
(14 ioctls checked, none for FBD). So **linear-only is a hard constraint here**, not a flag.

## Remaining unblocked options

1. Reduce composite passes / pixels touched (pass count, client-buffer vs output format match) -
   weston-side, no UAPI change.
2. Improve raw render: marginal 321 Mpix/s vs vendor 380-696 is 1.2-2.2x and is in Mesa's scope.

Next instrument: weston's own GPU time for a composited frame, to confirm the penalty is in the
composite rather than in the sync round-trip sharing the same 83 ms residual.
