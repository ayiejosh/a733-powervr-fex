# Desktop benchmark: vendor stack vs open stack, checked visually

`daily.sh vendor|open` measures one GPU stack in a real Plasma desktop on the HDMI screen and needs no
one at the board: every result is a text log or a screenshot under the run directory.

- `vendor`: the running X11 Plasma desktop on the vendor `pvrsrvkm` driver; nothing is switched.
- `open` (`DESKTOP=plasma`): the display manager is stopped, `switch-safe.sh open` binds the open
  `powervr` driver and switches the system to the open Mesa (`system/open-stack/`), and Plasma Wayland
  starts in a logind session on tty2 (`plasma-open.sh`). At exit everything is torn down and the vendor
  desktop is restored. `wedge-guard.sh` runs first: it reboots the board through sysrq if the GPU
  wedges (a wedge also blocks `sudo`).

Steps: compositor/GL/Vulkan info; Chromium and Firefox on `page.html` (600 WebGL cubes, 300 animated
CSS layers, 1080p H.264 video); glmark2 windowed, plus `glmark2 --validate` (open: default, native
GLES2 and zink); a 15-minute glmark2 soak; 2 minutes of idle cost.

**Numbers count only with a correct picture.** Each page phase and each desktop state is screenshot
(KWin ScreenShot2, `shot.py`) and checked by `vcheck.py` (WebGL/CSS: every cell of a 4x4 grid has
detail along x, which catches missing, partial and smeared frames; video/desktop: not black, not
flat). `glmark2 --validate` compares each scene with glmark2's reference pixels. Failures are counted
as `visual-fails=N` on the last line, and the screenshots are also looked at by hand.

Paths are those of the test board (`~/open-perf/tasks/T-2026-11-03-daily/{assets,runs}`, the repo in
`~/a733-powervr-fex`). `video.mp4` is not in the repo (16 MB); any 25 s 1920x1080 30 fps H.264 clip
works, for example:

```sh
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 25 -c:v libx264 -pix_fmt yuv420p video.mp4
```

Only `DESKTOP=plasma` is shipped here; `daily.sh` still knows the `weston` and bare `kwin` modes used
while bringing the desktop up, whose launchers set the libraries through the environment.

## Results, 2026-11-04, same resolution (runs 1104-0817-vendor, 1104-1109-open)

Both stacks at 1280x960, scale 1 (the vendor desktop's mode; the open run sets it with `MODE=1280x960@60`).
Vendor: X11 Plasma, not composited, apps on zink over the vendor Vulkan driver. Open: Plasma Wayland,
this repo's Mesa (patches up to 0400) and kernel (patches up to 0007). Open Chromium: 3 runs.

| | vendor | open |
|---|---|---|
| Desktop compositing | off | OpenGL (kwin_wayland on zink) |
| Chromium WebGL, 600 cubes | **no WebGL context** | 43.8-45.6 fps, picture ok |
| Chromium CSS, 300 layers | 15.8 fps | 14.5-19.5 fps |
| Chromium 1080p video, dropped | 2 / 606 | 1-3 / 605 |
| Firefox WebGL | 36.7 fps, **picture wrong** (dark slivers) | 28.9 fps (52.9 in a separate run), picture ok |
| Firefox CSS | 9.4 fps | 9.1 fps |
| Firefox 1080p video, dropped | 0 / 598 | 29 / 598 (34 in a separate run) |
| glmark2 windowed | 297 | 495 (zink), 526 (native GLES2), 342 (zink forced) |
| glmark2 --validate | 26 ok, 1 failed | 27 ok, 0 failed (zink, native and zink forced) |
| 15-min soak | ran, session up | ran, session up |
| GPU errors | 0 | 0 while in use (2 firmware context resets when the soak's glmark2 is killed) |
| Idle cost, RAM | 1.4 % of a core, 2345 MB | 2.0 % of a core, 2447 MB |
| Max temperature | 66.6 C | 64.9 C |
| visual-fails | 3 | 0 |

At the open stack's own 1920x1080, scale 1.35 (Firefox renders at 2x, 2844x1600): Chromium WebGL
39 fps; Firefox drops about half the video frames (293-351 / 598) whatever compositor settings it is
given, so that drop is the extra pixels, not a broken path.

## Daily apps on the open stack (2026-11-04)

Run in a held Plasma Wayland session (`HOLD=1`, `apptest.sh`, `browsertest.sh`), each checked from a
screenshot and the kernel log.

| App | Through | Result |
|---|---|---|
| Konsole, Dolphin, Gwenview | native Wayland (Qt) | ok |
| Chromium, Firefox (WebGL, CSS, video) | native | ok |
| `es2gears_wayland`, `vkcube --wsi wayland` | native GLES / Vulkan | ok |
| `glxgears`, `vkcube --wsi xcb`, `xeyes` | X11 through Xwayland | ok (Vulkan X11 needed the vendor ICD hidden, `system/open-stack`) |
| `xeyes` (x86-64) | FEX | ok |
| Notepad | wine (Hangover), `guirun` | ok |
| D3D11 tests (`gpu-experiment/mind3d`: tri, cube, tex, depth, mrt2, rtt, bctex) | wine + FEX (ARM64EC), wined3d on zink | ok, feature level 10_1 |
| D3D11 `compute.exe` | same | fails: wined3d needs GL 4.3 for compute; zink is GL 3.2 here |

Not caused by the open stack (same on vendor): DXVK-Sarek v1.12.0 crashes after `vkCreateDevice` with
Hangover 11.16 (the prefix runs wine's own d3d DLLs); `xeyes` under box64 fails on a `libXt` symbol;
x86 Google Chrome is no longer in the FEX rootfs.

Fixed on the way: a kernel oops from Firefox (`kernel/powervr-backport` 0003-0007), Firefox WebGL drawn
in a quarter of the window (`mesa` 0400), Vulkan X11 clients crashing in the vendor ICD, and launchers
that only knew the vendor stack.
