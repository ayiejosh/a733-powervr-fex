# GPU OpenGL (Zink) + a GPU-composited Wayland desktop

## 1. OpenGL via Zink-on-Vulkan

The vendor stack gives you Vulkan (via `libVK_IMG`) but no usable display-GL. The
working route to **GPU OpenGL** is Zink (GL → Vulkan) on a recent Mesa, on top of
the vendor Vulkan ICD.

Build Mesa 25.3.x with:

```
-Dgallium-drivers=zink,softpipe -Dvulkan-drivers= \
-Dplatforms=x11,wayland -Dglvnd=disabled -Dllvm=disabled
```

(Mesa 25.3 needs `wayland-scanner` ≥ 1.20; bullseye ships 1.18 — build wayland
1.23 first and put its `bin` on PATH + `pkgconfig` on `PKG_CONFIG_PATH`.)

**One required patch:** the img Vulkan device lacks `robustness2.nullDescriptor`,
which Zink hard-requires. In `src/gallium/drivers/zink/zink_screen.c`, find the
`nullDescriptor` check (around the feature-validation block, ~line 3457 in 25.3.6)
and make it **warn + continue** instead of `goto fail`. (`fillModeNonSolid=0` is
only a warning — wireframe — not a blocker.)

Run any GL app against it:

```sh
GALLIUM_DRIVER=zink MESA_LOADER_DRIVER_OVERRIDE=zink \
LIBGL_DRIVERS_PATH=<mesa-zink>/lib/aarch64-linux-gnu/dri \
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json \
LD_LIBRARY_PATH=<mesa-zink>/lib/aarch64-linux-gnu:/usr/local/lib \
<your-gl-app>
# GL_RENDERER -> zink (PowerVR B-Series BXM-4-64 MC1)
```

> Gotcha: `LIBGL_ALWAYS_SOFTWARE=1` is often set in desktop session env and forces
> zink-CPU. `unset` it.

Native GLES (no Zink) also works **offscreen** via `LD_LIBRARY_PATH=/usr/local/lib`
against `/dev/dri/renderD128`, and on a native glamor X11 server — but **not** as a
default and **not** for Wayland clients (see `docs/FINDINGS.md`).

## 2. GPU-composited Wayland desktop (`sway` + `wayvnc`)

`sway/` holds a working, capturable GPU desktop:

- `sway.config` — headless output 1280×720, keybinds, autostart terminal.
- `waybar.config` + `waybar-style.css` — status bar with **tappable Apps/Term
  buttons** (usable over touch/VNC, where there's no Super key).
- `sway-headless.service`, `wayvnc.service` — user services (enable + linger).

```sh
mkdir -p ~/.config/sway ~/.config/waybar ~/.config/systemd/user
cp sway/sway.config        ~/.config/sway/config
cp sway/waybar.config      ~/.config/waybar/config
cp sway/waybar-style.css   ~/.config/waybar/style.css
cp sway/{sway-headless,wayvnc}.service ~/.config/systemd/user/
systemctl --user enable --now sway-headless wayvnc      # linger must be on
```

`sway-headless.service` runs sway with `WLR_BACKENDS=headless WLR_RENDERER=gles2
LD_LIBRARY_PATH=/usr/local/lib` (vendor GLES → GPU compositing). `wayvnc` serves it
on `127.0.0.1:5901`; point a websockify/noVNC at that for a browser desktop.
`grim` (wlr-screencopy) confirms it's compositing on the GPU.

This is GLES2-class — great for a compositor, **not** enough for KDE/KWin (needs
GL ≥ 3). That's a hard wall here; see `docs/FINDINGS.md`.
