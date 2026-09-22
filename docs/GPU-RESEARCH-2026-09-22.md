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
