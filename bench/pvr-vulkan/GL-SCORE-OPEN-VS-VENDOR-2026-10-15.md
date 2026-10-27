# GL score: open vs vendor, and the env var that hid it

2026-10-15, Radxa Cubie A7A, kernel 6.6.98-5-aw2511.

## The result

| | **VENDOR** | **OPEN** |
|---|---|---|
| session | X server on `pvrsrvkm` | **Xwayland on weston on `powervr`** |
| `GL_RENDERER` | zink Vulkan 1.3(…**IMAGINATION_PROPRIETARY**) | zink Vulkan 1.3(…**IMAGINATION_OPEN_SOURCE_MESA**) |
| `GL_VERSION` | OpenGL ES 2.0 Mesa 25.0.7-2+deb13u1 | OpenGL ES 2.0 **Mesa 26.3.0-devel (git-8c1ea61dd1)** |
| **glmark2 Score** | **280 · 287 · 302** (three runs) | **226** |
| scenes with FPS | 33 | **33** |
| errors | 0 | **0** |

**Same `glmark2-es2` (the X11 variant), same X11 protocol, same Mesa/zink front end — only the Vulkan driver
underneath differs.** **So this is a clean comparison, and the open driver is ~20-25% behind on GL with no rendering
errors.**

## The env var that made this look impossible

`env26.sh` exports:

```sh
export MESA_LOADER_DRIVER_OVERRIDE=${MESA_LOADER_DRIVER_OVERRIDE:-sun4i-drm}
```

**That is correct for WESTON** — the GPU cannot drive the display directly, so the display server needs kmsro +
the sun4i-drm KMS driver. **It is WRONG for a client.** **A client inheriting it asks for the KMS driver and cannot
initialise a canvas:**

```
Error: main: Could not initialize canvas
```

**Which is exactly what six previous attempts hit.** **The fix is two lines in the client's environment:**

```sh
. /home/radxa/gpu-open-stack/env26.sh
unset MESA_LOADER_DRIVER_OVERRIDE     # server-only setting
export GALLIUM_DRIVER=zink            # what wclient.sh already did
DISPLAY=:0 glmark2-es2 -s 800x600
```

**`wclient.sh` — the existing client wrapper — had this right all along** (`GALLIUM_DRIVER=zink`, no override).

## The gate that has to come first

**`switch-open.sh` runs `rmmod pvrsrvkm` without `set -e`.** **With the desktop up that module's refcount is ~188; it
only falls to 0 once every client has released the device.** **If it is not 0 the rmmod fails SILENTLY, the driver
never changes, and the script reports a failed switch.** **So the switch must be gated:**

```sh
while [ "$(awk '$1=="pvrsrvkm"{print $3}' /proc/modules)" != "0" ]; do sleep 1; done
```

**A stuck refcount (observed at 2, unmoving for 40 s) does not clear by waiting — it clears on reboot.**

## Compositor compatibility, now measured

| compositor | vendor | open |
|---|---|---|
| **X11 + `kwin_x11`** (the installed desktop) | **works** | **FAILS** — `X` cannot start (`Failed to read display number from pipe`, `Attempt 1 ... on vt 2 failed`), so `kwin_x11` never launches |
| **weston + Xwayland** | — | **works** — socket present, `Xwayland :0` present, glmark2 scores 226 |

**⚠️ On the open driver, `display-manager` reports `active` while `kwin_x11`, `X` and `plasmashell` are all absent.
Do not trust that unit's state there.**

## Reproducing

```sh
sudo bench/pvr-vulkan/open-wayland-score.sh          # gates on the refcount, switches, weston, glmark2, restores
```

**It refuses to switch unless the refcount is 0, and its `EXIT` trap restores the vendor driver and the desktop.**
