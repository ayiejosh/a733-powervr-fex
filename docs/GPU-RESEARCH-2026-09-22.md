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
