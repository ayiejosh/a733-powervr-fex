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

## Results, 2026-11-04 (runs 1104-0817-vendor, 1104-0842-open)

Same board, same 1920x1080 screen at 1.35 scale. Vendor: X11 Plasma, apps on zink over the vendor
Vulkan driver. Open: Plasma Wayland, PR #12 Mesa, kernel with the CCCB fix.

| | vendor | open |
|---|---|---|
| Desktop compositing | off (kwin_x11, not composited) | OpenGL (kwin_wayland on zink) |
| Chromium WebGL, 600 cubes | **no WebGL context** | 39.2 fps, p95 33.5 ms, picture ok |
| Chromium CSS, 300 layers | 15.8 fps | 21.0 fps |
| Chromium 1080p video, dropped | 2 / 606 | 6 / 605 |
| Firefox WebGL | 36.7 fps, **picture wrong** (dark slivers, no cubes) | 17.9 fps, **picture wrong** (window not shown; the run before drew at quarter size) |
| Firefox CSS | 9.4 fps | 8.5 fps |
| Firefox 1080p video, dropped | 0 / 598 | **299 / 598** |
| glmark2 score (default GL) | 297 | 546 (zink), 578 (native GLES2), 376 (zink forced) |
| glmark2 --validate | 26 ok, 1 failed (`[function] fragment-complexity=medium`) | 27 ok, 0 failed (default, native and zink) |
| 15-min soak | ran, session up | ran, session up |
| GPU errors in the kernel log | 0 | 0 |
| Idle cost (desktop processes), RAM | 1.4 % of a core, 2345 MB | 1.5 % of a core, 2526 MB |
| Max temperature | 66.6 C | 68.0 C |
| visual-fails | 3 | 1 |

Open is ahead on everything Chromium and glmark2 do, with a correct picture, and the desktop is
GPU-composited. Firefox is the gap: WebGL draws wrong on both stacks, and on the open stack Firefox
drops half the video frames.
