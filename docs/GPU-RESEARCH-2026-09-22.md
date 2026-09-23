# GPU research — what the vendor and manufacturer actually ship, and what the GPU can really do

**Date:** 2026-09-22 · **Board:** Radxa Cubie A7A (Allwinner A733, sun60iw2) · **Kernel:** 6.6.98-5-aw2511
**GPU:** PowerVR B-Series **BXM-4-64 MC1**, **BVNC 36.56.104.183** · **Driver:** Imagination DDK **24.2@6603887**
(kernel `pvrsrvkm` from `img-bxm-dkms` 0.1.0-3 + userspace in `/usr/local/lib`)

This document answers a specific question — *is there a vendor or manufacturer release, driver, or
documentation we can use to make this GPU more capable or more compatible?* — and reports three
corrections to what this repo previously said about the GPU, each measured on the board today.

Illustrated summary: [`comics/`](comics/) · capability matrix it corrects: [`FINDINGS.md`](FINDINGS.md)
· GPU throughput numbers: [`../bench/GPU_BENCHMARK.md`](../bench/GPU_BENCHMARK.md)

---

## 0. Short version

| | what this repo said | what is true (measured 2026-09-22) |
|---|---|---|
| desktop rendering | "software-rendered X11" | **the X server has been rendering on the PowerVR GPU all along** — `glamor X acceleration enabled on PowerVR B-Series BXM-4-64`, and it holds 6 clients on `renderD128` as DRM master of `card0`. The *clients* are software, not the server |
| windowed GL | "does NOT work" | **it works** — a GLES 3.2 window on `:0` renders on the GPU with 0 swap errors and survives 40 s at 1080p60 (28 277 GPU IRQs, 62 °C, kernel log clean) |
| "GPU desktop = hard-blocked" | blanket block | the block is real but **narrow**: no GLX, and the vendor DDK has **no Wayland surface extensions at all**. X11 clients *can* use the GPU today |
| firmware for our BVNC | "not upstream; needs an IMG build request" | **Imagination has published it.** `powervr/rogue_36.56.104.183_v1.fw` exists on their `powervr` branch and is already installed here, byte-identical (`sha256 1db1c399…`) |
| the slow window | "GPU is fast, wiring is missing" | the GPU *is* fast (579 Mpix/s off-screen) but **any per-frame GPU sync costs 1.3–48 ms**; that is the "slow window" |

---

## 1. The vendor stack: what is actually on this board

Two independent halves, and only one of them is a package:

| piece | where it comes from | licence |
|---|---|---|
| kernel module `pvrsrvkm` (DKMS, source at `/usr/src/img-bxm-dkms-0.1.0-3`) | `img-bxm-dkms` 0.1.0-3, Radxa apt `a733-trixie-test`; source from [`radxa/allwinner-bsp`](https://github.com/radxa/allwinner-bsp) branch `cubie-aiot-v1.5.0`, `modules/gpu/img-bxm/linux/rogue_km` | **dual MIT / GPL-2.0** — we may patch and rebuild it |
| userspace DDK — `libsrv_um`, `libusc`, `libpvr_dri_support`, `libVK_IMG` (Vulkan ICD), `libPVROCL` (OpenCL), `libPVRScopeServices`, `libGLESv2_PVR_MESA`, `/usr/local/lib/dri/{pvr,sunxi-drm,swrast}_dri.so` (one 15 MB unified driver), `/usr/local/lib/libpvr_mesa_wsi.so`, loader `libvulkan.so.1.3.280` | **no apt package** — bundled inside Allwinner's [`xserver-xorg-img-bxm_1.21.1-2_arm64.deb`](https://github.com/radxa/allwinner-debian) (27.5 MB, 83 files, maintainer `Allwinnertech`) | prebuilt binaries; no source, no licence file found |

Two consequences worth knowing:

1. **The X server on this board is the vendor's fork, not Debian's.** `/usr/bin/Xorg` is a dpkg
   *diversion* installed by `xserver-xorg-img-bxm-1.21.1-2.deb` (Debian's is parked at
   `/usr/bin/Xorg.bak`), together with the vendor `modesetting_drv.so` and `libglamoregl.so`
   (Debian's kept as `.bak`). `/etc/ld.so.conf.d/00_xserver-xorg-img-bxm.conf` puts `/usr/local/lib`
   ahead of the system Mesa, so the vendor EGL/GLES/Vulkan win by default.
2. **That vendor Xorg is built without GLX.** `xdpyinfo` lists 25 extensions and GLX is not one of
   them; `glxinfo` fails with *"couldn't find RGB GLX visual or fbconfig"*; the log contains no GLX
   module load; `strings /usr/bin/Xorg | grep -c "Initializing extension GLX"` = **0**. The vendor
   DDK ships GLES only (`EGL_CLIENT_APIS: OpenGL_ES`) — there is no `libGL`, so desktop GL has to
   come from **zink** (GL→Vulkan) or not at all. This is the concrete reason "KDE Discover uses
   OpenGL rendering, which is not supported by the current GPU driver"
   ([Radxa FAQ](https://docs.radxa.com/en/cubie/a7a/faq)).

The package database no longer records either thing (`dpkg -l` shows neither `img-bxm-dkms` nor
`xserver-xorg-img-bxm`, though both are installed; several hundred `:amd64` entries have lost their
`.list` files). Treat `dpkg -S`/`apt` state on this board as advisory, not authoritative.

---

## 2. Corrections to this repo's own claims, measured

### 2.1 The X server already uses the GPU

`/var/log/Xorg.0.log` (boot of 2026-09-22):

```
(II) modeset(0): glamor X acceleration enabled on PowerVR B-Series BXM-4-64
(II) modeset(0): glamor initialized
(II) modeset(0): [DRI2]   DRI driver: sunxi-drm
(II) Initializing extension DRI3
```

and at runtime:

```
$ sudo cat /sys/kernel/debug/dri/128/clients      # the pvr render node
             command  tgid dev master a   uid
                   X  1018 128   n    y  0
                   ...  (6 clients)
$ sudo cat /sys/kernel/debug/dri/0/clients        # sunxi-drm, the display
                   X  1018   0   y    y  0        # master
```

So `X` (pid 1018) is DRM master of the display and holds six render-node clients: 2D acceleration is
being done by the PowerVR through glamor+EGL, exported as dma-bufs and scanned out by `sunxi-drm`.
This has been stable for 8+ hours of desktop use. What is software on this board is the **clients**
(`~/.config/plasma-workspace/env/10-software-render.sh` exports `LIBGL_ALWAYS_SOFTWARE=1` and
`QT_QUICK_BACKEND=software` on X11), plus `picom --backend xrender`, which is CPU compositing.

### 2.2 Windowed GPU rendering works on the display today

New harness [`../bench/gles-x11.c`](../bench/gles-x11.c) — a real X11 window, a real EGL window
surface, a real `eglSwapBuffers` per frame, phase-by-phase timing:

```
$ LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0 ./gles-x11 1920 1080 4 2400     # 40 s, vsync on
EGL_VENDOR  : Mesa Project                    GL_VENDOR   : Imagination Technologies
GL_RENDERER : PowerVR B-Series BXM-4-64       GL_VERSION  : OpenGL ES 3.2 build 24.2@6603887
RESULT      : 59.8 fps  124.0 Mpix/s  (40.141 s, 0 swap errors)
PHASES      : draw 6.54 | glFinish 9.92 | eglSwapBuffers 0.26  ms/frame
pvr IRQs    : 28277        temp 59 → 62 °C        kernel log: 0 Oops / BUG / Call trace
```

1080p60, sustained, on the GPU, presented to the real HDMI display, with zero errors. The same
works through **zink** (`GL_RENDERER: zink Vulkan 1.3(PowerVR B-Series BXM-4-64 MC1 …)`, system Mesa
25.0.7, IMG Vulkan ICD via `VK_KHR_xcb_surface`), which is the path desktop GL has to take here.

### 2.3 The GPU is fast; *synchronising* is slow

`bench/glbench.c` (offscreen FBO, 1280×720, vendor stack) against the same shader with one
`glFinish()` per frame — the only difference being the sync:

| shader loop | pipelined (no per-frame sync) | `glFinish()` per frame | cost of one sync |
|---|---|---|---|
| 1 | 9 125 fps | 756 fps | **1.3 ms** |
| 4 | 6 992 fps | 278 fps | **3.6 ms** |
| 16 | 2 302 fps | 81 fps | **12.4 ms** |
| 64 | 598 fps | 21 fps | **47.9 ms** |

And windowed at 800×600, where the sync simply moves into whichever call blocks first:

| shader loop | draw | `glFinish` | `eglSwapBuffers` | fps |
|---|---|---|---|---|
| 4 | 0.20 ms | 3.11 ms | 0.26 ms | 280 |
| 64 | 0.28 ms | 26.02 ms | 0.36 ms | 37.5 |
| 256 | 0.29 ms | 99.23 ms | 0.34 ms | 10.0 |

Everything else was ruled out:

* **it is not the window buffer** — rendering into a GPU-local FBO and then copying it into the
  window (`BLIT=1`) changes nothing: 26.00 → 26.03 ms/frame at loop 64;
* **it is not the X server** — Xorg burns 8.4 % of one core during the windowed run (3.5 % when
  idle-ish), so there is no CPU readback/copy hiding there;
* **it is not the swap** — `eglSwapBuffers` is 0.04–0.36 ms in every configuration;
* **it is not resolution-bound** — windowed throughput is a flat ≈18 Mpix/s from 320×240 to
  1280×720 (199 / 57 / 37.5 / 19.9 fps), while the same shader does 579 Mpix/s off-screen.

So the cost is per-sync and scales with the *shader*, which is why it looks like "the window is
slow": a vsync'd client pays it once per frame. At the panel's 1920×1080 with light (UI-like)
shaders that is 6.5 ms draw + 9.9 ms sync = 16.4 ms — just inside a 60 Hz frame, which is exactly
what the 59.8 fps above shows. **GPU compositing at 1080p60 is feasible on this hardware; heavier
per-pixel work per frame is not.**

---

## 3. What the manufacturer actually ships (the answer to the question)

### 3.1 Imagination — the open stack now covers this exact GPU

| artefact | status |
|---|---|
| **Firmware for our BVNC** | **published.** `powervr/rogue_36.56.104.183_v1.fw` on [`imagination/linux-firmware` branch `powervr`](https://gitlab.freedesktop.org/imagination/linux-firmware) — added `0fb5cfd8` (2025-09-17), re-issued `8a58f818` (2026-04-15, "version 1.1.OS@6976702"). Installed here at `/lib/firmware/powervr/`, byte-identical to branch HEAD. **Not yet in upstream linux-firmware**, which carries only 33.15.11.3 / 36.52.104.182 / 36.53.104.796 |
| open firmware format | verified on the installed file: `info_version 3`, flags `0x1` (`OPEN_SOURCE`), fw version `1.1`, packed BVNC `36.56.104.183` — i.e. exactly what `pvr_fw_validate()` demands. The vendor's `rgx.fw.36.56.104.183` is `info_version 2`, flags `0x80020810` (closed) and is *not* reusable |
| kernel driver | `drivers/gpu/drm/imagination` (module `powervr`), mainline since **6.8**. Supported list today: 33.15.11.3 (AXE-1-16M), 36.52.104.182 (BXM-4-64 MC1), 36.53.104.796 (BXS-4-64 MC1) — **ours is absent**, `exp_hw_support=1` bypasses the `-ENODEV`, and BXM-4-64 was promoted to "supported" by `e55fead2` (2026-07-24) after a CTS run |
| Mesa | `src/imagination` (**Vulkan only**, "pvr"); [`device_info/bxm-4-64.h`](https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/src/imagination/common/device_info/bxm-4-64.h) already defines **`PVR_DEVICE_IDENT_36_V_104_183` (0x36104183) = our GPU**, Vulkan 1.2. Debian 13 ships no pvr ICD, so this needs a newer/self-built Mesa |
| documentation | [docs.imgtec.com](https://docs.imgtec.com/html/index.html) is public (no NDA) but has **no hardware reference manual**; the only register document IMG publishes is Rogue-only (`rogue-registers-description-docs.zip`). The de-facto hardware documentation is the kernel's [`pvr_rogue_cr_defs.h`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/gpu/drm/imagination/pvr_rogue_cr_defs.h) (342 815 bytes of register fields) + [`pvr_drm.h`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/include/uapi/drm/pvr_drm.h) UAPI + Mesa's per-BVNC tables |
| tools | PVRTune / PVRScope / PVRCarbon / PVRTexTool / PVRShaderEditor are downloadable without an account ([downloads](https://developer.imaginationtech.com/downloads/)) — PVRScope is the on-device counter tool |

### 3.2 Radxa / Allwinner

* The **kernel module source is public** (dual MIT/GPL-2.0) — this is the single most useful
  artefact, because it turns "closed driver" into "a driver we can patch and rebuild".
* Versions published for A733: `img-bxm-dkms` 0.1.0-1 (2025-08-08), 0.1.0-2 (2025-09-08, *"add
  Vulkan ICD support"*), **0.1.0-3 (2026-03-19, current)**. There is a `1.4.9-1 UNRELEASED`
  changelog entry dated 2026-09-09 — the next release exists in the changelog but not in the repo.
* **DDK 24.2@6603887 is frozen across every public branch** (`main`, `powervr`, `cubie-aiot-v1.4.6/8`,
  `v1.5.0`); the only other DDK in the BSP is the older 1.18 line for T527. **No newer public DDK
  exists for B-Series.** Imagination's DDK 25.1 RTM2 (Aug 2025, Vulkan 1.4 on Android) is announced
  but not downloadable without registration.
* Radxa's own metapackage **`task-a733-powervr`** (new in `task-a733` 0.2.8, 2026-03-20) ships:
  `/usr/lib/environment.d/99-powervr-mesa.conf` = `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1`,
  `MESA_LOADER_DRIVER_OVERRIDE=zink`, `LIBGL_KOPPER_DRI2=1`, and `/usr/lib/modprobe.d/powervr.conf`
  = `options powervr exp_hw_support=1`. Read together: Radxa's intended configuration is **the open
  `powervr` driver with an unvalidated GPU, GL delivered through zink, and the vendor Vulkan flag
  that literally says it is broken.** It is not installed on this board.
* Radxa staff, 2026-05-14: **"目前没有Wayland的支持"** — no Wayland support
  ([forum](https://forum.radxa.com/t/gpu-wayland/30823)).
* Allwinner's SDK *documentation* is public (`tina5.0_aiot/product/docs`); the SDK **source** needs
  registration + an uploaded SSH key. The AIOT SDK will not be upstreamed.

### 3.3 What does not exist (so we stop looking)

* **No Wayland on this DDK.** Verified in the binaries here: `libVK_IMG.so.24.2.6603887` has
  `VK_KHR_xcb_surface` (2 hits) and **0 hits** for `VK_KHR_wayland_surface` and `VK_KHR_display`;
  the vendor `libEGL.so.1.0.0` has `EGL_EXT_platform_x11` and **0 hits** for wayland/`wl_display`.
  dma-buf *is* supported (`VK_EXT_image_drm_format_modifier`, `VK_EXT_external_memory_dma_buf`).
  A Wayland compositor cannot present on this GPU, full stop.
* **No GLX**, therefore no desktop GL without zink.
* **No newer kernel for A733**: `linux-image-radxa-a733 6.6.98-5` (2026-09-20) is newest, and its
  config has no `CONFIG_DRM_POWERVR`; the source tree (`radxa/kernel`,
  `allwinner-aiot-linux-6.6`) has **neither `drivers/gpu/drm/imagination` nor `drm_gpuvm`**.
* **No mainline A733 platform support** — clk/CCU, pinctrl, pmdomain and a `sun60i-a733.dtsi` are
  still posted-not-merged, and there is no GPU node; the vendor's `img,gpu` compatible matches
  nothing upstream (it would have to become `img,img-bxm-4-64`, `img,img-rogue`).
* **No B-Series register manual**, public or purchasable.

---

## 4. Ranked paths from here

| # | path | what it buys | cost / risk |
|---|---|---|---|
| 1 | **Let X11 clients use the GPU again**: drop `LIBGL_ALWAYS_SOFTWARE=1` for GLES/EGL apps, route desktop GL through `zink` (the DDK has no GLX), keep `picom --backend xrender` | the desktop's own clients stop burning CPU on pixels; GPU GLES apps work today (proven §2.2) | hours; low risk for GLES clients, **medium for a GL compositor** (see below) |
| 2 | **GPU-composited KDE/X11** — `KWIN_COMPOSE=O2ES` (KWin's OpenGL ES backend, which the vendor EGL provides) | the actual "windows drawn by the GPU" goal | hours; **this is the class that deadlocked before** — a hang costs a power-cycle, which the HW watchdog turns into a ~96 s reboot |
| 3 | **Patch the vendor module** — the PRIME-import patch already in [`../kernel/pvrsrvkm-drm-prime-import.patch`](../kernel/pvrsrvkm-drm-prime-import.patch) (`prime_fd_to_handle`/`gem_prime_import` are unimplemented in the running module — verified) is what wlroots/kmsro/zink need; the module source is MIT/GPL so we may also go after the old `mutex_spin_on_owner` deadlock | technically unblocks the compositor path at the kernel level | days; needs a module rebuild + reload, and X currently holds 17 refs on it (so it means stopping the display or rebooting) |
| 4 | **Open stack on this board** — backport `drm_gpuvm` + `drivers/gpu/drm/imagination` to the 6.6 BSP kernel, build Mesa with the pvr ICD, `exp_hw_support=1`, firmware already present | mainline GPL/MIT driver, no deadlock class, Vulkan 1.2, MSAA/ASTC, PVRScope counters; removes the closed blob from the display path | weeks; driver needs ≥ 6.8 APIs, `Module.symvers` has no `drm_gpuvm*`, so it is a real port |
| 5 | **Contribute upstream** — (a) get `rogue_36.56.104.183_v1.fw` from IMG's branch into upstream `linux-firmware`; (b) add `PVR_PACKED_BVNC(36,56,104,183)` to `pvr_device.c` | makes path 4 a configuration instead of a hack for everyone with an A733 | small patches, but outward-facing → needs your go-ahead |
| 6 | Wait for a newer Radxa kernel / mainline A733 | everything above, done by others | months |

Path 1 is the "more compatible" answer, path 2 the "more capable" one, path 4 the real fix.

---

## 5. Repro

```sh
# windowed GPU render on the display (X11, vendor EGL/GLES) — the "can it show a window" test
cd bench && gcc -O2 gles-x11.c -o /tmp/gles-x11 -lX11 -lEGL -lGLESv2
LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0 SWAP_INTERVAL=0 /tmp/gles-x11 800 600 64 200
LD_LIBRARY_PATH=/usr/local/lib BLIT=1 SWAP_INTERVAL=0 /tmp/gles-x11 800 600 64 200   # offscreen+copy
glrun /tmp/gles-x11 800 600 64 300                                                    # via zink->Vulkan

# the sync cost, isolated from any window
gcc glbench.c -o /tmp/glbench -lEGL -lGLESv2 -lgbm -ldl
LD_LIBRARY_PATH=/usr/local/lib /tmp/glbench /dev/dri/renderD128 64 300                # pipelined
#   -> add glFinish() inside the frame loop for the per-frame-sync number (see §2.3)

# who is using the GPU
sudo cat /sys/kernel/debug/dri/128/clients ; sudo cat /sys/kernel/debug/dri/0/clients
grep -i glamor /var/log/Xorg.0.log | head -3

# the open-stack prerequisites, on this board
ls -l /lib/firmware/powervr/                                  # open-ABI fw for 36.56.104.183
python3 -c "d=open('/lib/firmware/powervr/rogue_36.56.104.183_v1.fw','rb').read();\
import struct;print([hex(x) for x in struct.unpack('<16I',d[-4096:-4032])])"
grep -c DRM_POWERVR /boot/config-$(uname -r)                  # 0
```

## 6. What is *not* established here

* Whether the historical `mutex_spin_on_owner` hang still reproduces — the compositor path was
  attempted after this research was written and did **not** hang the kernel: KWin crashes in
  userspace instead (§7). §2.2 shows the X11 client path is stable under sustained load.
* Whether `KWIN_COMPOSE=O2ES` (path 2) can be made to work — attempted, it crashes in Qt's EGL
  config negotiation, §7. Root cause narrowed but not closed.
* The mechanism behind the per-sync cost (measured, not explained): it tracks shader size, which is
  why it looks like a per-frame shader re-upload rather than a buffer copy. Not confirmed.
* Whether Mesa's pvr driver would actually come up on BVNC 36.56.104.183 — the device table entry
  exists and the firmware matches, but nothing here has run the open driver.
* Whether the DDK's Vulkan ICD passes CTS on this BVNC (Radxa's own flag says it does not).

---

## 7. The GPU-composited desktop, attempted

Path 2 from §4 was run on 2026-09-22 behind a boot-safe guard — `test.sh` / `revert.sh` plus
`gpu-test-guard.service` (a `Before=display-manager` unit that restores the known-good compositor
config if the board reboots mid-test, so a hang cannot become a boot loop). Tooling:
`/home/radxa/gpu-desktop-test/`.

Setup: `kwinrc [Compositing] Enabled=true`, a KWin user-unit drop-in
`plasma-kwin_x11.service.d/gles-test.conf` with `KWIN_COMPOSE=O2ES`, `KWIN_OPENGL_INTERFACE=egl` and
`UnsetEnvironment=LIBGL_ALWAYS_SOFTWARE QT_QUICK_BACKEND` (the Plasma X11 session exports software GL
into the *systemd user manager*, which would otherwise silently defeat the test), then
`systemctl --user restart plasma-kwin_x11.service`.

**It fails in userspace, and it does not hang the kernel:**

| | |
|---|---|
| kernel | **clean** — 0 Oops / BUG / Call trace across every attempt; the board stayed up, the desktop kept running, no power cycle |
| KWin | **SIGSEGV** while initialising GL compositing (`Application::crashHandler() called with signal 11`); systemd restarts it, and it then protects itself: *"Compositing disabled: video driver seems unstable…"* (`[Compositing] LastFailureTimestamp`, `openGLIsBroken=true`) |
| every start, before any of that | `Cannot find EGLConfig, returning null config` — that string is **Qt's** (`libQt6Gui.so.6`), not KWin's |
| Qt's GL integration | tries `xcb_glx` → *"Failed to initialize"* (the vendor Xorg exports no GLX, §1) → falls back to `xcb_egl` → *"successfully initialized"* |
| the test itself | verified to have taken effect: the running KWin's `/proc/<pid>/environ` shows `KWIN_COMPOSE=O2ES` and no `LIBGL_ALWAYS_SOFTWARE` |

So "the live compositor path deadlocks the kernel" (the June conclusion) no longer describes what
happens on this stack: today the failure is a **userspace EGL config negotiation failure inside
Qt/KWin**, contained to the compositor.

It is also not "the driver has no usable configs". New probe
[`../bench/egl-configs.c`](../bench/egl-configs.c) enumerates the vendor EGL on this display:

```
EGL 1.5  vendor=Mesa Project  apis=OpenGL_ES
TOTAL configs=36   window-capable=36   alpha>=8=18   ES2-renderable=36   (visuals 33 and 34)
  minimal RGB888 + WINDOW + ES2             -> matched
  RGB888 + ALPHA8 + depth24 + stencil8      -> matched
  BUFFER_SIZE=32 + ALPHA8                   -> matched
  desktop OpenGL (EGL_OPENGL_BIT) + WINDOW  -> NO CONFIG   <-- the vendor DDK is GLES-only
```

36 configs cover ES2/ES3 in every alpha/depth/stencil/samples combination; X has three visuals
(`0x21` 24-bit TrueColor, `0x22` 24-bit DirectColor, `0x7f` **32-bit ARGB**) and **all three** have a
matching config — so the compositor's ARGB visual is not the problem either.

**The root cause is the API the request names.** New shim
[`../bench/egl-trace.c`](../bench/egl-trace.c) `LD_PRELOAD`s into a process and dumps every
`eglChooseConfig` (hooking `eglGetProcAddress` as well, because Qt resolves its entry points that
way). Against KWin's compositing init:

```
[egl-trace] eglChooseConfig(RED=8 GREEN=8 BLUE=8 ALPHA=0 SAMPLES=0 SAMPLE_BUFFERS=0 DEPTH=0 STENCIL=0
                            SURFACE_TYPE=4 RENDERABLE_TYPE=8)
[egl-trace]   -> ok=1 matched=0   <-- NOTHING MATCHES     (x6, progressively fewer constraints)
Cannot find EGLConfig, returning null config
[egl-trace] eglCreateContext(config=(nil)): ?=2 ?=0
[egl-trace]   -> ctx=0x… err=0x3000        <- EGL_BAD_CONFIG — and a *non-null* context
```

`RENDERABLE_TYPE=8` is `EGL_OPENGL_BIT`: **desktop OpenGL, on every single request.** The vendor DDK
is GLES-only (`EGL_CLIENT_APIS: OpenGL_ES`, §1), so no config can ever match. Qt then logs the
message, calls `eglCreateContext()` with the null config anyway, and gets a context pointer plus
`EGL_BAD_CONFIG` back (a conformant driver returns `EGL_NO_CONTEXT` there) — after which KWin records
the failure, disables compositing, and in the earlier runs segfaulted.

All three obvious levers were measured, and none of them changes the request:

| tried | measured effect |
|---|---|
| `KWIN_COMPOSE=O2ES` (KWin's OpenGL ES backend) | still `RENDERABLE_TYPE=8` — **not one ES2 request is made** |
| `KWIN_OPENGL_INTERFACE=egl` | honoured (EGL is used) but the attribute set is unchanged |
| `QT_OPENGL=es2` | unchanged |

So GPU-composited KWin here is blocked in **Qt's EGL config negotiation**, which asks for an API the
vendor driver does not implement — not in the kernel, and not by anything settable from the
environment. Lifting it needs either a Qt/KWin-side change (ask for ES2 in the QPA/compositor probe)
or a driver exposing desktop-GL configs; neither exists on this stack today.

**State after the test: reverted and verified** — `Enabled=false`, drop-in removed, picom XRender
running, KWin restarted through its unit, sentinel cleared. `LastFailureTimestamp` was deliberately
**left in place**: the protection is accurate now, and it stops anyone re-enabling GL compositing
straight into a crash loop. To retry anyway:
`kwriteconfig6 --file kwinrc --group Compositing --key LastFailureTimestamp --delete`.

---

## 8. What was actually applied (session-wide GPU GL), and the wall that remains

Everything above is diagnosis. This is the part that changed the machine, on 2026-09-22.

### The change

`~/.config/plasma-workspace/env/10-software-render.sh` forced software GL on X11
(`LIBGL_ALWAYS_SOFTWARE=1`, `QT_QUICK_BACKEND=software`) as the board's defence against the
historical deadlock. That defence is no longer justified — §2.2 and §7 show the X11 client path is
stable and the compositor failure is userspace, not a hang — so the X11 branch now routes all GL
through **system Mesa + zink -> PowerVR Vulkan**. The applied file and the revert recipe are in
[`../system/desktop-gl-zink/`](../system/desktop-gl-zink/README.md).

### Why zink and not the vendor GLES

The vendor DDK answers `EGL_CLIENT_APIS: OpenGL_ES` only, while Qt's X11 EGL integration asks for
`EGL_OPENGL_BIT` (§7). Measured side by side:

| desktop-GL + WINDOW request | result |
|---|---|
| vendor EGL | **0 configs** -> Qt logs *"Cannot find EGLConfig, returning null config"*, null config |
| system Mesa + zink | **45 configs**, context created, no message |

### What it buys (measured, same window, same shader)

| workload | GPU via zink | software (llvmpipe) | gain |
|---|---|---|---|
| 320x240, loop=16 | **345.5 fps** (26.5 Mpix/s) | 111.4 fps (8.6 Mpix/s) | **3.1x** |
| 800x600, loop=64 | **33.5 fps** | 120 frames did not finish in 60 s (<2 fps) | **>17x** |

The gain is workload-dependent, and the small-workload case shows why: the GPU path pays a **~2.9 ms
per-frame floor** (the sync cost of §2.3), so cheap frames collapse the ratio to ~3x. Per-pixel
throughput is where the GPU wins by orders of magnitude.

Verified after the change: a new client reports
`GL_RENDERER: zink Vulkan 1.3(PowerVR B-Series BXM-4-64 MC1 (IMAGINATION_PROPRIETARY))`; plasmashell
maps `libVK_IMG` and renders on the GPU via `QT_QUICK_BACKEND=opengl`; KWin still composites with
picom XRender; kernel log clean; shell steady-state CPU unchanged at idle (1.3 % vs 1.7 % of a core).

### The wall that remains: KWin's own compositor

Retried three further ways, each reverted afterwards: `KWIN_COMPOSE=O2` (desktop GL) +
`KWIN_OPENGL_INTERFACE=egl` + `LIBGL_KOPPER_DRI2=1` + both feature fakes. Qt finds configs now, but
the *platform* integration still fails:

```
libEGL warning: egl: failed to create dri2 screen
qt.qpa.gl: Xcb EGL gl-integration initialize failed
QXcbIntegration: Cannot create platform OpenGL context, neither GLX nor EGL are enabled
kwin_core: Compositing disabled: no OpenGL support
```

The block has moved: it is no longer "the vendor driver has no desktop-GL config" (zink fixed that
for clients) but "Qt's X11 EGL/kopper integration cannot initialize" — a Mesa-side limitation, not
something settable from the environment. So the desktop still composites with picom on the CPU,
while its clients and its shell no longer do. Found along the way: zink *hard-requires*
`geometryShader` on an IMG device (`zink: Imagination proprietary driver w/o geometryShader is
unsupported`), which is why the session-wide feature fake is unavoidable on this path.

### Trade-offs on record

* `PVR_FAKE_GS=1` is session-wide, so every Vulkan app is told the device has geometry shaders, while
  the blob rejects GS *pipelines*. An app that trusts the flag and uses GS will fail. This is the
  price of zink here; narrow it by moving the layer and `PVR_FAKE_*` out of the session script and
  into per-application wrappers (as `glrun` does).
* zink logs `PERF WARNING! > 100 copy boxes detected` for the shell — an inefficiency, not an error.
* OpenCL needed nothing: `/etc/OpenCL/vendors/IMG.icd` -> `libPVROCL.so` was already registered.

---

## 9. The PRIME-import patch is applied and verified

The patch in [`../kernel/`](../kernel/README.md) is ours — written for the 5.15 BSP and still
applying to the 6.6 DDK. It is now built, installed, loaded at boot and proven:

| check | result |
|---|---|
| live module build-id | `5d842b08…` (patched; stock is `50b99ea6…`) |
| `drmPrimeFDToHandle` on a foreign dma-buf (from `/dev/dma_heap/system`) | `OK -> handle=1` (stock: `EINVAL`) |
| re-export handle -> fd | `OK` |
| GPU writes into the imported foreign pages | `readback: 65536/65536 == 0xCAFEF00D` -> **PASS** |

Two traps cost two reboots, both recorded in
[`../kernel/prime-import-activation/README.md`](../kernel/prime-import-activation/README.md):

1. **`xz`'s default check breaks `modprobe`.** The DKMS module uses `Check: CRC32`; `xz`'s default is
   `CRC64`; this kernel's in-kernel XZ decoder supports CRC32 only, so it refuses the stream and
   `modprobe` returns `EINVAL` — while `insmod` of the same uncompressed `.ko` succeeds. Compress
   with `xz -c --check=crc32`.
2. **My boot "safety" guard caused the failure it was meant to catch.** It restored the stock module
   before udev's autoload ran, so the stock build loaded and it looked like the patched module would
   not load. It is report-only now, and `pvrsrvkm-load.service` loads the module explicitly before
   `display-manager`, logging the build-id it got.

What it changes for the user, honestly: nothing visible *yet*. The consumers of foreign dma-buf
import (kmsro, wlroots, a compositor that scans out its own GPU buffers) are blocked elsewhere — no
Wayland surface extensions in this DDK (§3.3), KWin blocked in Mesa's kopper integration (§8). It
closes *this* gap so those paths are not additionally blocked by the driver.

## 10. Stage 4: Mesa pvr — the open driver now executes GPU work here

Stage 3 proved the mainline driver binds and accepts firmware. It did not prove anything
executes. This stage closes that: a userspace Vulkan driver talks to the open driver, submits a
compute shader, and the results come back correct.

### The three answers, separately

"Does this board have Vulkan?" turned out to be three different questions, and they have three
different answers. The measurement is the same binary in every case
(`bench/pvr-vulkan/vktest.c`: 1M-element xorshift, every element verified, 10 dispatches).

| path | ICD | kernel driver | result |
|---|---|---|---|
| vendor | `/usr/lib/libVK_IMG.so` (DDK `24.2@6603887`, Vulkan 1.3.277) | `pvrsrvkm` | **PASS**, 2.497 ms/dispatch, 3.36 GB/s |
| Mesa pvr "srv" | Mesa `pvrsrvkm` winsys | `pvrsrvkm` | **refused by design** |
| Mesa pvr "drm" | Mesa `powervr` winsys | **mainline `powervr`** | **PASS**, 2.880 ms/dispatch, 2.91 GB/s |

The vendor ICD is the surprise: the GPU already had working Vulkan 1.3 through Allwinner's DDK,
and nobody had measured it. That is the baseline the open path is now within 13% of, on a
bandwidth-bound compute kernel.

The middle row is closed by an explicit version gate, not by a bug of ours:
`pvr_srv_winsys_create()` calls `pvr_is_driver_compatible()`, which accepts **only downstream
driver version 1.17** (`PVR_SRV_VERSION_MAJ/MIN` in `winsys/pvrsrvkm/pvr_srv_bridge.h`). This
board's vendor module reports `24.2.6603887` through `drmGetVersion`, so Mesa returns
`VK_ERROR_INCOMPATIBLE_DRIVER` before issuing a single ioctl. Mesa's srv backend and this DDK are
not version-compatible, and no amount of local work changes that.

### What the open path needed

Two gates, both found by instrumenting Mesa rather than guessing:

1. **Enumeration is a device-tree whitelist.** `pvr_drm_configs[]` in
   `src/imagination/vulkan/pvr_device.c` maps a render node's `compatible` to a display node's
   `compatible`, and on this board the names are `img,gpu` (GPU) and `allwinner,sunxi-drm`
   (display engine) — neither is in the table (mediatek and TI parts are). With no match the
   driver enumerates **zero** devices, and Mesa 25.x does not accept a display compatible of
   `NULL` either, so both entries are required.
   `mesa/0001-pvr-add-A733-img-gpu-platform.patch`.
2. **Device info for the BVNC.** After that, `pvr_physical_device_init()` fails with
   `-9 VK_ERROR_INCOMPATIBLE_DRIVER` at its first step, `pvr_device_info_init(dev_info, bvnc)`,
   because the driver only supports BVNCs it ships feature/quirk tables for. Mesa 25.0.7–25.2
   ship `axe-1-16m`, `bxs-4-64`, `gx6250` — **not `bxm-4-64`**. Mesa 25.3.0 ships
   `device_info/bxm-4-64.h` containing `PVR_DEVICE_IDENT_36_V_104_183`, which is this GPU.

`struct pvr_device_info` is byte-identical between 25.0.7 and 25.3.0, so the blob can be
backported to an older Mesa (`mesa/0002-...patch`) — but that is a dead end worth recording:
25.0.7's pvr also has a **stub shader compiler** (`pco_nir.c` carries four `finishme`s and
`pvr_hardcode.c` returns empty programs), so it cannot compile a pipeline even with correct
device info. 25.3.0 has no `finishme` in `pco_nir.c` and no hard-coded-program path at all: it
compiles at runtime.

That gives a hard dependency chain for the working build: pvr → CLC → LLVM + **LLVMSPIRVLib
19.1.x** + **libclc** + SPIRV-Tools ≥ 2024.1. On this board LLVM 19.1.7 and SPIRV-Tools 2025.1.1
were already present; LLVMSPIRVLib was built from source (v19.1.15) and libclc came from
Debian's `libclc-19` package plus a hand-written `libclc.pc`. Recipe in `mesa/README.md`.

### The result

```
WARNING: powervr is not a conformant Vulkan implementation, testing use only.
device[0] name="PowerVR B-Series BXM-4-64 MC1" api=1.2.328 driver=0x06403000 vendor=0x1010 device=0x36104183
queue family 0: flags=0x7 count=2
memory: type 0, heap 0 (4436 MB)
dispatching 10 x 1048576 elements (4 MiB per dispatch)...
submit+wait: 28.797 ms total, 2.880 ms/dispatch
throughput:  2.91 GB/s (read+write)
RESULT: PASS - 1048576/1048576 elements correct
```

with, on the kernel side, the same bring-up as stage 3 — `clk_bus enabled`, `reset_bus
deasserted`, firmware `rogue_36.56.104.183_v1.fw` loaded, `powervr 1.0.0 20230904` on minor 1.

Also worth recording: the kernel was never the problem. `pvr_dev_query_gpu_info_get()` fills
`gpu_id` from `pvr_gpu_id_to_packed_bvnc(&pvr_dev->gpu_id)`, and that is read from the hardware
control registers by `pvr_load_gpu_id()`; Mesa's trace showed it receiving
`0x240038006800b7` → `36.56.104.183` correctly. Mesa's vendored `pvr_drm.h` is byte-identical to
this kernel's UAPI header in both 25.0.7 and 25.3.0.

### What this does not show

- It is a **compute** test. A device, a queue, a pipeline, a fence and a readback — no window, no
  presentation, no WSI: this build has `-Dplatforms=` (no X11/Wayland platform), so nothing here
  says anything about scanout, vsync, or the compositor.
- `WARNING: powervr is not a conformant Vulkan implementation` is the driver's own words; the
  build sets `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1`, which Mesa requires for a non-conformance-listed
  BVNC. This is not a CTS pass, and no CTS was run.
- ~2.91 GB/s vs the vendor's 3.36 GB/s on this kernel: one kernel, one size, 10 dispatches. It is
  a first number, not a benchmark suite.
- The kernel driver is the v6.8 source adapted to this 6.6 BSP kernel, with local bring-up glue
  for the vendor device tree (`clk_bus`, `reset_bus`, `core` ← `clk`). Upstream-quality support
  still means a device-tree overlay and real clock/power-domain modelling.

### Operating facts worth keeping

- The desktop is `display-manager.service` (a system unit); stopping it frees the vendor module in
  under a second (188 refs → 0). Restarting it brings X, KWin, plasmashell and picom back in about
  ten seconds — **no reboot needed**, which retires the "reboot to recover" flow used in stage 3.
- This session's own harness runs in `user@1000.service/app.slice/dsh-web.service`, not in the
  graphical session, so it survives the swap. Verified rather than assumed.
- Two side effects of a swap, both harmless and both now known: `powervr` logs
  `Unbalanced pm_runtime_enable!` when loaded right after the vendor module is unloaded (the
  platform device is left runtime-PM-enabled by the vendor driver; it does not appear on a boot
  where only one driver was ever loaded), and `kmsconvt@tty1.service` takes a SEGV because its
  DRM console disappears underneath it (`systemctl restart kmsconvt@tty1` clears it).

## 11. Stage 4 continued: rendering, the submit cost, and a real bug in our own glue

### Rendering works, and it is not submit-bound

`bench/pvr-vulkan/vkrender.c` draws a full-screen triangle offscreen with a fragment shader the
host can predict pixel for pixel, resolves it, copies it out and compares all 262144 pixels. It was
validated against the vendor ICD first, so a failure on the open driver means the driver, not the
test.

| test | vendor ICD + pvrsrvkm | Mesa pvr + mainline powervr |
|---|---|---|
| compute, 1M elements | 2.497 ms/dispatch, 3.36 GB/s | 2.833 ms/dispatch, **2.96 GB/s** |
| offscreen 512x512, one submit per frame | 0.663 ms/frame, 395.2 Mpix/s | 1.658 ms/frame, **158.1 Mpix/s** |
| offscreen 512x512, 8 frames per submit | — | 1.365 ms/frame, 192.0 Mpix/s |
| offscreen 512x512, 32 frames per submit | — | 1.297 ms/frame, 202.0 Mpix/s |

Compute is within 12% of the vendor stack; graphics is correct but about 2.5x slower. Batching
recovers only ~19%, so **the gap is not per-submit overhead** — it is in the render path itself
(tiling, state setup, or how Mesa's pvr driver sequences passes) rather than in the queue
arbitration that option (b) degraded. That matters for sequencing: option (a), a real multi-ring
`drm_sched` port, is not the lever for this number.

### A bug in the bring-up glue, found by the backtrace it caused

Repeated run/resume cycles produced a kernel warning:

```
CPU: 5 PID: 91212 Comm: pvr-queue Tainted: G W O
 pvr_power_device_resume+0x7c/0x248 [powervr]
 pvr_queue_run_job+0x108/0x340 [powervr]
powervr 1800000.gpu: ks-bringup: no reset_bus (-16)
```

The `clk_bus` / `reset_bus` shim (§10) sat in `pvr_power_device_resume()`, which runs on **every**
runtime-PM resume, so it called `devm_clk_get()` and
`devm_reset_control_get_optional_exclusive()` each time. Two consequences: a devres allocation
leaked per resume, and because the reset is exclusive the second acquisition failed with `-EBUSY`.
The state is now acquired once and cached in `struct pvr_device`
(`ks_bus_clk`, `ks_bus_rst`, `ks_bringup_done`); enabling the clock stays per-resume, because the
mainline driver disables its clocks when it suspends. After the fix the log is one line
(`ks-bringup: clk_bus found, reset_bus found`) and the warning is gone; compute and all three
render batches still pass.

Rebuild recipe, including the part that is easy to get wrong:
```sh
cd /home/radxa/kspike/img
sudo make -C /lib/modules/$(uname -r)/build M=$PWD \
     KBUILD_EXTRA_SYMBOLS=/home/radxa/kspike/mod/Module.symvers modules
```
Without `KBUILD_EXTRA_SYMBOLS` modpost fails on the `drm_gpuvm` symbols that the backport module
provides.

### GL over the open driver: not yet, and the blocker is not the driver

The goal was zink (GL -> Vulkan) over the pvr ICD, headless via `EGL_MESA_platform_surfaceless`,
rendering the same predictable pattern. What is established:

- **Mesa's pvr driver advertises `VK_EXT_robustness2` with `nullDescriptor = true`**
  (`pvr_device.c`), which is what Mesa 25.3's zink requires. The vendor ICD does **not** have it —
  zink refuses it with *"Zink requires the nullDescriptor feature of KHR/EXT robustness2"*. So on
  paper the open driver is the better Vulkan substrate for GL, not the worse one.
- zink does reach the pvr device: the run shows pvr's device being created twice
  (`Core count fetching is unimplemented` + the conformance warning, twice), so device selection
  succeeds.
- EGL then fails: `libEGL warning: egl: failed to create dri2 screen` /
  `DRI2: failed to create screen` -> `eglInitialize failed (0x3001)`, identically with a
  surfaceless-only build and after adding Mesa's DRM platform (`-Dgbm=enabled`), and with the
  system GL stack it fails differently (`did not find extension DRI_Mesa version 1`).

This board has **three Mesa generations** in play: the vendor stack's GL in `/usr/local/lib` (Mesa
24.0.1-based, on the loader path via
`/etc/ld.so.conf.d/00_xserver-xorg-img-bxm.conf`), Debian's 25.0.7, and our 25.3 builds. That is
almost certainly the EGL/DRI confusion. The next step is to pin one coherent set (our own libEGL +
libgallium + `LIBGL_DRIVERS_PATH`, with the vendor `/usr/local/lib` kept out of the search path)
rather than to change anything in the driver.

### Environment the open path now needs (all reproducible, all recorded)

`bison` 3.8.2, `flex` 2.6.4 and `m4` 1.4.19 in `/home/radxa/gltools` (Debian packages unpacked
there, since apt cannot install on this image), with `BISON_PKGDATADIR` and `M4` pointing into it;
`wayland-protocols` 1.44 and `libwayland-dev` headers; `libclc.pc` written by hand; LLVMSPIRVLib
19.1.15 built from source. See `mesa/README.md`.

## 12. GL: the vendor baseline, and exactly where the open path stops

### The vendor GL stack, measured headless (new)

The vendor stack is GLES-only (no desktop GL, as §1 says), but it does run without a display
server, so it can be measured on the same harness:

```
EGL 1.5  vendor=Mesa Project
GL_VENDOR:   Imagination Technologies
GL_RENDERER: PowerVR B-Series BXM-4-64
GL_VERSION:  OpenGL ES 3.2 build 24.2@6603887
512x512, FBO + glReadPixels + glFinish:  1.131 ms/frame, 231.7 Mpix/s, 262144/262144 pixels correct
```

That is the number any GL-over-open-driver result has to be read against, and it is the first
headless GL measurement on this board.

### Where zink over the open driver stops, and what was ruled out

Getting here eliminated four candidate causes, one of which was a real missing dependency:

1. **libudev was not installed**, so Mesa's EGL device enumeration had no backend: the device list
   was empty and the surfaceless probe reported `DRI2: failed to load driver`. Installing
   `libudev-dev` and rebuilding fixed enumeration — it now reports
   `EGL devices: 1 / device[0] render node: /dev/dri/renderD128`.
2. **WSI was compiled out.** pvr's `VK_KHR_swapchain` is gated on `PVR_USE_WSI_PLATFORM`, and our
   first build had `-Dplatforms=` (none). Rebuilt with Wayland WSI (`wayland-protocols` 1.44 +
   `libwayland-dev` installed); the ICD now has it.
3. **Driver loading is fine.** `LIBGL_DRIVERS_PATH` + `MESA_LOADER_DRIVER_OVERRIDE=zink` resolve
   `zink_dri.so`; proof is that the same build against the *vendor* ICD gets as far as zink's own
   check and fails with *"Zink requires the nullDescriptor feature of KHR/EXT robustness2"* — i.e.
   the driver loaded and zink ran.
4. **Device matching is fine.** Mesa's pvr advertises `VK_EXT_physical_device_drm`, so zink can
   pair the DRM render node with the pvr Vulkan device; the run shows the pvr device created (its
   conformance warning appears once per device creation) with no *"failed to choose pdev"*.

What remains is inside Mesa: `driCreateNewScreen3()` returns NULL, which surfaces as
`egl: failed to create dri2 screen` -> `DRI2: failed to create screen`, on **both** the surfaceless
platform and an explicitly selected device platform (`EGL_PLATFORM=device` +
`DRM_RENDER_NODE=/dev/dri/renderD128`, added to the harness and validated against the vendor GL
stack first). This board has three Mesa generations installed — the vendor's 24.0.1-based GL in
`/usr/local/lib` (on the loader path via `/etc/ld.so.conf.d/00_xserver-xorg-img-bxm.conf`),
Debian's 25.0.7, and our 25.3 builds — and the system stack fails differently
(`did not find extension DRI_Mesa version 1`). A coherent GL stack is the next thing to try, not a
driver change.

### What this sets up

The pieces for a **render -> scanout** test (the capability that actually matters for using the
GPU) are all present: the kernel driver uses `drm_gem_shmem`, so PRIME import/export comes from the
shmem helper; Mesa's pvr advertises `VK_KHR_external_memory_fd` and
`VK_EXT_external_memory_dma_buf`; the display side is `sunxi-drm` on `card0`, and the vendor module
does not have to be loaded for that side to work.

## 13. The open stack renders and puts it on the display

Stage 4 proved the open driver computes and renders. This closes the loop that actually matters:
a buffer drawn by the open driver is scanned out by the display controller.

```
GPU: PowerVR B-Series BXM-4-64 MC1 (api 1.2.328), scanout buffer 1920x1080
trying to create an exportable image:
  RGBA8 LINEAR renderable      WORKS (dma-buf fd=5)
scanout image: RGBA8 LINEAR, rowPitch=7680 offset=0 size=8294400 modifier=0x0
1/3 render: PASS (2073600 pixels verified in the scanout image)
2/3 export: PASS (dma-buf fd=6, 8294400 bytes)
   imported into /dev/dri/card0 as GEM handle 1
   display: connector 146 crtc 99 mode 1920x1080@60 (image 1920x1080)
3/3 scanout: fb 163 on crtc 99 - committed, CRTC reports it
```

The path is `/dev/dri/card1` (powervr) for the Vulkan render and the dma-buf, and
`/dev/dri/card0` (sunxi-drm) for `drmPrimeFDToHandle` -> `drmModeAddFB2` -> `drmModeSetCrtc`.
The pattern (two 64-pixel ramps over a constant blue, the same one `vkrender` uses) was on the
panel for six seconds, and it is the open driver's output: no vendor module was loaded at the time.

### What made it work, and what did not

Four things were learned by failing first:

1. **The external-memory capability query is gated on instance extensions.** With an instance that
   enabled nothing, *every* shape reported `exportable=no` and the test refused to start. Adding
   `VK_KHR_external_memory_capabilities` + `VK_KHR_get_physical_device_properties2` at instance
   creation turned all 16 shapes in the grid to `exportable=yes`. The query is worth having, but it
   is not the thing that decides: pvr implements import/export for dma-buf either way, so the test
   now attempts creation, allocation and `vkGetMemoryFdKHR` and reports what actually happens.
2. **pvr renders straight into a LINEAR image** (`RGBA8`, colour attachment + transfer source), so
   no render-then-copy dance was needed. An earlier version assumed it would be, and a probe that
   only asked about `BGRA8`/copy-destination shapes made it look impossible.
3. **Format mapping:** `VK_FORMAT_R8G8B8A8_UNORM` is `DRM_FORMAT_ABGR8888`;
   `VK_FORMAT_B8G8R8A8_UNORM` is `DRM_FORMAT_ARGB8888`.
4. **The panel's active mode is not its preferred mode.** The first run found the connector on
   3840x2160 and refused to scan a 1280x720 buffer; the next run found 1920x1080 on the same
   connector. The tool now re-runs itself at whatever size the display is actually using.

The kernel side needed nothing: the driver's GEM objects come from `drm_gem_shmem`
(`gem_prime_import_sg_table` is wired up), so a dma-buf export is available for free, and
sunxi-drm imported the pvr buffer without complaint.

### Honest scope

- The pixels were verified **in the image that was handed to the display**, and the display device
  accepted and committed it (the CRTC reports our framebuffer id). There is no writeback capture
  yet, so "the panel showed it" rests on that commit plus a human looking at the screen.
- One frame, one commit, six seconds. No page flips, no vsync, no double buffering, no animation -
  that is the next step, and it is what separates "can scan out" from "can drive a display".
- Still compute-and-render only as far as Mesa is concerned: this does not need the compositor, and
  the GL path over the open driver (§12) remains blocked in Mesa's EGL.

## 14. The open stack drives the display, and a Mesa bug fell out of it

### Animated page-flipped presentation

`bench/pvr-vulkan/pvranimate.c` is the compositor's job in miniature: two exportable linear images
on the pvr device, two framebuffers on sunxi-drm, and a loop of render -> fence -> `drmModePageFlip`
-> wait for the flip event.

```
[1920 1080 240] presented 240 frames in 4023.5 ms: 59.6 fps (2 buffers, 0 flip timeouts)
[1920 1080 240] first 6 presented frames, pixel 0: 2 0 2 18 34 50
[1920 1080 240] animation: the pattern advances frame to frame
[1920 1080 240] VERDICT: PASS - the open driver presented animated frames by page flip
[3840 2160 120] presented 120 frames in 4315.1 ms: 27.8 fps (2 buffers, 0 flip timeouts)
[3840 2160 120] VERDICT: PASS
```

**60 fps at 1080p** is the panel's own refresh rate - the loop is not the bottleneck at that size.
At 4K it is 28 fps, so the limit there is render throughput, not presentation. Zero flip timeouts at
both sizes.

### The bug this exposed in Mesa's pvr driver

The animation phase is delivered by a push constant. Pushing two different values produced
**byte-identical frames**, which is not a subtle rendering difference - it is a feature doing
nothing. The cause:

- the generated dispatch table wants `pvr_CmdPushConstants`
  (`build/src/imagination/vulkan/pvr_entrypoints.c`: `.CmdPushConstants = pvr_CmdPushConstants`);
- the driver defines only `pvr_CmdPushConstants2KHR` (the Vulkan 1.4 spelling);
- so core `vkCmdPushConstants` resolves to the generated entrypoint **stub** and silently does
  nothing. Anything written against Vulkan 1.0-1.3 gets no push constants at all, with no error.

`mesa/0003-pvr-implement-core-CmdPushConstants.patch` adds the missing entry point by delegating to
the 2KHR variant. With it applied, push constants take effect: the same test then shows
`phase 32 pushed twice -> content matches (pixel 0 = 129, want 129)` and the presented frames
advance `2 0 2 18 34 50`.

A second issue remains open, and the same test pins it down: **a pushed value reaches the GPU one
submission late**. Pushing 0 then 16 then 16 gives 2, 2, 66 - each frame renders with the previous
submission's value - and pushing the same value twice is the workaround. The mechanism looks like
the upload being tied to the pipeline's special-buffer setup (`state->push_consts[stage].dev_addr`
is only populated once per command buffer), but that is a hypothesis, not a result.

### Method note

The first version verified frame 0 from inside the presentation loop, reading back the image the
display was simultaneously scanning out. It passed, then failed, then passed again at 4K. That is
not a driver result, it is a race in the test, so the verification moved to a separate submission
after the loop stops, plus a steady-state case whose expected value is unambiguous. Two runs of a
flaky check are not evidence; the fix was to stop racing rather than to re-run until it agreed.

### §12 addendum: two harness bugs found, and where zink over pvr really stops

Chasing the GL failure further produced two genuine fixes - to **our** side, not to Mesa:

1. **`MESA_LOADER_DRIVER_OVERRIDE` is ignored for root.** `loader_get_driver_for_fd()` honours
   the override only `if (__normal_user())`. The swap script runs as root, so zink was never
   selected: the loader fell back to the kernel's DRM name, `powervr`, found no gallium driver for
   it, and `driCreateNewScreen3()` returned NULL with no message at all. The GL phase now runs as
   the desktop user (`runuser -u radxa`), who is in the `render` group. This is the kind of thing
   that looks exactly like a driver bug and is not one.
2. **The GBM backend search path is baked in at configure time.** Loading `dri_gbm.so` failed with
   `cannot open shared object file: No such file or directory (search paths
   /usr/local/lib/aarch64-linux-gnu/gbm)` because the build was configured with the default prefix
   and only installed under `DESTDIR`. `GBM_BACKENDS_PATH` now points at the real location. A
   proper install to the configured prefix would make both this and `LIBGL_DRIVERS_PATH`
   unnecessary - that is the cleaner fix and the next thing to do here.

With the override actually applying and the backend found, the GBM path gets further than the
surfaceless one: `gbm_create_device()` reaches the zink driver and pvr's Vulkan device is created
twice (its conformance warning appears once per device creation), then screen creation still fails.

My first theory was that zink needs `VK_EXT_image_drm_format_modifier` to import the DRM fd
(Mesa's pvr advertises `KHR_external_memory_fd` and `EXT_external_memory_dma_buf` but not the
modifier extension). Reading `zink_screen.c:3593` does not support that: only the dmabuf-modifier
*query* callbacks are gated on it, not fd import. So that theory is recorded here as a dead end.

The next concrete step is instrumentation, not another guess: zink creates a pvr device and then
fails without logging anything, so the failure is between device selection and screen completion.

## 15. GL works on the open driver: the blocker was Mesa's pvr missing dynamic rendering

The GL failure that resisted several rounds of investigation has one cause, and it is not a
harness problem, not the kernel driver and not zink doing something odd:

**zink requires `VK_KHR_dynamic_rendering` (or a Vulkan 1.3 device). Mesa's pvr driver in 25.0-25.3
does not implement it.** `get_api_version()` returns 1.2 and the extension table has no
`KHR_dynamic_rendering`, so `zink_get_physical_device_info()` returns false, `driCreateNewScreen3()`
returns NULL, and EGL reports `egl: failed to create dri2 screen`. The reason was invisible because
that particular rejection is reported with `debug_printf()`, which is compiled out of release
builds - every other zink failure logs through `mesa_loge` and would have been visible.

Mesa **main** has it: `pvr_physical_device.c` carries `.KHR_dynamic_rendering = true` alongside
`.robustness2 = true, .nullDescriptor = true`, claims 1.2 + the extension, and its vendored
`pvr_drm.h` is byte-identical to this kernel's UAPI header. Building main's pvr ICD needed no
patches at all: main's enumeration is capability-based (DRM driver name plus dumb-buffer and PRIME
caps), so the `img,gpu` device-tree entry that 25.x requires does not exist there.

```
GL_RENDERER: zink Vulkan 1.2(PowerVR B-Series BXM-4-64 MC1 (IMAGINATION_OPEN_SOURCE_MESA))
GL_VERSION:  OpenGL ES 2.0 Mesa 25.3.0
20 frame(s) in 109.015 ms (5.451 ms/frame, 48.1 Mpix/s)
RESULT: PASS - 262144/262144 pixels correct
```

So the full open stack now goes: **kernel `powervr` -> Mesa pvr (Vulkan) -> zink -> GLES -> FBO,
with every pixel verified**, and it runs from a plain `stage4-mainline-vulkan.sh` with no
environment overrides. The `GL_RENDERER` string naming `IMAGINATION_OPEN_SOURCE_MESA` is the open
driver's own driver id, not the vendor blob's.

### Honest numbers and limits

- **48.1 Mpix/s against the vendor stack's 231.7 Mpix/s** for the same 512x512 offscreen pattern:
  GL through zink is ~4.8x slower here. Some of that is zink's nature, some is the render-path gap
  measured in §14 (158 vs 395 Mpix/s), and the ES2 context this ends up using is not the vendor's
  ES 3.2.
- **Only a GLES 2 context was accepted.** Requests for a GLES 3 context come back
  `EGL_BAD_CONFIG` (0x3005) with Mesa's own note `context api is 0x40 while config supports 0xd` -
  Mesa appears to evaluate the ES3 request against the OpenGL renderable bit. That is unexplained
  and is the next thing to look at; the test falls back to a GLES 2 shader path (vertex buffer plus
  `gl_FragColor`) which produces the identical pattern.
- This is offscreen GL. There is still no surface/swapchain path that a compositor could use on the
  open driver, so the desktop keeps running the vendor stack.

### Method note: two runs of this investigation were invalid

A compile error (`eglGetCurrentAPI` is not a function; it is `eglQueryAPI`) left a **stale
`glheadless` binary** in place, and the build script's `command || fallback` pattern hid it, so two
consecutive "results" were produced by the previous binary and were meaningless. The build script
now distinguishes a link failure from a compile failure and aborts loudly on the latter. The same
class of mistake - also mine, not Mesa's - was a missing `eglMakeCurrent()` that had been dropped by
an earlier edit, which made every GL call return NULL.

### §15 addendum: why the swaps started failing, and what the GL level is

Two more of the same kind of problem - mine, in the harness - surfaced while re-running:

1. **`kmsconvt@tty1` was holding the GPU.** The tty1 console renders through the vendor GL stack,
   and stopping it released **154 of 172** `pvrsrvkm` references (measured directly). Once it was
   running, stopping the desktop no longer dropped the module to zero and the swap aborted safely
   ("something still holds the vendor module") instead of unloading. The irony is that this was
   self-inflicted: kmscon had SEGV'd during an earlier swap, and restarting it to tidy up is what
   broke the following runs. The swap script now stops it for the duration and restarts it in the
   restore path.
2. **A degraded session poisoned the next swap.** With X up but no window manager, stopping
   `display-manager` does not free everything. The script now repairs an unhealthy session *before*
   taking the GPU, and re-checks afterwards (one `display-manager` restart recovered the desktop in
   the run where the autologin session did not come up).

With those fixed, one `stage4-mainline-vulkan.sh` run passes every phase on the open stack:

| phase | result |
|---|---|
| compute, 1M elements | PASS |
| offscreen render, BATCH 1/8/32 | PASS (262144/262144 pixels each) |
| render -> dma-buf -> sunxi-drm scanout | PASS (fb 162 committed on crtc 99) |
| page-flipped presentation 1080p / 4K | PASS (52.7 fps / 27.8 fps, 0 flip timeouts) |
| zink GL over pvr | PASS (262144/262144 pixels, 48.6 Mpix/s) |

**GL level: ES 2.0, not ES 3.x.** The EGL configs this path offers advertise `RenderableType =
0xd` (`EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENVG_BIT`) and not `0x40`
(`EGL_OPENGL_ES3_BIT`), so an ES3 context request is refused with `EGL_BAD_CONFIG`. Mesa only sets
`disp->ClientAPIs |= EGL_OPENGL_ES3_BIT_KHR` when the driver's config carries `__DRI_API_GLES3`
(`egl_dri2.c:626`), so the question is why zink's configs do not - that is the next thing to look
at, and it is a quality gap rather than a blocker, since ES2 renders correctly here.

### §15 addendum 2: the GL level is ES 3.2, matching the vendor stack

The ES3 gap is closed, and it was one variable. Mesa derives the EGL config's ES3 bit from
`screen->max_gl_es2_version >= 30` (`dri_util.c:178`), and that value comes from
`st_api_query_versions()` -> `get_version(fscreen->screen, options, API_OPENGLES2)`. With zink over
pvr that came out below 30, so the configs advertised `RenderableType = 0xd`
(`ES | ES2 | OpenGL`) and ES3 context requests were refused with `EGL_BAD_CONFIG`.

`MESA_GLES_VERSION_OVERRIDE=3.2` sets that version through
`_mesa_override_gl_version_contextless()` (`dri_util.c:159`), the ES3 bit appears in the config, and
the ES3 context is accepted:

```
GLES 3: config found (surface=window alpha=8)
  context GLES 3 (client version)  -> OK
GL_RENDERER: zink Vulkan 1.2(PowerVR B-Series BXM-4-64 MC1 (IMAGINATION_OPEN_SOURCE_MESA))
GL_VERSION:  OpenGL ES 3.2 Mesa 25.3.0
RESULT: PASS - 262144/262144 pixels correct
```

This is not a fake capability: zink really does implement ES 3.2, and the override only supplies the
version the DRI screen failed to derive. The test keeps a GLES 2 shader path (vertex buffer plus
`gl_FragColor`) for the case where only an ES2 config is offered.

**Like for like with the vendor stack**, same 512x512 offscreen pattern, both at GLES 3.2:

| stack | ms/frame | Mpix/s |
|---|---|---|
| vendor (libGLESv2_PVR_MESA, DDK 24.2) | 1.131 | 231.7 |
| open (zink -> Mesa pvr -> powervr) | 5.501 | 47.7 |

So GL on the open driver works at the same API level as the vendor stack and is **~4.9x slower**.
That gap lines up with the render-path measurement in §14 (158 vs 395 Mpix/s) plus zink's own
overhead; it is a performance question now, not a correctness or capability one.

## 16. Performance: where the time actually goes, and two real wins

The open stack works (§15), so the remaining question is speed. The harness now reports a
per-stage split (record / submit / GPU wait / flip wait) and can vary the workload, which turned a
vague "it is slower" into attributable numbers. **All measurements below are on the same board, same
boot, same workload, with the vendor stack (libVK_IMG 24.2 + pvrsrvkm) as the reference.**

### 16.1 Two presentation wins

Presentation was serialising every frame behind its own flip: at 1080p the GPU work was only 6.6 ms
against a 16.7 ms vblank, yet the loop spent 10 ms waiting and lost ~11% of vblanks.

| case | before | after | change |
|---|---|---|---|
| 1080p page-flipped animation | 53.5 fps | **59.6 fps** | +11%, now at panel refresh |
| 4K page-flipped animation | 27.8 fps | **40.0 fps** | **+44%** |

Two changes did it: three buffers with the wait moved to "this buffer's own flip" (so rendering
runs back to back instead of render->flip->wait), and issuing each flip as soon as its frame is
rendered rather than gating on the previous flip's event, using a retry on `EBUSY` to align to the
next vblank. The 4K case gains most because its 20.6 ms of GPU work now overlaps the vblank instead
of adding to it. Both runs report 0 flip timeouts.

### 16.2 What is still slower, and exactly where

Per-frame split, 1024x1024, single full-screen triangle plus an image-to-buffer copy:

| | record | submit | GPU wait |
|---|---|---|---|
| vendor | 0.174 ms | 0.190 ms | 1.706 ms |
| open (25.3 / main) | 0.859 / 0.971 ms | 0.270 / 0.355 ms | 3.603 / 3.451 ms |

Splitting the workload (`MODE=render` vs `MODE=copy`) shows the gap is **the draw, not the copy**:

| GPU ms/frame | vendor | open | ratio |
|---|---|---|---|
| 512x512 render | 0.397 | 0.883 | 2.2x |
| 1024x1024 render | 0.772 | 2.555 | **3.3x** |
| 1024x1024 copy | 0.728 | 1.023 | 1.4x |

And compute is close (2.833 ms vs 2.497 ms for 1M elements, §14), so this is specific to the
graphics path.

### 16.3 The mechanism: per-frame work proportional to the surface, not to what is drawn

Varying one thing at a time, all at 1024x1024:

| variable | vendor GPU | open GPU | reading |
|---|---|---|---|
| full render area | 0.731 ms | 2.604 ms | baseline |
| quarter render area (16x fewer pixels drawn) | 0.596 ms | 2.542 ms | open is **insensitive to coverage** |
| `LOAD_OP_LOAD` instead of `CLEAR` | 1.554 ms | 4.439 ms | not the clear (and worse for open) |
| `LOAD_OP_DONT_CARE` | 1.598 ms | 3.420 ms | not the clear |
| `STORE_OP_DONT_CARE` | 0.547 ms | 2.506 ms | not the store |

So the open driver's per-draw cost follows the *surface* size but ignores how much of it is drawn,
and ignores the attachment load and store operations. It is also not:

- **the GPU clock** - both stacks run `gpu0`/`pll-gpu` at 1,104,000,000 Hz under load (sampled from
  `clk_summary` during the runs);
- **runtime power management** - pinning the GPU awake (`power/control=on`) changes nothing
  (53.3 vs 52.8 fps, identical stage timings), and the suspended time seen per run is process-start
  firmware boot, not per-frame;
- **submit overhead** - BATCH=8 recovers only ~11-20%, so it is not per-submission cost;
- **the copy path** - 1.4x, while the draw is 2.2-3.3x.

That leaves internal per-frame work over the whole surface in the graphics job path (tile buffer /
parameter buffer handling sized by the surface), which is a Mesa pvr matter rather than something
the harness can configure away. For a compositor the practical consequence is that the cost is per
frame *per surface*, so it does not amortise with frame size - which is exactly why small frames
look disproportionately slow on this driver.

### 16.4 Harness bugs found while measuring (again mine, not the driver's)

- The animation phase's `grep` filter **hid a fatal error**: 1080p was dying with `EBUSY` from
  triple buffering and the log showed nothing. Filters now include `fail|busy|error`.
- The perf phase referenced `$GL_ICD` before it was assigned; with `set -u` the subshell aborted
  silently into `/dev/null`. The ICD resolution now happens before any phase uses it.
- `MODE=render`/`copy` returned before printing timings, because the early-exit for
  "cannot verify" was placed above the report. Timings now print first.
- **The swap left a dead desktop**: the open driver's remove path trips a warning in
  `pvr_context_device_fini` (`pvr_remove`), and afterwards the vendor module can load *without*
  creating `/dev/dri/card1`, so X cannot start at all. The restore path now checks for `card1`,
  reloads the vendor module cleanly if it is missing, and retries X once after a clean reload - no
  reboot needed (recovering this way is how it was diagnosed).

### 16.5 The GL gap is a readback cost, not a submission cost

Splitting the GL frame the same way (512x512, 30 frames, `PVR_TIMING=1`):

| stage, ms/frame | vendor (libGLESv2_PVR_MESA) | open (zink -> pvr) |
|---|---|---|
| GL calls (clear + draw) | 0.116 | **0.064** |
| `glReadPixels` | 0.852 | **5.516** |
| `glFinish` | 0.004 | 0.039 |
| total | 0.972 | 5.620 |

**zink's submission path is faster than the vendor's** (0.064 ms against 0.116 ms per frame of GL
calls). The entire 5x difference in this benchmark is `glReadPixels`, i.e. the image-to-host
readback, which is not something a game, a compositor or any normal renderer does per frame. The
"GL is 5x slower" statement in §15 is therefore true of *this test* and misleading about real
workloads; the parts of GL that a real workload uses are competitive.

Readback scaling, which separates a fixed cost from bandwidth:

| size | vendor | open |
|---|---|---|
| 128x128 (64 KB) | 0.702 ms | 3.038 ms |
| 256x256 (256 KB) | 0.755 ms | 3.369 ms |
| 512x512 (1 MB) | 1.424 ms | 5.287 ms |

So the open path pays roughly **2.9 ms fixed plus 2.3 ms/MB**, against the vendor's ~0.65 ms plus
~0.77 ms/MB - a large per-readback cost that looks like a sync/flush or staging allocation rather
than bandwidth. That is the next concrete thing to look at on the driver side, and it is what
zink's `glReadPixels` (and anything else that needs pixels on the host) is paying for.

Also tried, for completeness, on the GL path: `ZINK_DESCRIPTOR_MODE=cached` is *worse* (6.371 vs
5.895 ms/frame), `MESA_GLTHREAD=true` is slightly better (5.507) and the combination is neutral.

### 16.6 Summary of the performance position

| workload | vendor | open | ratio |
|---|---|---|---|
| page-flipped 1080p animation | - | **59.6 fps** | at panel refresh |
| page-flipped 4K animation | - | **40.0 fps** (was 27.8) | +44% this session |
| compute, 1M elements | 2.497 ms | 2.833 ms | 1.13x |
| Vulkan draw, 512 / 1024 | 0.397 / 0.772 ms | 0.883 / 2.555 ms | 2.2x / 3.3x |
| Vulkan image->buffer copy, 1024 | 0.728 ms | 1.023 ms | 1.4x |
| GL submission (GL calls) | 0.116 ms | 0.064 ms | **0.55x (faster)** |
| GL readback, 512 | 0.852 ms | 5.516 ms | 6.5x |

What remains is two driver-internal items - the graphics job path costing time proportional to the
surface regardless of what is drawn (§16.3), and the fixed per-readback cost (§16.5). Neither is
reachable from the harness or from a configuration knob: the levers a test can pull (render area,
load/store ops, batching, tiling, queue priority, power state, DVFS) have all been tried and
measured here.

### 16.7 Why the desktop kept dying after a swap

Three separate causes, all found by reading kwin's own log rather than guessing:

1. **SDDM respawns X during the swap window.** A KDE session started *while the open driver was
   bound*, so kwin came up rendering through **zink on pvr** instead of the vendor GL, failed
   (`Qt platform plugin "xcb"` then a broken X connection) and left the desktop dead. The swap now
   waits for X and kwin to be gone, then clears the session (`pkill -x`, never `pkill -f` - a
   `-f` pattern matches the script's own command line and killed a run that way once).
2. **The open driver's remove path trips `pvr_context_device_fini` (`pvr_remove`)**, and after that
   the vendor module can load *without* creating `/dev/dri/card1`; X then cannot start at all. The
   restore path detects the missing node, reloads the vendor module cleanly and retries - no reboot
   (that warning is worth its own look: it is in our adapted driver, not in the harness).
3. **kmscon on tty1 holds most of the GPU references** (§15 addendum), so it is stopped for the
   duration of a swap.

The useful side effect of (1): a KDE session on the open stack is *nearly* reachable - kwin got as
far as creating a zink screen on pvr and printing its warning before failing. That is a much
shorter path to a real desktop on the open driver than it looked.

## 17. The reboots: our display tools pointed the CRTC at a freed framebuffer

Both forced reboots had the same cause, and it was in the benchmark tools, not in Mesa, the
kernel driver or the desktop.

### What happened

`pvrscanout` and `pvranimate` both finished with:

```c
if (old && old->buffer_id)
    drmModeSetCrtc(dfd, old->crtc_id, old->buffer_id, old->x, old->y, &conn->connector_id, 1, &old->mode);
```

`old` is the CRTC state read at startup - and the swap stops the desktop *before* these tools run, so
`old->buffer_id` is X's framebuffer, whose memory was freed when X exited. Pointing the CRTC at it
makes the display engine scan unmapped memory, which faults on every scan:

```
iommu_master de0_iommu: Runtime PM usage count underflow!
L1 PageTable Invalid
0x0x00000000fc000000 is not mapped!
Bug is in DE0 module, invalid address: 0xfc000000, data:0x0, id:0x4
WARNING: CPU: 0 PID: 0 at bsp/drivers/iommu/sunxi-iommu-v2.c:405 sunxi_iommu_irq
```

The fault address stays constant because the engine keeps re-reading the same dead buffer. A second
path had the same defect: `pvrscanout` re-execs itself at panel size with every fd `O_CLOEXEC`, so
the exec freed the framebuffers while the CRTC was still scanning them.

### Why it was a reboot and not a hang

The storm (26,424 fault lines in one hour, 17,692 in the next, 786 in the last) saturates CPU 0 in
the IRQ handler and floods the log - journald reported `Missed 16209 kernel messages`. This board
runs `watchdog-pet`, a deliberately **health-gated** petter: a normal-priority canary must complete
a trivial operation, and after 8 consecutive failures (~96 s wedged) it *withholds* pets on purpose so
the 16 s `sunxi-wdt` hardware watchdog resets the box instead of leaving it hung. Timing fits exactly
(storm from ~13:36:5x, canary failing from ~13:38:00, withhold ~13:39:36, reset ~16 s later). That is
also why there is no shutdown sequence and no panic in the log - it was a hardware reset, by design.

Ruled out with checks, not assumptions: a clean `reboot` (no systemd stop sequence, and the petter
disarms on SIGTERM - it never got the signal), `panic_on_warn` (0, so the WARNINGs cannot panic),
OOM/earlyoom (3.3-4.6 GB available, no kills), kernel stalls or panics (kernel-only search: 0 lines).
And a control run - stopping and starting the desktop session alone, with no module swap and no
benchmark - produced **0 faults**, which is what pinned this on the tools rather than on X.

### The fix

* `release_display()`: switch the CRTC **off** (`drmModeSetCrtc` with no framebuffer), then *confirm*
  it is off by polling `drmModeGetCrtc` until `buffer_id == 0`, then let three vblanks pass before any
  fd can close. Idempotent, and used by `atexit`, by the SIGINT/SIGTERM handlers, at the end of the
  run, and before `pvrscanout`'s self-rerun `exec`.
* Never restore the old CRTC state: a framebuffer that belonged to a stopped desktop must not be put
  back on screen.
* The harness now counts faults per phase and trips if a phase adds more than 20, so a regression
  aborts the run instead of storming.

### Verification

`display-fix-test.sh` runs the shortest possible display work with a tripwire after every step and an
armed restore:

```
step 1 faults: 0        (scanout, with the self-rerun exec)
step 2 faults: 0        (60-frame animation at 1080p)
step 3 faults: 0        (two 4K animations back to back)
RESULT: faults scanout=0 animation=0 repeated=0 (limit 20 each)
PASS - the display hand-back leaves no faults behind
desktop: X=1 kwin=1 plasmashell=1 ; idle fault rate after restore (10s): 0
```

Where the same work previously added 128+ fault lines per run - and thousands when the restore failed
and nothing re-modeset the display.

## 18. Readback: the open driver had only a write-combined memory type

Chasing the GL readback cost (§16.5, 6.5x the vendor) found a concrete driver gap.

### The gap, measured

`memtypes` (new tool) prints every memory type a driver offers and times the two halves of a
readback. Vendor driver, 1 MB:

| memory type | flags | gpu copy + fence | cpu read | cpu write |
|---|---|---|---|---|
| type 2 | DEVICE_LOCAL HOST_VISIBLE HOST_COHERENT | 0.385 ms | **2.827 ms (354 MB/s)** | 0.191 ms |
| type 3 | DEVICE_LOCAL HOST_VISIBLE HOST_CACHED | 0.392 ms | **0.151 ms (6643 MB/s)** | 0.048 ms |

Mesa's pvr offered exactly **one** type, `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`, and it
behaved like the vendor's slow one: 3.031 ms / 330 MB/s. The tell is that CPU *writes* are fast
(0.19 ms) while reads are slow - that is a **write-combined** mapping.

Where it comes from: the kernel maps PowerVR BOs write-combined unless asked otherwise
(`drm_gem_shmem`'s `map_wc`), and `pvr_gem.c` clears it only for `PVR_BO_CPU_CACHED` - a
**kernel-only flag at bit 63** that userspace cannot pass, because the UAPI reserves bits 3..63 and
the ioctl validates against `DRM_PVR_BO_FLAGS_MASK`.

### The fix

* **Kernel/UAPI**: add `DRM_PVR_BO_CPU_CACHED` (`_BITULL(3)`, previously reserved) and accept it, so
  userspace can ask for a cacheable mapping.
* **Mesa**: a second memory type (`DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED`), a
  `PVR_WINSYS_BO_FLAG_CPU_CACHED` winsys flag mapped to the new DRM flag, and the BO flags now
  derived from the memory type's `propertyFlags` - which is exactly what the **FIXME in
  `pvr_device.c` asks for** ("Need to determine the flags based on
  `memoryTypes[...].propertyFlags`").

Result with the same tool, open driver:

| memory type | cpu read |
|---|---|
| type 0 (write-combined, as before) | 3.019 ms (331 MB/s) |
| type 1 (new, cacheable) | **0.381 ms (2624 MB/s)** - 7.9x faster |

The flag is deliberately advertised as `HOST_COHERENT` as well as `HOST_CACHED`, because that is
what Mesa's zink looks for when classifying its cached staging heap (`vk_domain_from_heap`); with
only `HOST_CACHED` zink silently falls back to the write-combined type and nothing improves.

**Honest caveat**: the sound model for a cacheable, non-coherent mapping is cached-but-not-coherent
plus working `vkFlush`/`vkInvalidateMappedMemoryRanges`, and pvr's are still no-ops. The type is
therefore verified by pixel-exact tests rather than assumed: compute (1,048,576/1,048,576 elements),
render (262,144/262,144 pixels) and 60 frames of zink GL (262,144/262,144 pixels each) all pass with
the cached type in use. Implementing real flush/invalidate is the follow-up that would make it
sound for mappings held across frames.

### What it bought, and what it did not

GL readback (`glReadPixels` of a 512x512 frame, zink): **5.5 ms -> 4.48 ms**, with submission still
at 0.03 ms. That is an 18% improvement, not the 7.9x the memory numbers suggest - so most of the
readback path's cost is not the CPU read of the staging buffer. Since the image-to-buffer copy was
measured at only 1.4x the vendor's (§16.2), the remaining suspect is the synchronisation around the
copy on the readback path, which is the next thing to measure (an image-to-host-buffer variant of
`memtypes` would separate copy, sync and CPU read).

### 18.1 The readback path is now faster than the vendor's

`memtypes` grew a readback section that mirrors what a GL driver does - copy a 512x512 render target
into a host-visible buffer, wait for the fence, read it on the CPU - and reports the three parts
separately. Same board, same boot, 1 MB:

| stack / memory type | record | fence wait | cpu read | **total** |
|---|---|---|---|---|
| open, write-combined (before) | 0.008 | 0.339 | 3.000 | 3.347 ms |
| **open, cached (after the fix)** | 0.010 | 0.340 | 0.380 | **0.729 ms** |
| vendor, coherent (type 2) | 0.103 | 0.442 | 3.035 | 3.580 ms |
| vendor, cached (type 3) | 0.118 | 0.440 | 0.276 | 0.834 ms |

So on the readback path the open driver now beats the vendor stack: **0.729 ms against 0.834 ms**
(13% faster), and 4.9x faster than the vendor's coherent type. The win comes from two things - the
cached type the fix adds, and the fact that Mesa records this copy far more cheaply than the vendor
(0.010 ms against 0.118 ms), which more than covers the vendor's slightly faster CPU read.

The GL number is the remaining puzzle: `glReadPixels` moved only 5.5 -> 4.48 ms even though the
underlying readback is now 0.73 ms, so most of zink's readback cost is in zink's own path (staging
choice, an extra copy, or per-call synchronisation) rather than in the driver. That is the next thing
to instrument, and it is a zink/GL question rather than a kernel-driver one.

### 18.2 The draw gap is per-pass setup, not fill - and one more gap: allocation

Following §16.3 (draw cost follows the surface, not what is drawn), two more experiments narrowed it:

**An empty render pass is nearly free on both drivers.** `MODE=empty` (same render pass, nothing
loaded or stored, no draw) at 1024x1024:

| | empty pass | pass + one triangle | difference |
|---|---|---|---|
| vendor | 0.000 ms | 0.690 ms | 0.69 ms |
| open | 0.394 ms | 2.571 ms | **2.18 ms** |

So the vendor's whole cost is the draw, and the open driver's draw costs **3.2x** more than the
vendor's, while its pass overhead (0.39 ms) is a smaller separate matter.

**The excess is not bytes per pixel either.** Rendering into R8 (1 B/px), R16 (2 B/px) and RGBA8
(4 B/px) at 1024x1024:

| format | vendor | open |
|---|---|---|
| R8 | 0.526 ms | 1.798 ms |
| R16 | 0.592 ms | 1.788 ms |
| RGBA8 | 0.618 ms | 2.692 ms |

The vendor barely moves (0.53 -> 0.62 ms); the open driver is flat between R8 and R16 and then jumps
for the 4-byte format. Combined with §16.3 (insensitive to render area, load op and store op), the
excess is per-draw work proportional to the surface's *pixels* but independent of coverage, format
and attachment behaviour - which is what per-tile bookkeeping over the whole tile grid looks like,
and Mesa's pvr does exactly that: `pvr_rt_get_isp_region_size()` sizes the ISP region headers from
`tiles_per_mtile * mtiles` (`pvr_arch_job_render.c:166-180`, used at :609), i.e. from the full
surface's tile grid rather than from the tiles that will actually be processed. That is the
mechanism to attack; it is real driver work (initialise only the regions that will be used, or reuse
the initialisation when the target is unchanged) and is not something the harness can configure away.

**Allocation is 2.2x the vendor's.** `memtypes` now times allocate+bind+free of 1 MiB host-visible
buffers:

| | allocate + bind |
|---|---|
| vendor type 2 / 3 | 0.343 / 0.389 ms |
| open type 0 / 1 | 0.862 / 0.735 ms |

Two consequences. First, it is a second, independent gap on the open stack (page allocation and
zeroing in `drm_gem_shmem`, where the vendor DDK keeps its own pool) that a BO cache in the driver
would address - and the open stack allocates several buffers per frame (the ioctl profile in §16.3
showed 5 `VM_MAP` and a BO per submission). Second, it explains part of the zink readback mystery:
`glReadPixels` costs 4.48 ms while the underlying readback is 0.68 ms, and a fresh staging
allocation costs ~0.8 ms, so per-call allocation plus a per-call synchronisation is where the rest
of that 4.48 ms lives.

### 18.3 Refuted: it is not per-frame allocation. Confirmed: it is sized by the surface, not the render area

Two hypotheses tested and settled with measurements rather than reasoning.

**Not per-frame BO allocation.** The driver now counts allocations per command buffer
(`PVR_ALLOC_TRACE=1`), because any buffer created per command buffer is per-frame cost:

```
[alloc] command buffer 1:  31 bo(s), 5156 KiB     <- setup
[alloc] command buffer 2:   2 bo(s),  132 KiB
[alloc] command buffer 50:  2 bo(s),  132 KiB     <- steady state
```

Steady state is 2 BOs / 132 KiB per frame - a few percent of the frame, not the gap. My
"region headers are re-allocated every frame" theory was wrong, and the counter is what said so.

**The per-pass work is sized by the framebuffer extent, not by the render area.** Same number of
drawn pixels (256x256), different surfaces:

| | 1024x1024, quarter render area | 256x256, full | ratio |
|---|---|---|---|
| vendor GPU | 0.568 ms | 0.297 ms | 1.9x |
| open GPU | **2.607 ms** | **0.508 ms** | **5.1x** |
| open record+submit | 1.117 ms | 0.477 ms | 2.3x |

Drawing the same 256x256 of content costs 3.7x more on the open driver when the surface happens to
be 1024x1024. Combined with §16.3 (render area changes nothing) this says the per-pass tile
structures - the region headers, the SPM/EOT state, the tile grid - are derived from the image
extent (`rstate->width/height` come from the framebuffer, and `pvr_rt_mtile_info` is computed from
those), so a partial render pays for the whole surface. The vendor is much less sensitive (1.9x).

That matters for exactly the workload a compositor has: rendering damage rectangles into a
full-size target. Fixing it means sizing those per-pass structures from the render area, which is
not a local change - the datasets are created at framebuffer creation (`pvr_arch_framebuffer.c`),
where no render area exists yet, so they would need a per-render-area variant or an offset-based
scheme. That is the next real piece of driver work, and it is worth more than the remaining
per-tile efficiency difference measured at full extent (2.5 ms against the vendor's 0.69 ms at
1024x1024, §18.2).

### 18.4 Mechanism: the tile grid is the framebuffer's, and empty tiles are walked

The code path behind §18.3, for whoever picks this up:

* At job setup the tiling parameters are built from the **render target dataset**, not from the render
  area: `pvr_arch_rt_mtile_info_init(dev_info, &tiling_info, rt_dataset->width, rt_dataset->height,
  rt_dataset->samples)` (`pvr_arch_job_render.c:1138`). `rt_dataset` is created per framebuffer
  (`pvr_arch_job_render.c:634`, from `pvr_arch_render_target_dataset_create`), and `rstate->width/height`
  come from the framebuffer too (`pvr_arch_framebuffer.c:265-267`). The render area never enters it.
* `CR_ISP_CTL.process_empty_tiles` (`pvr_arch_job_render.c:1145`) is set from
  `job->process_empty_tiles`, which the driver raises when a subpass has load ops that must apply to
  every tile - `"Empty tiles need to be cleared too."` (`pvr_arch_cmd_buffer.c:9446-9452`), and per
  the comment there, the optimised version of this ("selectively enable empty tile processing")
  is explicitly called out as a larger change.
* So a render pass over a 1024x1024 framebuffer walks its whole tile grid whether the render area is
  all of it or a quarter of it, and whether the draw covers everything or one triangle. That is what
  all the measurements show: cost follows the surface (5.1x sensitivity against the vendor's 1.9x),
  ignores the render area, ignores coverage, and barely moves with format or load/store op.

**Fix sketch** (not landed - it needs hardware-semantics care, and the pixel-exact tests are the
safety net): derive the job's tiling from the render area instead of the dataset, and offset the
region-header base by the tile origin so the smaller grid still indexes correctly. The region-header
*size* can stay framebuffer-sized (it is allocated once per framebuffer, and the allocation trace in
§18.3 shows that is not the cost); what needs to shrink is the grid the ISP is told to walk.

Worth doing because it is exactly a compositor's workload - a partial render into a full-size target -
and because the same grid also feeds the clear path, so the win applies to clears as well.

## 19. The draw gap quantified: 5.5x the per-tile cost, nothing else

A size sweep of the same render-only workload (one full-screen triangle, 16x16 tiles) fits a simple
model with two terms - a fixed floor and a per-tile cost - and the whole gap is in the second term:

| size | tiles | vendor GPU | open GPU | ratio |
|---|---|---|---|---|
| 256x256 | 256 | 0.284 ms | 0.516 ms | 1.8x |
| 512x512 | 1,024 | 0.360 ms | 0.903 ms | 2.5x |
| 1024x1024 | 4,096 | 0.645 ms | 2.570 ms | 4.0x |
| 2048x2048 | 16,384 | 1.914 ms | 9.479 ms | 5.0x |

```
open   ~ 0.37 ms + 0.556 us/tile      (predicts 0.94 / 2.65 / 9.5 ms - all match)
vendor ~ 0.26 ms + 0.101 us/tile      (predicts 0.36 / 0.67 / 1.9 ms - all match)
```

The fixed cost is comparable (0.37 against 0.26 ms); the per-16x16-tile cost is **5.5x**. That single
number explains everything else measured: the growing ratio with resolution (the ratio is
`(0.37 + 0.556n) / (0.26 + 0.101n)`, so it climbs towards 5.5x as n grows), the 4K animation gap
(32,400 tiles -> ~18 ms of tile work against the vendor's ~3.3 ms), and why small frames look
better than large ones.

The driver's own tile trace confirms the grid (`PVR_TILE_TRACE=1`):

```
[tile] rt 1024x1024 samples=1 -> tiles 64x64 mtiles 4x4 tiles_per_mtile 16x16 x_tile_max=63 y_tile_max=63
[tile] features: simple_internal_parameter_format=1 gpu_multicore_support=1 process_empty_tiles=1 -> skip_init_hdrs=1
```

### Hypotheses tested and rejected on the way here

* **per-frame BO allocation** - driver counters say 2 BOs / 132 KiB per frame, not a surface-sized
  buffer (§18.3).
* **region-header re-initialisation** - the device takes the `skip_init_hdrs` path.
* **empty-tile clearing** (`process_empty_tiles`) - setting `LOAD_OP_DONT_CARE` in a clean
  render-only run changes nothing: 2.594 ms against 2.635 ms (my earlier load-op test was polluted
  by the copy stage, which is why it read as "no difference" for the wrong reason).
* **the attachment store** - `STORE_OP_DONT_CARE` changes nothing (2.547 against 2.506 ms).
* **render area / coverage / format / clock / runtime PM** - all measured in §16.3 and §18.2.

What is left is the per-tile processing itself: the open driver spends 5.5x what the vendor spends
for each 16x16 tile it walks, at the same clock. That is inside Mesa's pvr tile pipeline and its
control streams, and it is the single largest remaining performance item on this stack - bigger than
the render-area sizing in §18.3, which only affects partial renders.

### 19.1 Clocking was the last easy explanation, and it is not it either

The device tree gives the GPU eight clocks (`clk_parent clk clk_bus clk_800 clk_600 clk_400 clk_300
clk_200`), and the vendor stack has all five fixed-rate ones enabled with `gpu@1800000` as the
consumer. That looked like a plausible 4-5x: if our bring-up only maps `core` and `clk_bus`, the
internal blocks could be running from a much slower source. Sampled under load, with our driver:

```
pll-peri0-800m  rate=800000000  enable=Y consumer=[gpu@1800000]
pll-peri0-600m  rate=600000000  enable=Y consumer=[gpu@1800000]
pll-peri0-400m  rate=400000000  enable=Y consumer=[gpu@1800000]
pll-peri0-300m  rate=300000000  enable=Y consumer=[gpu@1800000]
pll-peri0-200m  rate=200000000  enable=Y consumer=[gpu@1800000]
```

Identical to the vendor, and `gpu0`/`pll-gpu` are at 1,104,000,000 Hz on both sides. So the 5.5x
per-tile cost is not clocking - it is the per-tile work itself, inside Mesa pvr's control streams
and tile pipeline configuration.

### 19.2 Where this leaves the biggest item

Everything reachable from outside the driver has now been tried and measured: render area, coverage,
attachment load and store operations, format and bytes per pixel, batching, clock, runtime PM,
per-frame allocations, region-header initialisation, empty-tile processing, and the peripheral
clocks. The gap survives all of them and sits in one number - **0.556 us per 16x16 tile against the
vendor's 0.101 us** - with a comparable fixed floor.

Closing it needs the PowerVR hardware programming guide (or the upstream maintainers' knowledge of
what the ISP/TPU control stream should look like per tile), because the next step is to change how
tiles are set up rather than to remove work the API asked for. That is a genuinely different kind of
task from everything in this document so far, and it is the honest boundary of what measurement and
the public sources here can resolve.

## 20. Compatibility: what the open driver does not offer, measured against the vendor

Performance is bounded by the hardware's per-tile behaviour (§19), so this section switches to the
other question: what can the open stack not *do* that the vendor stack can. `vkaudit` (new tool)
dumps a driver's capabilities in a diffable form; run once per ICD and diffed, it gives the list.

### 20.1 Fixed: the driver under-reported its own maximum extent by 2x

`pvr_physical_device.c` hard-coded `maxImageDimension1D/2D/Cube`, `maxFramebufferWidth/Height` and
`maxViewportDimensions` to **4096**, while the driver's own helper says otherwise:

```c
static inline uint32_t rogue_get_render_size_max(const struct pvr_device_info *dev_info)
{
   if (PVR_HAS_FEATURE(dev_info, simple_internal_parameter_format))
      if (!PVR_HAS_FEATURE(dev_info, screen_size8K))
         return 4096U;
   return 8192U;
}
```

`bxm-4-64.h` - this board's GPU - has `.has_screen_size8K = true`, so the helper returns **8192**,
which is exactly what the vendor driver reports. The file even computed that value and marked it
`UNUSED`. The result was that applications asking for anything wider than 4096 were refused by a
limit the hardware does not have.

The limits now use the device's real maximum (`max_render_size`), and that matches the vendor
driver exactly:

| limit | vendor | open, before | open, now |
|---|---|---|---|
| maxImageDimension2D | 8192 | 4096 | **8192** |
| maxImageDimensionCube | 8192 | 4096 | **8192** |
| maxFramebufferWidth/Height | 8192/8192 | 4096/4096 | **8192/8192** |
| maxViewportDimensions | 8192 8192 | 4096 4096 | **8192 8192** |

Verified by rendering at sizes that were previously impossible, with every pixel checked:

```
6144x6144: RESULT: PASS - 37748736/37748736 pixels correct
8192x8192: RESULT: PASS - 67108864/67108864 pixels correct
1024x1024 regression: PASS   compute 1M elements regression: PASS
```

### 20.2 The remaining compatibility gap, in priority order

Measured, not guessed - each line is present in the vendor audit and absent from the open one, and
each is explicitly `false`/hard-coded in Mesa's pvr rather than an oversight:

1. **`bufferDeviceAddress`** (and `bufferDeviceAddressCaptureReplay`). DXVK and vkd3d want it, and so
   do a lot of modern engines.
2. **8- and 16-bit storage, `shaderFloat16`, `shaderInt8`** - `storageBuffer8/16BitAccess`,
   `uniformAndStorageBuffer8/16BitAccess`, `storagePushConstant8/16`, `storageInputOutput16`. DXVK
   uses 16-bit types heavily.
3. **`variablePointers` / `variablePointersStorageBuffer`**, **`drawIndirectCount`** - engine-side
   conveniences that some renderers require.
4. **Core features**: `depthClamp`, `occlusionQueryPrecise`,
   `vertexPipelineStoresAndAtomics` - all advertised by the vendor, all `false` in pvr's feature
   table.
5. **API version**: open reports 1.2, vendor 1.3.277. Applications that require 1.3 (or a 1.3 core
   feature) refuse the device; zink works around it because pvr advertises dynamic rendering as an
   extension.
6. **2x MSAA**: `framebufferColorSampleCounts` is `1|4` here, `1|2|4` in the vendor.
7. **Timestamps**: `timestampPeriod = 0.0` and `timestampComputeAndGraphics = false`; the vendor
   reports 512 ns. Anything using timestamp queries gets nothing useful.
8. **X11 WSI**: our ICD build has `platforms=wayland` only, so X11 applications have no surface path
   at all on the open stack (the vendor ICD has xcb/xlib surfaces). This one is a build-configuration
   gap in *our* stack rather than a driver gap, and is the cheapest of the list to close.

Items 1-5 are implementation work in Mesa's pvr - the hardware supports them (the vendor driver
proves it on this very board), but the driver does not implement them yet, so enabling the flags
alone would produce wrong rendering rather than working features. Items 6-8 are smaller: 2x MSAA
needs the two-sample position setup, timestamps need the query path, and X11 needs a rebuild with
the xcb headers present.

### 20.3 Closing the X11 WSI gap (item 8 of the list)

The open ICD we had been testing was configured `-Dplatforms=wayland` only, so an X11 application had
no surface path at all on the open stack - `VK_KHR_xcb_surface` and `VK_KHR_xlib_surface` were simply
not in the driver. That is our build configuration rather than a driver limitation, and it is the
cheapest of the eight gaps to close.

What it took on this board:

* the XCB development packages (`libxcb-randr0-dev`, `libx11-xcb-dev`, `libxcb-dri3-dev`,
  `libxcb-present-dev`, `libxcb-sync-dev`, `libxcb-shm0-dev`, `libxcb-shape0-dev`,
  `libxcb-glx0-dev`, `libxcb-render0-dev`, `libxxf86vm-dev`, `libxshmfence-dev`,
  `libwayland-egl-backend-dev`, and friends). The board's apt is dependency-broken, so they were
  fetched with `apt-get download` and unpacked with `dpkg-deb -x` into `/home/radxa/x11dev/root`,
  the same workflow used for bison earlier in this project;
* `PKG_CONFIG_PATH` pointing at the unpacked `.pc` files **and** `C_INCLUDE_PATH` at the unpacked
  headers - meson finds a dependency through its `.pc` file but that file points at `/usr/include`,
  where the headers are not, so the first build failed on `xcb/dri3.h: No such file or directory`;
* a **separate build directory** (`build-x11`, `-Dplatforms=x11,wayland`) so the working ICD in
  `build/` stays usable while the new one is built and validated.

Status: configuration succeeded and the build is running; verification follows once it links -
`vkaudit` should then list the xcb and xlib surface extensions. One caveat worth stating up front:
end-to-end X11 presentation cannot be tested on this board while X runs on the vendor driver, because
the open driver and the vendor module claim the same GPU device - only one can be bound at a time.
Testing it therefore needs an X server that does not touch the GPU; `Xvfb` has been unpacked into the
same prefix for exactly that purpose (Xephyr, which is installed, needs a parent display and so is no
use while the desktop's X has been stopped for the swap).

### 20.4 X11 WSI verified

The rebuilt ICD (`build-x11`, `-Dplatforms=x11,wayland`) advertises what the application-facing side
needs, which the previous build did not have at all:

```
VK_KHR_surface
VK_KHR_xcb_surface        <- new
VK_KHR_xlib_surface       <- new
VK_KHR_display            <- new
VK_EXT_acquire_drm_display <- new
VK_EXT_direct_mode_display <- new
VK_EXT_headless_surface
VK_KHR_get_surface_capabilities2
```

and the regressions still pass with it: render `1048576/1048576 pixels correct`, compute
`1048576/1048576 elements correct`. The ICD in `build-x11` is now the default the swap script and the
open-run helper use, since it is a superset of the wayland-only build.

So of the eight compatibility gaps in §20.2, two are closed: the under-reported extent limit (§20.1,
verified by 6144² and 8192² renders) and X11 WSI (here). What remains is the feature list in
§20.2 items 1-7, which needs implementation inside Mesa's pvr rather than configuration - the
hardware supports all of it (the vendor driver on this board proves it), and the flagship item is
`bufferDeviceAddress`, which DXVK and vkd3d want.

**Next step for X11, and its one caveat**: end-to-end presentation through an X server has not been
run, because the open driver and the vendor module claim the same GPU device, so X cannot be running
on the vendor driver while the open one is loaded. Doing it needs an X server that never touches the
GPU: `Xvfb` is unpacked into `/home/radxa/x11dev/root` for that, and a small xcb-surface test
(create surface, swapchain, render, present) would close the loop. The capability itself is in place
and advertised.

### 20.5 Second fixed gap: 2x MSAA was implemented but not advertised

The same pattern as §20.1. The driver's sample-count masks were `1|4`, while the implementation
handles 2 samples: `pvr_cr_isp_aa_mode_type()` maps 2 samples to `ROGUE_CR_ISP_AA_MODE_TYPE_AA_2X`,
and the ISP partition sizing has an explicit `isp_samples == 2` path (`pvr_arch_job_common.c:336`).
The vendor driver advertises `1|2|4` on this board, so the mask was simply narrower than the code.

The masks now read `1|2|4` (0x7) for framebuffer colour/depth/stencil/no-attachment and for sampled
image colour/integer/depth/stencil, and `vkrender` gained `SAMPLES=1|2|4` - a multisampled attachment
plus a resolve target, with the existing pixel check doing the verification:

```
SAMPLES=1  RESULT: PASS - 262144/262144 pixels correct
SAMPLES=2  RESULT: PASS - 262144/262144 pixels correct     <- refused before this change
SAMPLES=4  RESULT: PASS - 262144/262144 pixels correct
```

One honest detail: the first run of this test reported 538 pixels "wrong" for both 2x and 4x, with
values at half the expected brightness. That was the *test* being wrong rather than the driver: the
full-screen triangle's hypotenuse passes through the top-right corner, so pixels on that edge are
only partially covered and the resolve correctly averages them toward the background (126 against an
expected 253 is exactly a two-sample average of 0 and 253). The check now verifies interior pixels
exactly and requires edge pixels to be a plausible blend, reporting them separately instead of
counting them as correct - which is why the numbers above are honest rather than the test having
been loosened until it passed.

### 20.6 Compatibility scorecard after this round

| gap | status |
|---|---|
| extent limit under-reported by 2x (8192 vs 4096) | **fixed and reproducible**, 4096²/6144²/8192² PASS with the corrected test (§21.11) |
| X11 WSI (xcb/xlib surfaces) absent | **enabled**, verified as advertised; end-to-end pending an X server that does not use the GPU |
| 2x MSAA implemented but not advertised | **fixed**, verified at 1x/2x/4x |
| `bufferDeviceAddress` (+ capture replay) | base feature **closed in §21**; capture replay still open |
| 8/16-bit storage, `shaderFloat16`, `shaderInt8` | open - implementation work |
| `variablePointers`, `drawIndirectCount` | open - implementation work |
| `depthClamp`, `occlusionQueryPrecise`, `vertexPipelineStoresAndAtomics` | open - implementation work |
| API 1.2 vs the vendor's 1.3 | open - needs the 1.3 core feature set |
| timestamps (`timestampPeriod = 0.0`) | open - the driver has no timestamp query path at all, so the value is honest |

Three of the four gaps that were "the driver can already do this" are now closed.

**Corrected twice (see §21.11):** the extent-limit row above is verified by 6144² and 8192² renders
*and is reproducible*. The intermediate claim that those renders failed intermittently turned out to
be a missing pipeline barrier in the test tool, not a driver fault, so the original row stands. What remains needs
features implemented inside Mesa's pvr, not flags flipped: the hardware supports them (the vendor
driver on this board is the proof), but enabling a feature without implementing it would render
wrongly rather than work.

## 21. Buffer device addresses: the feature was implemented, only the advertisement was missing

`bufferDeviceAddress` was listed in 20.6 as "implementation work". It turned out to be mostly
bookkeeping. Everything an application needs was already in the driver:

- `pvr_GetBufferDeviceAddress()` existed and returned `buffer->dev_addr.addr`;
- `pvr_BindBufferMemory2()` already assigned that address when the buffer was bound;
- buffer descriptors are filled with `PVR_DEV_ADDR_OFFSET(buffer->dev_addr, offset)`
  (`pvr_arch_descriptor_set.c:37`), so the device VA a descriptor hands the hardware *is* the value
  an application would be given;
- the compiler already lowered `PhysicalStorageBuffer` to the global LD/ST that shared memory itself
  uses (`pco_trans_nir.c` `trans_load_global`/`trans_store_global`, address format
  `nir_address_format_2x32bit_global`).

What was missing was the switch. And in this driver the switch is not cosmetic: `vk_spirv_to_nir()`
installs SPIR-V capability masks derived from `vk_physical_device::supported_features`
(`vk_physical_device_spirv_caps_gen.py`), so while `bufferDeviceAddress` was false a
`PhysicalStorageBuffer` shader module was rejected before the compiler ever saw it. The feature flag
was the gate on a working code path.

### 21.1 The change

| file | change |
|---|---|
| `pvr_physical_device.c` | `.KHR_buffer_device_address = true`, `.EXT_buffer_device_address = true`, `.bufferDeviceAddress = true` |
| `pvr_device.c` | publish `vk_buffer::device_address` at bind; `pvr_GetBufferDeviceAddress()` returns it; add the `KHR`/`EXT` entry point aliases |
| `pco_nir.c` | late 64-bit cleanup (see 21.2) |

One flat flag is enough for all three feature structs: Mesa's generated feature code maps
`bufferDeviceAddress` to `VkPhysicalDeviceVulkan12Features`, `VkPhysicalDeviceBufferDeviceAddressFeatures`
and `...FeaturesEXT` together (`vk_physical_device_features_gen.py:104`).

The entry point aliases are not decoration either. The generated dispatch table references
`pvr_GetBufferDeviceAddressKHR`/`EXT` as weak symbols that resolve to the entry point stub when
undefined, so without real definitions `vkGetDeviceProcAddr("vkGetBufferDeviceAddressKHR")` returns
the stub and an application concludes the feature is absent. That is exactly what the pre-change ICD
does when asked to create a device with `VK_KHR_buffer_device_address` enabled: `vkCreateDevice`
returns -7 (`VK_ERROR_EXTENSION_NOT_PRESENT`).

Capture replay stays unsupported, deliberately: it needs the winsys to place an allocation at an
address chosen by the application, which the heap allocator cannot do. It is optional, and
DXVK-Sarek on this board only asks for `VK_KHR_buffer_device_address` (checked with `strings`), not
the capture replay feature.

### 21.2 The crash this uncovered, and the pass-ordering bug behind it

Enabling the feature made the shader compilable but `vkCreateComputePipelines()` **segfaulted**:

```
Unsupported alu instruction: "32    %12 = unpack_64_2x32_split_x %11"
SIGSEGV in trans_alu -> trans_cf_nodes -> pco_trans_nir -> pvr_compute_pipeline_compile
```

`nir_lower_explicit_io()` emits `pack/unpack_64_2x32_split` when it converts a 64-bit address into
the 2x32-bit global format. Those intrinsics are lowered **only** by `nir_opt_algebraic()`
(`nir_opt_algebraic.py:2260`); `nir_lower_pack()` handles only the non-split `pack_64_2x32`. In
`pco_postprocess_nir()` the last `pco_nir_opt()` call passes `algebraic = false`, so its internal
`nir_lower_int64()` is the last pass that can create them and nothing afterwards removes them. They
then reach `trans_alu()`, hit the `default:` case, and `UNREACHABLE("")` is undefined behaviour in a
release build - hence a segfault inside `vkCreateComputePipelines()` instead of an error.

A related pre-existing pathology is now visible in the logs: `pco_nir_opt()` prints
`WARNING! Infinite opt loop!` and bails out of its optimisation loop after 1000 iterations. The
reason is a cycle - `nir_lower_int64()` builds 64-bit values out of `pack/unpack_64_2x32_split`,
while `nir_opt_algebraic()`'s rules rewrite those back into 64-bit `u2u64`/`ishl`/`ushr`. Each pass
undoes the other, so the loop never converges and whichever state it abandons is arbitrary.

`pco_nir.c` gains a conditional late cleanup: if the shader still contains a split intrinsic, run
`nir_lower_int64()` + `nir_opt_algebraic()` until it settles (bounded), so the intrinsics are gone
before translation. The change is **inert for every shader that compiled before it**: such a shader
provably contained no split intrinsic, so the new block never runs for it.

### 21.3 Verification: a matrix, because "nothing was written" has several causes

`bench/pvr-vulkan/bda.c` runs the address through a descriptor (two 32-bit halves packed in the
shader) and through a push constant (`uint64_t`, the way applications do it), against three access
modes: constant store (needs no prior content), load+store, and a global atomic. Every element gets a
distinct expected value, and the buffer under test is never mapped by the CPU - all transfers go
through a staging buffer - so a failure means the address is wrong, not that a cache was stale.

| access | address delivered by descriptor | address delivered by push constant |
|---|---|---|
| constant store | **256/256** | 0/256 |
| load+store | **256/256** | 0/256 |
| `atomicAdd` | **256/256** | 0/256 |

The vendor driver passes all six. So **buffer device addresses work on the open driver**: the
address, the element indexing, the read path, the write path and global atomics through a raw device
address are all correct and match the vendor exactly.

The push-constant column is a separate bug, and `bench/pvr-vulkan/pctest.c` reduces it to something
with no buffer device address in it at all, by pushing byte-identical values into a block declared
two ways:

| push constant block | result |
|---|---|
| `uvec4 v` | **3/3 probes PASS** |
| `uint64_t a, b` | 3/3 probes FAIL - high 32 bits of each value come back as a copy of the low 32 |

```
uint64_t  full 16 bytes at offset 0   FAIL  got 11111111 11111111 33333333 33333333
                                            want 11111111 22222222 33333333 44444444
```

Push constants in general are fine; **64-bit members** are not. The mechanism is the cycle described
in 21.2: `unpack_64_2x32_split_y(a)` is rewritten to `u2u32(ushr(a, 32))`, the 64-bit `ushr` is not
lowered, and `trans_shift()` asserts `bits == 32` - an assert compiled out in a release build - so the
shift is translated as if the operand were a single 32-bit value and the high half is lost. This is
the next item, and it is why `bda` without `--bda-only` still fails.

### 21.4 The extent-limit verification looked like a single observation - resolved in 21.11

20.6 recorded the extent limit fix as "verified by 6144² and 8192² renders". That was one passing run
and it does not reproduce. Regression testing showed the larger sizes failing, so the question was
whether the buffer-device-address work caused it. It did not:

| ICD | 512 | 4096 (x3) | 6144 (x3) | 8192 (x3) |
|---|---|---|---|---|
| `build/`, linked 14:57, **before** this work | PASS | FAIL 3/3 | FAIL 3/3 | FAIL 3/3 |
| `build-x11/`, linked 16:59, after | PASS | FAIL 3/3 | FAIL 3/3 | FAIL 3/3 |

Both drivers fail identically, all-black readbacks (`got 0,0,0,0`), so this predates the change. And
it is not confined to the newly enabled sizes: **4096 was always a legal size** and it fails too.

What is actually going on is worse than a size limit: rendering reliability degrades within a single
boot of the module. In one sweep, 512x512 passed first and then failed after the large renders had
run in the same boot; in another, 4096 passed once and failed 3/3 in a fresh boot. So:

- the extent **limit advertisement** (16.5 -> 8192) is verified by `vkaudit`, which is what that fix
  was about;
- full-frame **correctness** at these sizes is not reproducible, and the earlier claim should not
  have been stated as verified;
- the degradation is the more interesting defect and is still open. It is not a CPU cache coherency
  problem: the cached host-visible type added in section 18 is genuinely coherent here (`memtypes`
  reads it back correctly at 2624 MB/s).

### 21.5 New tools

| tool | what it does |
|---|---|
| `bench/pvr-vulkan/bda.c` | the address-delivery x access-mode matrix; `--bda-only` for the address verdict alone |
| `bench/pvr-vulkan/pctest.c` | minimal `vkCmdPushConstants` probe, `uvec4` vs `uint64_t` block, with a marker field so "dispatch did not run" cannot be mistaken for "values were zero" |
| `bench/pvr-vulkan/regress.sh` | the whole suite in one table, with `verdict` / `clean` / `known` cases so pre-existing failures are shown without silently gating on them |
| `bench/pvr-vulkan/order-probe.sh`, `ab-big.sh` | the ordering and pre-change-ICD probes that established the above |

### 21.6 Scorecard

| gap | status |
|---|---|
| `bufferDeviceAddress` (KHR + EXT, DXVK/vkd3d's requirement) | **implemented, verified and advertised by default** 9/9 - descriptor *and* push-constant delivery, store, load+store, atomic |
| `bufferDeviceAddressCaptureReplay` | open by choice - needs application-directed placement in the winsys |
| compiler segfault on `PhysicalStorageBuffer` shaders | **fixed** (`pco_nir.c`) |
| 64-bit push constants | **fixed** (§21.8) - `pctest` 6/6, `bda` push-constant delivery 9/9 |
| cached host-visible memory type (section 18) | **opt-in** (`PVR_ENABLE_CACHED_MEMORY_TYPE=1`) - the kernel half of the missing cache maintenance is **fixed** (§21.12, corruption down 30x); per-submit flush/invalidate and a truthful coherency flag still to do |
| large render targets / reliability within a boot | **closed** - the withdrawal this row referred to was `vkrender`'s missing barrier, not the driver; 6144² and 8192² PASS reproducibly (§21.11), which restores the §20.6 claim. Row kept only to record that it was wrong here. |
| 8/16-bit storage, `shaderFloat16`, `shaderInt8`, `variablePointers`, `drawIndirectCount` | open - implementation work |
| `depthClamp`, `occlusionQueryPrecise`, `vertexPipelineStoresAndAtomics` | open - implementation work |
| API 1.2 vs the vendor's 1.3 | open - needs the 1.3 core feature set |
| timestamps (`timestampPeriod = 0.0`) | open - no timestamp query path exists, so the value is honest |

### 21.7 The 64-bit push constant bug: diagnosis

**Resolved in 21.8 - this section is kept because the negative results are what identified the
cause, but its conclusion (that the fix was still unknown) is superseded.**

**What the compiler is actually given.** `PCO_DEBUG_PRINT=nir` dumps the final NIR, and for the
`uint64_t` push constant block it is:

```
64    %11 = @load_push_constant (%10 (0x0)) (base=0, range=16, align_mul=256, align_offset=0)
32    %12 = unpack_64_2x32_split_x %11
32    %13 = unpack_64_2x32_split_y %11
64    %15 = @load_push_constant (%14 (0x2)) (base=0, range=16, align_mul=256, align_offset=8)
32    %16 = unpack_64_2x32_split_x %15
32    %17 = unpack_64_2x32_split_y %15
```

Note that a 64-bit push constant is **not** scalarised the way a `uvec4` block is (whose four
dwords become four separate 32-bit loads). The 64-bit load survives to the backend as one
instruction.

**What the backend does with it.** `trans_load_common_store()` reads `chans` from the destination
reference and loads that many consecutive 32-bit registers from `range->start + offset`:

```c
   unsigned chans = pco_ref_get_chans(dest);
   ASSERTED unsigned bits = pco_ref_get_bits(dest);
   assert(bits == 32);                    /* compiled out in a release build */
   ...
   pco_ref src = pco_ref_hwreg_vec(range->start + offset, reg_class, chans);
   return pco_mbyp(&tctx->b, dest, src, .rpt = chans);
```

`pco_ref_nir_def()` maps every NIR value to a reference as
`pco_ref_ssa(def->index, def->bit_size, def->num_components)`, so a 64-bit scalar becomes
**bits = 64, chans = 1** - one channel. Instrumenting the translator confirmed what that produces
downstream:

```
[lc64]   chans=2 ref_bits=64 comps=1        <- the load, after forcing two channels
[unpack] x src_bits=64 src_chans=1          <- what unpack_64_2x32_split_x sees
[unpack] y src_bits=64 src_chans=1          <- and _y sees exactly the same reference
```

Both halves therefore select the same thing, which is the observed symptom: the high 32 bits of
each 64-bit value come back as a copy of the low 32 bits.

**Three attempts, all with byte-identical results.**

| attempt | outcome |
|---|---|
| translate `pack/unpack_64_2x32_split` natively and turn off `lower_pack_64_2x32_split`/`lower_unpack_64_2x32_split` | removed the crash and the `WARNING! Infinite opt loop!` (the `nir_lower_int64`/`nir_opt_algebraic` cycle), but the value was unchanged |
| additionally widen the value in `trans_load_common_store` | unchanged |
| additionally model 64-bit values as 2x32 channels in `pco_ref_nir_def()` (the single point every NIR value is mapped) | unchanged |

All three were reverted. The reason is in the table: a change that cannot be observed to do
anything is not a fix, and the third one is a foundational representation change for every 64-bit
value in every shader - not something to leave in the tree on the strength of "it should work".
The committed state (21.2) was restored and re-verified afterwards: `bda --bda-only` PASS,
`pctest` uvec4 3/3 PASS, `vktest` PASS, `vkrender 512` PASS.

**Where the next attempt should look.** Scalar 32-bit push constant loads are correct at *every*
dword offset - the `uvec4` probes read dwords 0, 1, 2 and 3 each correctly, including offsets 8 and
12. A two-channel read from that same range returns the low dword twice. So the range, the layout
and the offsets are all fine; what is unverified is a multi-channel read of the constant/shared
register file (`pco_ref_hwreg_vec(..., chans)` plus `pco_mbyp(..., .rpt = chans)`). That is a much
narrower question than "64-bit is broken in pco", and it is where the evidence points.

Reproducer: `bench/pvr-vulkan/pctest.c` (no buffer device address involved - the `uint64_t` block
alone fails, with the `uvec4` block passing in the same run).

### 21.8 Resolved: the component was being selected with the wrong instruction

The three attempts in 21.7 each moved the failure without changing it, and that was the clue. There
were **two** independent ways to get the low half twice, and each attempt only addressed one of them:

1. with the split intrinsics lowered away (the committed state), the NIR that reaches the translator
   is `u2u32(ushr(%11, 32))` - confirmed by dumping it - and that 64-bit `ushr` is never lowered,
   because `nir_opt_algebraic()` rebuilds it from `unpack_64_2x32_split_y` while `nir_lower_int64()`
   rebuilds that from the shift. `trans_shift()` asserts `bits == 32`, which is compiled out in a
   release build, so the shift is translated as though its operand were a single 32-bit value;
2. with the intrinsics translated directly (attempts 1 and 3), the component was taken with
   `pco_mov` carrying an element modifier - **and that does not select a channel**. Every other place
   in the driver that takes a component apart uses `pco_comp` (`split_dest_comps` is built on it,
   `pco_trans_nir.c:69`), which is what the attempt should have used.

Attempt 1 fixed (1) and hit (2). Attempt 3 fixed the representation that (2) needed and still hit
(2). The fix is all of it together:

| change | why |
|---|---|
| `pco_ref_nir_def()` maps a 64-bit value to 2x32 channels | it is the one place every NIR value becomes a reference, so producers and consumers agree on the shape |
| `lower_pack_64_2x32_split` / `lower_unpack_64_2x32_split` off | stops `nir_opt_algebraic()` rewriting the intrinsics into 64-bit shifts, which is what made it fight `nir_lower_int64()` and never converge |
| `unpack_64_2x32_split_x/_y` translated with `pco_comp`, `pack_64_2x32_split` with `pco_trans_nir_vec` | takes the halves apart correctly |
| `gather_common_store_data()` counts a 64-bit value as two dwords | the range was under-sized, so the second dword of the last 64-bit push constant fell outside it |

**Verified:** `pctest` 6/6 - `uvec4` and `uint64_t` blocks, identical bytes, all offsets - and `bda`
9/9, which means a buffer device address now works when delivered by a push constant as well as by a
descriptor, for a plain store, a load+store and a global atomic. The `WARNING! Infinite opt loop!`
also disappears: the cycle is gone rather than merely survived.

### 21.9 The catch: it looked like advertising the feature broke GL - resolved in 21.10

Chasing the push constant bug turned up something worse about the feature advertisement itself.
With `VK_KHR_buffer_device_address` advertised, GL rendered through zink comes back wrong.
Isolated in one boot, same build except for the flags:

| `build-x11`, BDA advertised | glheadless 512x20 |
|---|---|
| yes | FAIL - 9504, 11616, 52416, 59040, 62320 of 262144 pixels wrong |
| no | **PASS 262144/262144, twice** |

The trigger is zink changing behaviour once it sees the extension (`have_KHR_buffer_device_address`):

- `zink_resource.c:325` marks every buffer `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`;
- `zink_bo.c:235` puts `VkMemoryAllocateFlagsInfo{VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT}` on every
  memory allocation;
- `zink_context.c:5942` installs `set_global_binding`;
- `zink_resource.c:3596` enables `resource_get_address`.

The pvr driver is not obviously the trigger: it ignores the usage bit (nothing in the driver reads
`VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`) and `pvr_AllocateMemory()` explicitly ignores
`VK_MEMORY_ALLOCATE_FLAGS_INFO` ("We're not yet using any of the flags provided"). Note also that the
zink in this stack comes from a different tree (`mesa-25.3.0/build-gl`), so the next step is to
isolate which of those four paths does it, in that tree.

**This was the wrong conclusion, and 21.10 has the right one: the trigger is not zink and not the
advertisement but the cached memory type. Nothing is gated any more** - the feature is advertised by
default. The reduction above is kept because it is what made the rest of the investigation possible.

**Verified in both configurations, one boot:**

| case | default (BDA off) | `PVR_ENABLE_BUFFER_DEVICE_ADDRESS=1` |
|---|---|---|
| glheadless 512x20 | **PASS 262144/262144** | FAIL (expected - see above) |
| pctest (6 push constant probes) | **PASS** | **PASS** |
| vktest compute | PASS | - |
| vkrender 512, samples=4 | PASS | PASS |
| bda (9 probes) | - (feature absent) | **PASS** |

The push constant fix in 21.8 is independent of the feature and is on in both columns.

### 21.10 Root cause of the GL corruption: the cached memory type could not keep its promise

Advertising the extension was never the fault. Chasing it produced the actual cause, and it is a bug
this project introduced earlier and then trusted because the tests that covered it happened to run in
a configuration where it could not show.

**The measurement that pointed at it.** `PVR_API_TRACE=1` (added to the driver for this) logs the
buffers and allocations an application asks for, so two runs can be diffed. Same test, same 44
buffers, `VK_EXT_buffer_device_address` off versus on:

| | allocations | memory types used |
|---|---|---|
| extension **off** | 5 | 3x type 0 (write-combined), 2x type 1 (cached) |
| extension **on** | 23 | 3x type 0, **20x type 1 (cached)** |

zink stops suballocating once it sees the extension - each buffer gets its own `VkDeviceMemory`, with
`VkMemoryAllocateFlagsInfo{DEVICE_ADDRESS}` on each - so the number of live allocations of the
cached type goes from 2 to 20. Nothing else about the API stream changes: 44 buffers either way.

**Type 1 is the cached host-visible type added in section 18.** Its own comment already said the
sound model would be cached-but-not-coherent plus real flush/invalidate, that
`pvr_Flush/InvalidateMappedMemoryRanges` are no-ops here (the kernel's shmem dma-buf has no
`begin/end_cpu_access`), and that its correctness "is verified by pixel-exact tests rather than
assumed". Those tests passed because with two long-lived cached BOs the missing cache maintenance did
not bite. With twenty freshly mapped ones - CPU writes an upload, GPU reads stale DRAM - draws come
back missing, which is exactly the symptom (`want r=66 g=2 got r=0 g=0`).

**A/B, same build, same boot:**

| configuration | glheadless 512x20 |
|---|---|
| BDA on, cached type **off** | **PASS**, **PASS** |
| BDA on, cached type on | FAIL 59408, FAIL 73952 |
| BDA off, cached type off | PASS |

And a repeatability check first, because the failure magnitude varies (48 to 85664 across runs):
BDA off passed 5/5, BDA on failed 5/5 - a reliable trigger, not noise.

**Two things were ruled out on the way**, and they are why the search went the right direction:

- zink's four `have_KHR_buffer_device_address` paths are *not* the trigger. Instrumenting the running
  zink (`mesa-25.3.0/build-gl`, gated by `ZINK_BDA_MASK`, bit per path: buffer usage bit, allocate
  flags, `set_global_binding`, `resource_get_address`) and disabling **all four** still failed
  (12064 wrong). Also note this stack's zink is built from a different tree than the ICD.
- the driver ignoring `VkMemoryAllocateFlagsInfo` and never reading
  `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` is genuinely harmless: the difference is *where the
  memory came from*, not what the flags said.

Independent confirmation from the vendor driver, whose four types are: `DEVICE_LOCAL`; `DEVICE_LOCAL
LAZILY_ALLOCATED`; `DEVICE_LOCAL HOST_VISIBLE HOST_COHERENT` (write-combined); and `DEVICE_LOCAL
HOST_VISIBLE HOST_CACHED` - **cached without coherent**, which is exactly the sound model. Ours was the
odd one out in claiming both.

**Fix.** The cached type is now opt-in, off by default, under `PVR_ENABLE_CACHED_MEMORY_TYPE=1`, with
the numbers and the reason in the comment. The default configuration advertises only the
write-combined coherent type, like the vendor driver's coherent type. Buffer device addresses are
advertised **by default** again - the gate from 21.9 is gone.

**Final suite, default configuration, nothing set:**

```
regress: 14 passed, 0 failed, 5 known-open
  bda (descriptor + push constant)             PASS
  pctest (vkCmdPushConstants)                  PASS
  glheadless 512x20                            PASS
  known-open: vkrender 512 r8, r16, 4096, 6144, 8192 (all pre-existing)
```

The readback throughput that the cached type bought (~6.6 GB/s against ~330 MB/s, section 18) is
still available behind the variable, and worth revisiting once the kernel can do the cache
maintenance - that is the real fix, and it is on the kernel side, not here.

### 21.11 Follow-up: all five remaining failures were test bugs

The suite had five cases that failed on both drivers and were carried as "known-open, pre-existing".
All five were faults in `bench/pvr-vulkan`, and with them fixed the suite is **20 passed, 0 failed, 0
known-open**.

**r8 and r16: the verifier ignored the format.** `vkrender`'s pixel check read `px + (y*size + x) * 4`
and compared four channels whatever the target format was, so a single-channel target could never
match. The shader writes `fract(coord/64)` as a normalized float, so the expectations are
`round(fract*255)` for `R8` and `round(fract*65535)` for `R16` - and at x = 0 those are 2 and **512**,
not `2 * 257 = 514`, which matters because 514 was what a naive expectation would have asserted. The
verifier is now format-aware (`R8`, `R16`, `RG16` and `RGBA8` each checked at their own width; the
MSAA edge rule applies only to the 4-channel case). Both drivers render all of them correctly:
`rgba8`, `r8`, `r16` and `rg16` at 512 all PASS 262144/262144.

**The large render targets: the tool was missing a pipeline barrier.** `vkrender` ended its render
pass and called `vkCmdCopyImageToBuffer` with nothing in between, relying on the copy's implicit
layout transition to order it after the render pass. A layout transition does not do that - the
application has to synchronize the *data* - so the readback could copy the pre-clear contents, which
are zeros, which is exactly the "all-black" that was being reported as a driver bug.

The evidence is the two drivers disagreeing about the same test:

| `vkrender 2048 1`, vendor driver | result |
|---|---|
| with an explicit render pass -> transfer barrier | **5/5 PASS** |
| without it (the old tool) | **0/5 FAIL** |

A conformant driver is allowed to fail the second case, and it does. On the open driver the
boundary hunting that followed was chasing an intermittent: 4096 gave pass, fail, fail, fail, pass,
and 4032 passed while 3840, 4000 and 4048 failed - not a size threshold at all. With the barrier in
place, **4096, 6144 and 8192 all PASS**, which also makes the extent-limit work of section 20
properly reproducible: the original 6144²/8192² claim was right, the withdrawal in 21.4 was wrong,
and 21.10's "rendering reliability degrades within one boot" was the same missing barrier seen
through a smaller sample.

`vkrender` keeps a `PVR_NO_READBACK_BARRIER=1` gate so the old behaviour stays reproducible for the
A/B, and `PVR_DUMP=1` prints the first bytes the readback delivered - which is what showed the frame
was the clear colour rather than the draw, and pointed at the copy rather than the geometry.

**What this changes about the driver's report card.** The open driver's Vulkan correctness is in
better shape than sections 20-21 said: it passes everything the vendor passes on this suite, plus
buffer device addresses and 64-bit push constants. The failures that were attributed to it at large
sizes were the harness's.

### 21.12 Follow-up: the cached memory type's missing half was in the kernel

21.10 blamed the cached host-visible type on the missing cache maintenance and gated it off. Half of
that was fixable, and the fix is one place in the open `powervr` module.

**The gap.** `pvr_bo-cpu-cached-uapi.patch` taught the kernel to map a BO cacheable when userspace
passes `DRM_PVR_BO_CPU_CACHED`:

```c
	shmem_obj->map_wc = !(flags & (PVR_BO_CPU_CACHED | DRM_PVR_BO_CPU_CACHED));
```

but the maintenance that makes a cacheable mapping usable is keyed on the **kernel-only** flag,
bit 63:

```c
	if (pvr_obj->flags & PVR_BO_CPU_CACHED) {
		if (shmem_obj->sgt)
			dma_sync_sgtable_for_cpu(dev, shmem_obj->sgt, DMA_BIDIRECTIONAL);
	}
```

and userspace cannot set bit 63 (`pvr_drv.c` rejects anything outside `DRM_PVR_BO_FLAGS_MASK`). So a
userspace-created cached BO was mapped cacheable and **never synchronised at all**: the CPU read
stale data after a GPU write, and the GPU could miss CPU writes. That is the whole of the GL
corruption in 21.10, and it was our patch's omission rather than anything zink did.

**The fix**, in `pvr_gem_object_create()`:

```c
	/* A request for a CPU-cacheable mapping must set the kernel-only flag as
	 * well. map_wc below is cleared for either spelling, but the cache
	 * maintenance in pvr_gem_object_vmap()/vunmap() is keyed on the kernel flag,
	 * so without this the object is mapped cacheable and never synchronised.
	 */
	if (flags & DRM_PVR_BO_CPU_CACHED)
		flags |= PVR_BO_CPU_CACHED;
```

`pvr_gem_object_flags_validate()` explicitly allows kernel-only flags, so this is safe, and
`pvr_drv.c` still refuses to accept bit 63 from userspace.

**Measured** - GL through zink, cached type enabled, BDA advertised, three runs:

| | pixels wrong (of 262144) |
|---|---|
| before the kernel fix | 59408, 73952 |
| after | 8032, 528, 2144 |

A thirty-fold improvement is not a fix. What remains is Vulkan's **per-submit** flush/invalidate:
`pvr_FlushMappedMemoryRanges`/`pvr_InvalidateMappedMemoryRanges` are still no-ops in the Mesa
driver, and the type still advertises `HOST_COHERENT` when it is not. Synchronising at map/unmap
covers *map, write, unmap, submit* - the pattern a staging upload uses - but not *map once, then
submit repeatedly*, which is what a buffer pool does. Completing this needs a flush/invalidate UAPI
(a PVR ioctl wrapping `dma_sync_sgtable_for_device/for_cpu` over a range) plus dropping the coherent
claim so applications know to call them.

The kernel maintenance also costs something, which is worth knowing before finishing the job: cached
readback measures **1212 MB/s against 304 MB/s** for the write-combined type, where the same test
measured 2624 MB/s before the sync was added. Still four times faster, and now for a defensible
reason.

The type therefore stays opt-in (`PVR_ENABLE_CACHED_MEMORY_TYPE=1`), and the default configuration
was re-verified on the rebuilt module: **20 passed, 0 failed, 0 known-open**.

## 21.13 Compatibility, re-measured after section 21

Sections 20.6 and 21.6 were both written before the last three fixes landed, and both had drifted in
the same direction: they list as open things that are now closed, and 21.6 repeated a "large render
targets" withdrawal that 21.11 had already retracted. Rather than edit the scorecards from memory,
both drivers were re-audited on 2026-09-23 and the two outputs **diffed**:

```
sudo ./open-run.sh bench/pvr-vulkan/vkaudit > /tmp/audit-open-now.txt     # after the module swap
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json \
  bench/pvr-vulkan/vkaudit > /tmp/audit-vendor-now.txt                    # vendor, no swap needed
```

Raw outputs kept at `bench/pvr-vulkan/audit-2026-09-23-{open,vendor}.txt`.

### One caveat before reading any of it: the loader edits the list

`vkaudit` runs through the system Vulkan loader, and the loader does **not** pass every ICD instance
extension through. `VK_KHR_wayland_surface` is missing from *both* drivers' instance lists as seen
through the loader - which looks like a gap until you ask the ICD directly:

```c
/* dlopen the ICD, vk_icdGetInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties") */
vk_icdprobe libvulkan_powervr_mesa.so
  VK_KHR_wayland_surface advertised by the ICD itself: YES      (21 instance extensions)
```

The vendor's list through the loader has 14 (no wayland either). So the wayland row is a
loader/environment artefact that affects both drivers identically, not a driver gap, and it is the
reason this section does not claim "open driver lacks wayland WSI". Device features and device
extensions are ICD-reported and unaffected by the filter.

### Closed since section 20

| gap | evidence in this audit |
|---|---|
| extent limit under-reported (4096 vs 8192) | `maxImageDimension2D 8192`, `maxFramebufferWidth/Height 8192/8192` on both drivers |
| X11 WSI absent | open ICD advertises `VK_KHR_xcb_surface` **and** `VK_KHR_xlib_surface` |
| 2x MSAA not advertised | `framebufferColorSampleCounts 0x7` = 1\|2\|4, matching the vendor |
| `bufferDeviceAddress` | `vk12.bufferDeviceAddress=1` (was 0), 9/9 on descriptors and push constants |
| 64-bit push constants | `pctest` 6/6, and the core `vkCmdPushConstants` entry point now resolves and works |

### Still open, measured

Everything the vendor advertises and the open driver does not: **21 device features and 30 device
extensions**, plus the API version. This is the authoritative list - what follows is the whole of it,
not a selection.

**1. 8/16-bit storage and the 16-bit shader types** (the group DXVK actually uses):
`vk11.storageBuffer16BitAccess`, `storagePushConstant16`, `storageInputOutput16`,
`uniformAndStorageBuffer16BitAccess`, and `vk12.storageBuffer8BitAccess`, `storagePushConstant8`,
`uniformAndStorageBuffer8BitAccess`, `shaderFloat16`, `shaderInt8`. Vendor has all nine, open has
none. Implementation work in pco/NIR, and the backend says so itself: `trans_load_input_vs()` and
`trans_store_output_vs()` both carry a bare `/* TODO: f16 support. */` and assert 32-bit types, and
there is no 8/16-bit storage-access lowering at all. The silicon is capable of the whole group - the
vendor driver implements it on this very board, and the core's device info reports 16-bit float
capability (`has_usc_f16sop_u8`, `has_pbe_filterable_f16` in `bxm-4-64.h`).

**2. `variablePointers` + `variablePointersStorageBuffer`**, **`drawIndirectCount`** - engine-side
conveniences some renderers require outright. `variablePointers` also needs
`VK_KHR_variable_pointers`.

**3. `vulkanMemoryModel` + `vulkanMemoryModelDeviceScope`** - present in the vendor, absent here.
Worth flagging because it was *not* on 20.2's list: the vulkan memory model affects how the compiler
may reorder and cache memory operations, so implementations that enable it usually need
`nir_lower_vulkan_memory_model` wired into the pvr pipeline first.

**4. Core features**: `depthClamp`, `occlusionQueryPrecise`, `vertexPipelineStoresAndAtomics` - all
advertised by the vendor, all `false` in pvr's feature table.

**5. Vulkan 1.3 features**: `robustImageAccess`, `pipelineCreationCacheControl`,
`shaderZeroInitializeWorkgroupMemory` (see below - these three are exactly the 1.3 blocker).

**6. API version**: open reports `1.2.363`, vendor `1.3.277`. Applications that *require* 1.3 still
refuse the device.

**7. Timestamps**: `timestampPeriod 0.000` against the vendor's `512.000`, and the vendor also
advertises `VK_KHR/EXT_calibrated_timestamps`. There is no timestamp query path in pvr at all, so the
honest value is 0 - this is unimplemented work, not a mis-set constant.

**8. `bufferDeviceAddressCaptureReplay`** - open by choice; it needs application-directed placement in
the winsys so an address survives a process restart.

### Where the open driver is ahead of the vendor

Worth recording, because "compatibility" is a two-sided comparison: the open driver advertises 4
features the vendor does not (`vk12.shaderInputAttachmentArrayDynamicIndexing`,
`shaderUniformTexelBufferArrayDynamicIndexing`, `shaderStorageTexelBufferArrayDynamicIndexing`,
`vk13.descriptorBindingInlineUniformBlockUpdateAfterBind`), **22 device extensions** the vendor lacks
(`VK_KHR_dynamic_rendering`, `VK_KHR_synchronization2`, `VK_KHR_maintenance4`, `VK_KHR_present_wait2`,
`VK_EXT_robustness2`, `VK_EXT_map_memory_placed`, ...) and **7 instance extensions** (the
`KHR_display`/DRM display path, `VK_EXT_direct_mode_display`, `VK_EXT_acquire_drm_display`,
`VK_EXT_surface_maintenance1`, ...) - against 106 and 114 device extensions respectively, so the two
lists are close in size rather than one being a subset. zink runs here partly *because* of the
overlap: it needs dynamic rendering, which the vendor only has as core 1.3.

### The API 1.3 gap is three features wide, and that is a smaller job than 20.6 implied

`VkPhysicalDeviceVulkan13Features` on the open driver is true for every flag **except three**:

```
vk13.pipelineCreationCacheControl=0
vk13.robustImageAccess=0
vk13.shaderZeroInitializeWorkgroupMemory=0
```

`textureCompressionASTC_HDR` is the only other false, and that one is optional. Every 1.3-promoted
feature 1.3 does *not* require is already advertised: `dynamicRendering`, `synchronization2`,
`maintenance4`, `inlineUniformBlock`, `descriptorBindingInlineUniformBlockUpdateAfterBind`,
`privateData`, `shaderIntegerDotProduct`, `subgroupSizeControl`, `computeFullSubgroups`,
`shaderDemoteToHelperInvocation`, `shaderTerminateInvocation`.

The three false ones are precisely the three that Vulkan 1.3 **requires**
([spec feature requirements](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#features-requirements)),
so the sentence in 20.6 - "needs the 1.3 core feature set" - was accurate but pessimistic. It is:
a flag plus a small compile-required path (`pipelineCreationCacheControl`), a shared-memory zeroing
prologue in the compiler (`shaderZeroInitializeWorkgroupMemory`), and per-access clamping for
out-of-bounds image reads (`robustImageAccess`). Whether reporting 1.3 is *worth* it is a separate
question: DXVK-Sarek and vkd3d-proton ask for 1.1 and work today at 1.2.

### Non-Vulkan items still open

- cached memory type: per-submit flush/invalidate UAPI and the untruthful `HOST_COHERENT` claim
  (§21.12); the type stays opt-in until then;
- the §16 push-constant "one submission late" result was measured on mesa-25.3.0 with
  `0003-pvr-implement-core-CmdPushConstants.patch`; it has not been re-measured on mesa-main;
- X11 WSI is advertised, but end-to-end presentation has never been exercised - it needs an X server
  that does not touch the GPU, since only one driver can own the device at a time;
- the DXVK/vkd3d path has not been re-run since `bufferDeviceAddress` and the 64-bit push-constant fix
  landed, so the real-application effect of both is unverified;
- performance is a separate axis from compatibility and is still the widest gap: draw is 2.2-3.3x the
  vendor's and the whole difference is per-tile cost (§19), with the tiling work blocked on the
  hardware programming guide.
