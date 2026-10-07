# Open PowerVR stack: now runs a real display stack (Wayland + XWayland)

**Date:** 2026-10-06 (evening)
**Board:** Radxa Cubie A7A (Allwinner A733), kernel `6.6.98-5-aw2511`
**GPU:** PowerVR B-Series BXM-4-64 MC1, BVNC 36.56.104.183
**Goal this session:** get the *open* stack (mainline `powervr` + Mesa pvr Vulkan + zink)
to drive a real display so it can be benchmarked like the vendor stack.

**Result: it does now.** Weston runs on `card0` with zink on the open Vulkan driver,
XWayland works, and `glmark2-es2` completes. Two real shader-compiler bugs were found and
fixed along the way. The measured system-level score is still far behind the vendor, and
that gap is now measurable rather than blocked.

---

## 1. The blocker was Mesa's `kmsro`, not the GPU

`card0` is `sunxi-drm` (display controller); the GPU is `card1`/`renderD128` (PowerVR).
Mesa's `kmsro` layer is what lets a render-only GPU draw into buffers the display can scan
out. `kmsro` picks its render driver from a table, and the relevant branch is:

```c
#if defined(GALLIUM_ZINK)
   if (!screen) {
      ro->create_for_resource = renderonly_create_kms_dumb_buffer_for_resource;
      screen = zink_drm_create_screen_renderonly(ro->gpu_fd, ro, config);
   }
#endif
```

**That branch does not exist in Mesa 25.0.7**, which is what is installed. It was added
later. Verified by diffing upstream `src/gallium/winsys/kmsro/drm/kmsro_drm_winsys.c`:

| Mesa version | has zink fallback in kmsro |
|---|---|
| 25.0.7 (installed) | **no** |
| 25.1.0 | no |
| 25.2.0 | no |
| 25.2.8 | no |
| 26.1.2 | **yes** |

Consequence on this board: `kmsro` found no usable render driver, silently fell back to
`llvmpipe`, and the PowerVR GPU was bypassed. The visible symptom was
`DRM_IOCTL_MODE_ADDFB2 -> ENOENT`, because the GBM buffer had been allocated on `card1`
while the framebuffer was registered against `card0` (`drm_gem_object_lookup` finds no
such handle in that fd's table, and `drm_gem_fb_init_with_funcs` turns that into `-ENOENT`).

### Why not just install a newer Mesa

* Debian sid `mesa-libgallium` 26.2.4 arm64 needs **`GLIBC_2.43`** and
  **`libLLVM.so.22.1`**; this board has glibc 2.41 / LLVM 19. Unusable.
* trixie-backports 26.1.2 **has** the fix, but its arm64 `mesa-libgallium` is 404 in the
  pool on every mirror tried (only `armel` is published).
* `apt install` is not safe here at all — see §6.

So Mesa was built from the local `mesa-main` tree (26.3.0-devel) instead.

## 2. Second blocker: no logind seat

`loginctl` shows this shell's session with **no seat** (`CLASS=manager`,
`SERVICE=systemd-user`), so KWin's `DBusLogindSeat` cannot `TakeDevice` and reports
`No suitable DRM devices have been found`. One environment variable bypasses it:

```bash
export LIBSEAT_BACKEND=noop     # libseat opens the device directly
```

Weston then takes `/dev/dri/card0` normally. (Weston must still run as root, otherwise it
cannot open `/dev/input/event*` and exits with `fatal: failed to create compositor backend`.)

## 3. Build recipe

```bash
meson setup --reconfigure build \
    -Dgallium-drivers=zink -Dgbm=true -Dopengl=true \
    -Dplatforms=wayland,x11 -Dglx=dri
ninja -C build
```

Notes that cost time:

* `with_gallium_kmsro = with_gallium_drm or (system_has_kms_drm and with_gallium_zink)`
  — enabling `zink` auto-enables kmsro and `sun4i-drm`, no extra option needed.
* **`src/meson.build:163` only builds the `dril` target when
  `with_glx == 'dri' or with_platform_x11`.** `libdril_dri.so` is where the per-driver
  entrypoints live (`DEFINE_LOADER_DRM_ENTRYPOINT(sun4i_drm)`), so a Wayland-only gallium
  build produces a `libgallium` with **no usable driver entrypoints at all**. Patch applied
  in the local tree to also build it when `with_platform_wayland`.
* `dril`'s megadriver symlinks (`sun4i-drm_dri.so`, `zink_dri.so`) are only created at
  *build* time if `prog_ln` is found — it is, so they appear in
  `build/src/gallium/targets/dril/`.

## 4. Runtime environment

`/home/radxa/gpu-open-stack/env26.sh` (sourced by the launcher and by clients):

```bash
B=/home/radxa/_REVIEW/emulation/mesa/mesa-main/build
LD_LIBRARY_PATH=$B/src/glx:$B/src/egl:$B/src/gbm:$B/src/gallium/targets/dri:\
$B/src/mesa/glapi/es2api:$B/src/mesa/glapi/es1api:$B/subprojects/zlib-1.3.1:\
/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu
LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dril
GBM_BACKENDS_PATH=$B/src/gbm/backends/dri
MESA_LOADER_DRIVER_OVERRIDE=sun4i-drm      # compositor
VK_ICD_FILENAMES=/home/radxa/pvr_gen_icd.json
PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1
LIBSEAT_BACKEND=noop
XDG_RUNTIME_DIR=/run/user/1000
```

Two details worth recording:

* `GBM_BACKENDS_PATH` is required — Mesa's gbm looks for `dri_gbm.so` under the install
  prefix, not the build tree.
* **Clients must use `MESA_LOADER_DRIVER_OVERRIDE=zink`, not `sun4i-drm`.** With
  `sun4i-drm` a client tries `DRM_IOCTL_MODE_CREATE_DUMB` on `card0`, which fails
  `EPERM` because the compositor holds DRM master.

```bash
sudo /home/radxa/gpu-open-stack/w26x.sh                       # weston + XWayland, card0
sudo /home/radxa/gpu-open-stack/wclient26.sh weston-simple-egl # Wayland client
sudo bash -c '. /home/radxa/gpu-open-stack/env26.sh; \
  export DISPLAY=:0 MESA_LOADER_DRIVER_OVERRIDE=zink; glmark2-es2 --benchmark default'
```

## 5. What works now

```
GL version:  OpenGL ES 2.0 Mesa 26.3.0-devel
GL renderer: zink Vulkan 1.3(PowerVR B-Series BXM-4-64 MC1 (IMAGINATION_OPEN_SOURCE_MESA))
Output 'HDMI-A-1' enabled with head(s) HDMI-A-1
Xwayland :0 -rootless -wm 50 -terminate
```

* Compositor: 0 `failed to create kms fb` (previously 2+ per second).
* Wayland client: `weston-simple-egl` 289/265/295/245 frames per 5 s (~50–59 fps, vsync).
* XWayland: `eglinfo` and `glmark2-es2` both report the open renderer.
* glmark2 now runs **to completion** (it used to abort mid-run).

## 6. Two real shader-compiler bugs found and fixed

Both were `UNREACHABLE()` aborts — i.e. the driver crashing on ordinary shaders.

### 6.1 16-bit `flrp` was never lowered

`nir_print.c` prints the bit size as a prefix, so the failing instruction

```
Unsupported alu instruction: "16    %81 = flrp %80 (0.099976), %79 (0.399902), %58"
```

is a **16-bit** flrp. The chain:

* `nir_options.lower_flrp32 = true` lowers 32-bit flrp, but `lower_flrp16` was unset, so
  `nir_opt_algebraic` *creates* 16-bit flrp (its `@16` creation patterns are gated on
  `!options->lower_flrp16`).
* `nir_lower_flrp(nir, 32, true)` uses the mask `32`, and the pass tests
  `alu->def.bit_size & lowering_mask`, so `16 & 32 == 0` — 16-bit flrp is never lowered.
* `trans_conv()`/`trans_alu()` have no `flrp` case at all.

**Fix:** `.lower_flrp16 = true` and `nir_lower_flrp(nir, 16 | 32, true)`.

### 6.2 `b2f16` / `i2f16` / `u2f16` had no translation

```
Unsupported alu instruction: "16    %75 = b2f16 %74 // preserve:sz"
```

The chain is more interesting, because the driver is *advertising* a capability it cannot
compile:

1. `pvr_physical_device.c:349` — `.shaderFloat16 = true`.
2. `zink_screen.c:632` — zink sees `shader_float16_int8_feats.shaderFloat16` and therefore
   implements GLES `mediump` as **fp16**.
3. Mesa's GLSL→NIR then emits 16-bit ops, including `b2f16`/`i2f16`/`u2f16`.
4. `trans_conv()` handles `b2f32`/`i2f32`/`u2f32` but has **no 16-bit cases**, and
   `trans_alu()`'s dispatch list does not route them to `trans_conv()` either — so they
   fall into `default: UNREACHABLE`.

glmark2's own shader comments confirm this is expected behaviour:
*"should be declared highp since the multiplication can overflow in mediump, particularly
if mediump is implemented as fp16"*.

**Fix:** added `b2f16`, `i2f16`, `u2f16` to both the `trans_conv()` switch and the
`trans_alu()` dispatch list, reusing the conversions the ALU does have
(`f2f16` via `pco_pck`/`PCO_PCK_FMT_F16F16`, and `i2f32`/`u2f32` via `pco_unpck`), so
`b2f16(x)` = select fp32 1.0/0.0 then pack down, and `i2f16(x)` = int→fp32→fp16.

Effect: the `cel` and `terrain` glmark2 scenes went from aborting the run to rendering.

## 7. Measured result, and the honest gap

`glmark2-es2 --benchmark default`, 800x600 windowed:

| stack | presentation | glmark2 Score |
|---|---|---|
| vendor (`IMAGINATION_PROPRIETARY`) | X + glamor | **341** |
| open (`IMAGINATION_OPEN_SOURCE_MESA`) | XWayland + weston | **22** |

**Scoring basis, checked empirically:** on this glmark2 build the Score is essentially the
**mean FPS** across scenes, not the sum — a 1-scene run at 28 fps printed `Score: 27`, and a
2-scene run at 26/29 fps printed `Score: 26`. The full default set ran 33 scenes summing to
785 fps, mean 23.8, and printed `Score: 22`. So the two numbers above *are* in the same
units and the vendor's 341 was recorded with the identical command on the vendor stack.

**So the open stack is currently ~15x slower on this system benchmark.** That is a large,
real gap, and it is now *measurable* — which it was not before this session. Caveats that
should be closed before treating 15x as exact:

* the presentation paths differ (vendor: X + glamor; open: XWayland → weston → card0);
* the worst scenes dominate the mean — `[terrain]` 4 fps and `[desktop] blur` 10 fps;
* the vendor number is from earlier in the session, before the two compiler fixes.

Note this is consistent with the older shader-loop measurement in
`results-2026-09-23-gl.txt`, where the open driver was ~70x behind the vendor on
`glbench` (101 vs 7231 Mpix/s at loop=4) — that was an even earlier build.

## 8. Where the remaining performance work is

The vendor GL path is **Mesa zink → vendor Vulkan**, and the open path is
**Mesa zink → open Vulkan**. The GL layer is shared, so the whole 15x is in the open
Vulkan driver (or in the dmabuf/presentation path added here). The per-stage numbers from
the purpose-built bench (`bench/pvr-vulkan`, 512x512x60 warmed) still frame it best:

| stage | open | vendor |
|---|---|---|
| record | +0.313 ms | — |
| submit | +0.151 ms | — |
| gpu_wait | +0.581 ms | — |
| **total frame** | **1.721 ms** | **0.668 ms** |

and compute (`vktest`) was 91% of vendor. So the fixed per-frame driver overhead is the
first target, then the heavy raster scenes (terrain/desktop-blur) where the 4 fps and
10 fps rows live.

## 9. Reproducing / undoing

* Build tree: `/home/radxa/_REVIEW/emulation/mesa/mesa-main` (`build/`).
* Launchers: `/home/radxa/gpu-open-stack/{env26.sh,w26.sh,w26x.sh,wclient26.sh}`.
* Raw glmark2 output: `/home/radxa/gpu-open-stack/glmark2-open-final.txt`.
* To restore the vendor stack: unbind `powervr`, `rmmod powervr`, bind `pvrsrvkm`,
  `systemctl restart display-manager`. **Never `pkill` `kwin_x11` while `pvrsrvkm` is
  bound** — that NULL-derefs the closed driver and reboots the board.
* System files added (build tooling, X11 dev headers, one Wayland header) are listed with
  undo commands in
  `Radxa-A7A/Recovery/GPU-FIRMWARE-RE-2026-10-06.md` §9/§9a. All are additive and none is
  read at boot.

## 10. Also done this session

* Removed the `PVR_JOB_LIMIT` diagnostic from
  `src/imagination/vulkan/winsys/powervr/pvr_drm_job_render.c` (it was marked
  "DIAGNOSTIC ONLY, not for shipping").
* Confirmed Debian sid Mesa 26.2.4 `.deb`s are unusable on this board (glibc/LLVM), and
  that `apt install` must not be used at all here: `dpkg --print-architecture` says `arm64`
  with no foreign architectures, but `/var/lib/dpkg/status` holds **431 `Architecture: amd64`
  and 0 arm64** entries.

---

# ADDENDUM 2 — 2026-10-07: vendor A/B, and FBCDC is the WRONG target

## A. Vendor A/B, same session, same harness

| harness | vendor | open | ratio |
|---|---|---|---|
| `glmark2-es2 --benchmark default`, X + glamor | **522** | — | — |
| `glmark2-es2 -b build:use-vbo=false`, weston + XWayland | **787** | **31** | 25x |

The second row is the one that matters: **identical compositor (weston), identical
XWayland, identical client, identical GL layer (zink)**. Only the Vulkan driver differs.
So the gap is not the compositor choice and not X vs Wayland.

Setup: vendor compositor used the BSP GL (`GL renderer: PowerVR B-Series BXM-4-64`,
`MESA_LOADER_DRIVER_OVERRIDE=sunxi-drm`, `/usr/local/lib/dri`); the vendor *client* used
`glrun` (zink -> `/usr/share/vulkan/icd.d/img_icd.json`, `IMAGINATION_PROPRIETARY`).

> The vendor measurement is what rebooted the board: the 1600x1200 run drove
> `PVRDmaBufOpsUnmapCommon` -> `sunxi_iommu_unmap` ->
> `iommu_master de0_iommu: Runtime PM usage count underflow!` -> `L1 PageTable Invalid`
> -> panic. That is a vendor `pvrsrvkm` PRIME/dmabuf bug, the 4th vendor-driver reboot
> this session. The open stack has never done this.

## B. What the 25x actually is

Splitting the open stack's frame with `glmark2-es2 --off-screen` (no presentation):

| size | windowed | off-screen | present cost |
|---|---|---|---|
| 320x240 | 18.9 ms | 7.3 ms | 11.6 ms |
| 800x600 | 35.7 ms | 7.1 ms | 28.6 ms |
| 1600x1200 | 100 ms | 11.8 ms | 88 ms |

* **Off-screen is flat ~7 ms** -> that is the real GPU work. Vendor total frame is
  1.27 ms, so rendering is ~5.6x behind.
* **Present fits `~8.4 ms + 41.5 ms/Mpix`** -> ~100 MB/s. It is 78% of the frame at
  800x600 and dominates everything.

So **FBCDC (framebuffer compression) is the wrong first target**: it addresses the 7 ms of
rendering, not the 25 ms of presentation. FBCDC is still a real gap (hardware has it,
`bxm-4-64.h .has_fbcdc_algorithm`, vendor sets `INICFG_FBCDC_V3_1_EN`, open driver says
"Currently no support for FBC") but it is second.

## C. What the present path is doing

* `card1` (PowerVR) has **zero connectors** — only `card0`/`sunxi-drm` drives HDMI-A-1.
  So kmsro is mandatory; the GPU cannot be the KMS device.
* Weston is **not** using dumb buffers: strace shows no `MODE_CREATE_DUMB`, but
  `PVR_CREATE_BO` + `PRIME_HANDLE_TO_FD` + `PRIME_FD_TO_HANDLE` — GPU BOs PRIME-shared
  to card0.
* Weston presents at only ~40/s (`MODE_ATOMIC` 398 in 10 s) while the client wants more,
  and the client is **blocked, not CPU-busy**: weston is essentially idle (6 jiffies
  utime in 8 s), Xwayland ~21% of one core.
* Client present is real DRI3 (`memfd_create("xshmfence")`, tiny writes, no shm image
  traffic), so it is not the XPutImage/readback fallback.

### Tried and reverted

Changing kmsro's zink branch from `renderonly_create_kms_dumb_buffer_for_resource` to
`renderonly_create_gpu_import_for_resource` (the vc4 model) **crashes**: that helper calls
`resource_get_handle(rsc)` on the GPU resource, but zink calls `create_for_resource`
*BEFORE* `resource->obj` is created (`zink_resource.c:1840` runs above
`resource_object_create`). NULL deref in `renderonly_create_gpu_import_for_resource`.
That ordering is exactly why zink uses the dumb-buffer variant. Doing the vc4 model for
zink needs a zink-side reorder (create the image, export it, then PRIME into KMS), not a
kmsro one-liner.

## D. Measurement caveat

This board runs an **x86-emulated Chrome Remote Desktop host** (`FEXInterpreter
/opt/google/chrome-remote-desktop/chrome-remote-desktop-host`), syncthing, and had
~2.5 GB swapped out during part of this work. One `top` sample showed `kswapd0` at 92%
and only 37 MB free. GPU buffers live in system RAM, so memory pressure can pollute
present-path numbers. Re-measure with those controlled before trusting absolute values;
the 25x is far too large to be explained by that noise, but the per-pixel constant is not.

## E. Corrected next steps

1. **Present path first** (~25 ms/frame, 78%). Find why weston presents at 40/s and why
   the client blocks. Candidates: weston's own zink render pass overhead per frame,
   buffer count / present mode, the kmsro PRIME round-trip per frame.
2. Then the ~7 ms rendering gap (5.6x vs vendor).
3. Then FBCDC, then the unconditional partial-render job, then TFBC.

---

# ADDENDUM 3 — 2026-10-08: the 28 ms is zink-specific, not the Vulkan present path

`vkgears` is a pure-Vulkan swapchain client — no zink, no GL, same weston+XWayland harness.
Measured on both drivers, same command:

| vkgears | vendor | open | ratio |
|---|---|---|---|
| default (vsync, 60 Hz panel) | 60.0 FPS | 60.0 FPS | 1.0x |
| `-present-mailbox` (vsync off) | **2589 FPS** | **613 FPS** | **4.2x** |

So the open driver's raw Vulkan WSI/present path is only **4.2x** behind, and in absolute
terms ~1.25 ms/frame (1.63 vs 0.39 ms) — **not** the ~28 ms glmark2 sees.

That rules the rest of the space out:

* not the compositor harness — vkgears uses the *same* Xwayland + weston path and presents at 613 FPS
* not the scanout buffer — GPU-import scanout was tried and was *slower* than dumb buffers
* not syscalls or CPU — slowest ioctl 37 us, weston idle
* not missing WSI — open driver has xcb/xlib/wayland surface extensions

**Conclusion: the ~28 ms is introduced by zink's present path specifically.** A native
Vulkan client through the identical stack pays ~1.6 ms; the GL-over-Vulkan client pays
~28 ms. Next probe is zink's kopper present/flush, not the driver's WSI.

Two real items remain on the WSI side, just smaller than the 28 ms: the 4.2x mailbox gap,
and the fact that both drivers are vsync-capped at 60 in the default mode.

---

# ADDENDUM 4 — 2026-10-08: it may be one problem, not two

More negatives, and a synthesis.

## Ruled out this round

* **zink implicit_sync drain** — `VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA` is already in
  zink's exclusion list (`zink_screen.c:2972`), so the extra `QueueSubmit` +
  `WaitForFences(ALL_COMMANDS)` in `kopper_present` does *not* run for the open driver.
  It runs for the *vendor* (not listed) and is still fast.
* **X11 vs Wayland** — same program both ways: `es2gears_x11` 22 FPS,
  `es2gears_wayland` 30 FPS. Only 1.36x apart. So it is zink-vs-native-Vulkan, not X-vs-Wayland.
* **Xwayland copying the frame** — Xwayland CPU is *flat* (17% at 320x240, 14% at 1600x1200)
  while FPS falls 92 -> 14. Not a CPU copy.
* **CPU spin** — windowed is 22% CPU and off-screen 70%, yet off-screen is 4.5x faster.
  The client is *blocked*, not spinning. Wall-clock strace shows 62 futex + 11 ppoll per
  frame windowed vs 6.5 futex/frame off-screen. (`/usr/bin/time` does not exist on this board.)

## The synthesis

Cost scales cleanly with area, GPU-side, and Xwayland is idle-ish:

| size | windowed | off-screen | present |
|---|---|---|---|
| 320x240 | 10.9 ms | 7.3 ms | 3.6 ms |
| 1600x1200 | 71 ms | 11.8 ms | 59 ms |

slope ~30 ms/Mpix ~ 133 MB/s. But the client's *own* off-screen rendering is 0.48 Mpix in
7 ms = 68 Mpix/s, also low. Vendor fill is ~380 Mpix/s (800x600 at 787 FPS).

So this is probably **not** a separate "present bug" at all: the open driver's memory
throughput looks ~6x low across the board. That makes **FBCDC the convergent fix** rather
than a second, independent problem — the vendor compresses framebuffer traffic and the
open driver does not ("Currently no support for FBC").

The render-target path is also LINEAR-only (`pvr_formats.c:476` "We support LINEAR only
yet"), on a tiler, which is consistent with a low-throughput write path.

---

# ADDENDUM 5 — 2026-10-08: the driver's render+scanout MATCHES the vendor; zink is the gap

## pvranimate: compositor-free render -> page-flip loop

```
display: 3840x2160@60, 240 frames
presented 240 frames in 5403.4 ms: 44.4 fps (3 buffers, 0 flip timeouts)
```

4K at 44.4 FPS = **368 Mpix/s**. Vendor fill is ~380 Mpix/s (800x600 at 787 FPS).
**The open driver's own rendering + scanout throughput matches the vendor.** With no
compositor in the way there is no performance gap to close.

And native Vulkan through the *full* stack is fine too: vkgears `-present-mailbox` 613 FPS.

So the entire gap is introduced by **zink** (GL over Vulkan). zink off-screen is 7 ms;
zink presenting is ~25 ms; native Vulkan presenting is ~1.6 ms.

## pvrscanout: the driver can do OPTIMAL tiling, renderable AND exportable

```
RGBA8  OPTIMAL  CA+TS  base=yes exportable=yes
```

So "LINEAR only" (`pvr_formats.c:476`) applies to the `VK_EXT_image_drm_format_modifier`
path only — `VK_IMAGE_TILING_OPTIMAL` images are renderable and dma-buf exportable.
The linear-render hypothesis is therefore not forced on us.

## memtypes: GPU bandwidth is FINE; CPU access and allocation are not

| | open | llvmpipe |
|---|---|---|
| GPU copy 1 MiB + fence | 0.564 ms (~1.9 GB/s) | 0.520 ms |
| CPU read 1 MiB | **2.982 ms (335 MB/s)** | 0.128 ms (7798 MB/s) |
| allocate+bind 1 MiB | **0.854 ms** | 0.004 ms |

The open driver's only memory type is `DEVICE_LOCAL HOST_VISIBLE HOST_COHERENT` — **there
is no `HOST_CACHED` type**, so any CPU access is ~23x slow. zink does per-frame uploads
through host-visible memory, which would make it slow in exactly the way native Vulkan
clients (which upload little) are not. That is the leading explanation for why the gap is
zink-specific, and it is a driver bug, not a zink one.

## Real bug found by pvranimate

```
push constants: NOT working - content does not follow the pushed value
ordering: a push made BEFORE vkCmdBindPipeline is ignored
  (pixel 66 vs 2 after bind) - the driver uploads push constants while setting up the pipeline
```

---

# ADDENDUM 6 — 2026-10-08: cached memory type tested — no gain, and it is still unsound

`pvr_physical_device.c:1319-1370` keeps a second `HOST_CACHED` memory type behind
`PVR_ENABLE_CACHED_MEMORY_TYPE`, with a comment recording that it was unsound (advertised
coherent; with `VK_KHR_buffer_device_address` zink gives each buffer its own allocation, 20
of 23 took that type, and GL rendered up to 79440/262144 pixels wrong), and that the real
flush/invalidate has since landed via `DMA_BUF_IOCTL_SYNC`.

Tested it, on the theory that 335 MB/s CPU reads vs 7798 MB/s would be exactly the kind of
thing that hits zink's per-frame uploads and not native Vulkan clients:

| | without | with cached type |
|---|---|---|
| windowed 800x600 | 42 FPS | **42 FPS** |
| off-screen | 188 FPS | 188 FPS |
| `glmark2-es2 --validate` | Success | **Failure** (multiple scenes) |

**No performance change at all, and correctness breaks.** So the earlier decision to keep it
opt-in is still correct, and the missing cached type is *not* the present-cost cause. Reverted;
validate returns to all-Success.

## Where the present cost stands

Still unexplained, but now bounded on both sides:

* weston compositing the damage runs at **~133 MB/s** effective (30 ms/Mpix, area-proportional,
  GPU-side, Xwayland idle, no blocking syscall)
* `pvranimate` renders 4K into GPU-allocated LINEAR dma-bufs at **368 Mpix/s (~1.5 GB/s)**

That is a ~10x gap between "weston composites into kmsro's dumb scanout buffer" and "the same
GPU renders into a GPU-allocated dma-buf". Note this is *not* the same as the earlier
GPU-import experiment (which was slower) — that path went through zink's LINEAR *modifier*
path, whereas `pvrscanout` shows plain `VK_IMAGE_TILING_OPTIMAL` is renderable *and*
exportable, which is the layout `pvranimate` actually uses.

---

# ADDENDUM 7 — 2026-10-08: it is zink's X11 present path, and nothing else

## The definitive same-configuration comparison

Same window (800x600), same weston + XWayland, same display:

| client | present mode | FPS |
|---|---|---|
| vkgears (native Vulkan) | fifo | 59.8 |
| vkgears (native Vulkan) | mailbox | **543.8** |
| zink (glmark2) | default | **42** |

13x between two clients through the identical stack. It is zink, unambiguously.

## Not vsync

`vblank_mode=0` -> 45 FPS. `MESA_VK_WSI_PRESENT_MODE=mailbox` -> 42.
`MESA_VK_WSI_PRESENT_MODE=immediate` -> 43. Real cost, not pacing.

## Not the Mesa version

The vendor 787 FPS figure used *system Mesa 25.0.7* zink (via `glrun`); the open 42 used
Mesa 26.3. Controlling for that: **open ICD + system Mesa 25.0.7 zink = 38 FPS**. So the
Mesa version is irrelevant and the comparison was valid.

## It is the X11 socket

`strace -f -e trace=ppoll -T`: **1348 ppoll calls, 9.6 s total, all on `fd=3`** which is
`socket:[287433]` — the X11 connection. (fd 4..8 are `/dev/dri/renderD128`.) The client is
blocked ~19 ms/frame on X11 round-trips, which matches the 18.5 ms present delta exactly.

Note this is why `strace -T` on `ioctl` found nothing: the block is not a GPU ioctl.

## Dead ends recorded so they are not retried

* zink `QueueWaitIdle` per frame — it is only in `zink_kopper_present_readback`, a
  `glReadPixels` path, not the normal present.
* allocation churn — `PVR_API_TRACE=1` shows **15 allocations for a whole run**
  (9 x 1921024 = the 800x600 swapchain images). zink is not allocating per frame.
* `PVR_API_TRACE` exists but only traces `vkAllocateMemory`/`vkCreateBuffer`; it is not a
  general API tracer.

## Where this leaves it

zink is fast on Wayland and slow on X11: `weston-simple-egl -b` (native Wayland, interval 0)
runs at 153-213 FPS, while the same zink on XWayland is 42. Native Vulkan on XWayland is 543.
So the target is **zink's X11 present path** — the Vulkan/X11 WSI round-trip on the open
driver — and not the GPU, not the compositor, and not the driver's rendering.

---

# ADDENDUM 8 — 2026-10-08: FOUND IT. The driver advertises fp16 and renders 20/27 scenes wrong

## The bug

`pvr_physical_device.c` advertised `shaderFloat16 = true`. zink therefore implements GLES
`mediump` as fp16, and Mesa emits 16-bit NIR ops. The driver's 16-bit path is not correct
enough for that.

Measured with `glmark2-es2 --off-screen -s 800x600 --validate`:

| ICD | shaderFloat16 | validate |
|---|---|---|
| open driver | **true** | **20 failures / 7 pass** |
| open driver | **false** | **0 failures / 27 pass** |
| llvmpipe (control) | - | 0 failures / 27 pass |

The llvmpipe row matters: it is the *same zink* over a correct software Vulkan driver, and it
passes 27/27. So the test is valid, zink is not at fault, and the fault is the driver's fp16.

Failing scenes with fp16 on: texture (nearest/linear/mipmap), blinn-phong-inf, phong, bump
(high-poly/normals/height), effect2d (both kernels), desktop blur, buffer (all three),
conditionals(frag=5), function (both), loop (all three). Passing: build (both), gouraud,
pulsar, shadow, and the simple conditionals.

## It is also faster

Mean FPS over the 19 scenes common to both full runs:

| | fp16 on | fp16 off |
|---|---|---|
| mean FPS | 24.0 | **34.3** |
| ratio | - | **+42.8%** |

Not surprising: every 16-bit op costs pck/unpck conversions in this driver.

## Consequence for the earlier pco work

The 16-bit `flrp` lowering and `b2f16`/`i2f16`/`u2f16` translations added earlier are still
correct and should stay - they turn hard `UNREACHABLE` aborts into working code if fp16 is
ever enabled. But they were treating the symptom: the real bug was advertising a capability
the implementation cannot honour. Advertise fp16 again only when the 16-bit path is complete.

## Method note

This was found by treating `glmark2 --validate` as a real test and getting a control
(llvmpipe = 27/27). Earlier in the session I twice asserted "correctness restored, 0 failures"
after counting only the *successes* - the failure count was never measured, and the 20
failures were present the whole time. Count both, always.

---

# ADDENDUM 9 — 2026-10-08: fp16 fix confirmed; the shared bottleneck is the compositor

## The fp16 fix holds

| | score |
|---|---|
| glmark2 default, fp16 advertised | 22 |
| glmark2 default, fp16 disabled | **32 (+45%)** |
| glmark2 `--validate` | 20 failures -> **0 failures** |

Both correctness and score improved from the same one-line change. The present cost itself
did **not** move (windowed/off-screen still 44 / 188 FPS), because the `build` scene barely
uses fp16 - the score gain comes from the shading/texture/buffer scenes that do.

## Feature audit (same class as fp16)

* `fillModeNonSolid = false` - zink declares it a base requirement and warns about it
* `geometryShader = false`
* `bufferDeviceAddress = true` but `bufferDeviceAddressCaptureReplay = false` - **correctly**
  not over-claimed (`pvr_device.c:914` FINISHME is consistent with that)
* `shaderInt8`, `storageBuffer8BitAccess`, `uniformAndStorageBuffer8BitAccess`,
  `storagePushConstant8` are all advertised. These are the *same shape* of risk as fp16, but
  validation now passes 27/27, so there is no failing test to catch them yet. Needs a targeted
  8-bit test, not glmark2.

## The present cost is a SHARED compositor cost, not a client cost

Two concurrent glmark2 clients, 800x600, clean machine:

| | FPS |
|---|---|
| one client alone | 40 |
| two clients | 19 + 18 = **37 total** |

Fair sharing of a shared ~40/s limit. So the client is not the limit - **weston's compositing
is**, and it is proportional to the damaged area.

This also resolves an earlier contradiction: `weston-simple-egl -f` at 4K looked fast
(41-66 FPS) but a rotating triangle *damages* a small area, and weston composites damage only.
Measuring full-screen client fps does not measure compositor throughput.

Rates: native Vulkan ~368 Mpix/s, zink's own off-screen render ~90 Mpix/s, weston's composite
~19 Mpix/s (36 ms/Mpix). So zink is ~4x off native Vulkan, and the compositor compounds it.

## Dead end

`ZINK_DEBUG` on the compositor changes nothing useful: none 45, nobgc 42, norp 41, rp 35 FPS.

---

# ADDENDUM 10 — 2026-10-08: second over-claim found (8-bit storage / shaderInt8)

After fp16, the same audit pattern found another advertised-but-unimplemented capability.
This one is worse: those shaders cannot compile at all.

Advertised as `true` in `pvr_physical_device.c`:

```
.storageBuffer8BitAccess            = true
.uniformAndStorageBuffer8BitAccess  = true
.storagePushConstant8               = true
.shaderInt8                         = true
```

But the ops that implement 8-bit storage access are not translated:

| op | cases in `pco_trans_nir.c` |
|---|---|
| `extract_u8` / `extract_i8` / `insert_u8` / `insert_i8` | **0** |
| `vec8` | 1 |

and `trans_conv()`'s default is `UNREACHABLE("Unsupported conversion op.")`.

Nothing lowers them away first:

* pco never sets `nir_shader_compiler_options::lower_bit_size` (grep: no occurrence
  anywhere in `src/imagination/`), so NIR keeps 8-bit ops
* `pco_nir.c` has no 8-bit lowering pass

So any shader using 8-bit storage access aborts the process. Disabled all four.

Verified no regression: `--validate` 0 failures / 27 pass, off-screen 179 FPS (baseline 188,
run variance). GL cannot reach this path - GLSL has no `int8` - which is exactly why the
27/27 validate result did not catch it. A Vulkan int8 test would confirm it directly.

Re-enable path: implement `extract_u8/i8` and `insert_u8/i8`, which map onto the `bfe`/`bfi`
the translator already has for `bitfield_extract`/`bitfield_insert`.

## Instrumentation now in the tree: PVR_JOB_TRACE

`pvr_drm_job_render.c` counts hardware jobs per submit. Result, and it is the same for both
clients:

```
zink      : submits=1600 jobs=4800 avg=3.00 geom=1600 pr=1600 frag=1600
vkgears   : submits=10000 jobs=30000 avg=3.00 geom=10000 pr=10000 frag=10000
```

**Every render submit issues 3 hardware jobs** - geometry + partial-render + fragment - which
is the unconditional-PR issue the driver's own TODO describes. It costs a TA->3D transition
per pass for every client *and* the compositor, and since the compositor is the shared
bottleneck (addendum 9) this is worth attacking. Skipping it safely needs SPM sizing
knowledge, so it was not attempted yet.

---

# ADDENDUM 11 — 2026-10-08: correction - the X-socket ppoll was a background thread

I had reported that the client "blocks ~19 ms/frame in ppoll on the X11 socket". **That was
wrong, and it invalidates several conclusions built on it.**

`wsi_common_x11.c` starts a dedicated thread:

```c
   u_thread_setname("WSI swapchain event");
   ...
   xcb_wait_for_special_event(chain->conn, chain->special_event);
```

That thread blocks on the X socket for the entire life of the swapchain. It is normal and
idle. The strace numbers were misread because **`strace -f -w -c` sums per-call wall time
across all threads**, so one thread sleeping forever in a single call dominates the totals -
which is also why the "totals" exceeded the wall clock of the run.

So: the client's main thread was never shown to block on X, and the ~19 ms windowed-vs-off-screen
delta remains unexplained. Do not build on the X-socket finding.

## What else this round settled

* **PR elimination (goal item 3) - investigated, correctly not attempted.** The PR exists
  because SPM is a *runtime firmware decision*: `job->requires_spm_scratch_buffer` is only set
  for `barrier_store` jobs, but the driver still always submits the PR so that a store exists
  if SPM is hit. `pvr_sub_cmd_gfx_requires_split_submit()` is `run_frag && layers > 1` - the
  *multilayer* split submit, a different mechanism. Skipping the PR safely needs SPM sizing
  knowledge, and getting it wrong loses data in exactly the large-render case validate would
  not catch. Left alone on purpose.
* **PVR_JOB_TRACE now measures every client.** Every render submit is 3.00 jobs
  (geometry + partial-render + fragment) for **all three**: zink, vkgears, and **weston**.
  weston does ~40 submits/s during a 45 FPS client run - i.e. one render pass per client
  frame at ~1.8 ms, so it *follows* the client rather than limiting it. The earlier
  "compositor is the shared bottleneck" claim is therefore also not established.
* **Present mode is already the fast one.** Instrumented zink: it picks
  `present_mode=0` (IMMEDIATE) with `type=0` (KOPPER_X11), and on X11 IMMEDIATE makes Mesa
  set `XCB_PRESENT_OPTION_ASYNC`, so there is no present-completion wait. Another theory gone.
* **The numeric feature audit has converged.** Every advertised bit-width capability now has
  matching pco support (`storageBuffer16BitAccess` <-> `i2i16`/`u2u16`/`f2f16`), and every
  unsupported one is now false (`shaderInt16`, `shaderFloat16`, `shaderInt8`, `shaderInt64`,
  `shaderFloat64`, plus the 8-bit storage trio). No further over-claims found in this axis.

---

# ADDENDUM 12 — 2026-10-08: the cost is weston's GPU composite, and it is not scanout or CPU

Two clean experiments pin it down.

## Headless compositor: scanout is not the cost

Same client, same zink, but weston on `headless-backend.so` (no KMS, no scanout, no flips),
1920x1080 output:

| compositor | client FPS |
|---|---|
| KMS, 3840x2160 output | 40-45 |
| **headless, 1920x1080 output** | **40** |

Identical. So the KMS scanout/page-flip path costs nothing here, and **weston's GL composite
is the cost**. Note also that a 4x smaller output did not help - consistent with cost tracking
*damage area*, not output size.

## weston is CPU-idle while the client collapses

| client window | client FPS | weston CPU |
|---|---|---|
| 320x240 | 95 | 6% of a core |
| 1600x1200 | 14 | **3% of a core** |

weston's CPU goes *down* while its work per frame goes up by 25x. So it is GPU-bound, not CPU.

## What this rules out for the present/窗口 cost

* KMS scanout and page flips (headless is identical)
* CPU copies in the compositor (flat 3-6%)
* output size (4x smaller output, same FPS)
* present-completion waits (zink picks IMMEDIATE -> `XCB_PRESENT_OPTION_ASYNC`)
* acquire and present calls themselves (instrumented: acquire ~free, present ~2.6 ms)
* QueueSubmit (instrumented: <1 ms windowed)

So weston's composite of an 800x600 damage region costs ~25 ms of **GPU** time, i.e.
~19 Mpix/s, against zink's own off-screen render at 90 Mpix/s and native Vulkan at 368 Mpix/s.
weston issues 3.00 jobs/submit (geometry + PR + fragment), same as every other client, one
submit per client frame.

Per-pass cost is the remaining suspect: weston's single full-window textured-blend pass costs
~5x what the client's own render pass costs, on the same GPU. That needs GPU-side counters,
which this board cannot provide: no `perf`, no `apitrace`, no `renderdoc`, no `valgrind`; only
`strace` and `gprof` (needs -pg builds). The driver's `pvr_fw/trace_*` debugfs is the remaining
instrument.

## Addendum 12b: swapchain images are OPTIMAL, not LINEAR

Instrumented `zink_resource_create` to print tiling for the swapchain-sized resource:

```
[tl] 800x600 linear=0 modifiers=0 m0=0x0 dt=0
```

So the client's swapchain image is `VK_IMAGE_TILING_OPTIMAL` with **no DRM modifiers at all**
(the WSI did not take the modifier path here). The "weston samples a LINEAR/untiled texture and
that is why the composite is slow" hypothesis is therefore **dead**, and so is the earlier
`supports_modifiers=false` result being about tiling.

Remaining, unexplained: weston's single full-window textured-blend pass costs ~25 ms of GPU
time on the same GPU where the client's own render pass costs ~5 ms and a native-Vulkan
full-screen pass costs ~2.7 ms/Mpix. Nothing available on this board can profile a GPU pass -
no `perf`, no `apitrace`, no `renderdoc`, no `valgrind`; only `strace` and `gprof` (needs -pg).

---

# ADDENDUM 13 — 2026-10-08: FBCDC is NOT reference-implementable here; feature reporting is clean

## FBCDC (goal item 2) is a from-scratch job, not a port

The DKMS kernel source gives only:

* the enable bit: `rgxsrvinit.c:558` sets `RGXFWIF_INICFG_FBCDC_V3_1_EN` from
  `pfnHasFBCDCVersion31()`
* the modifier constants: `include/public/powervr/img_drm_fourcc.h`
  `DRM_FORMAT_MOD_PVR_FBCDC_8x8_V1/V7/V10` and the `16x4` variants

It contains **no FBD structure, no compression-control-stream allocation, and no
compression register programming** - that all lives in the closed userspace. So unlike the
other gaps found this session, there is no reference implementation on this board.

Implementing it means designing the FBD layout without documentation, allocating and
formatting the compression stream, programming the PBE compression format, enabling the
init bit, and handling decompression on read. Silent corruption is the failure mode and the
board has no GPU-side verification tool. **Deprioritised on evidence.**

## The driver's feature reporting is now clean

Audited the remaining gap list against the source:

| feature | advertised | implemented |
|---|---|---|
| `drawIndirectCount` | **false** | `CmdDrawIndirectCount` / `CmdDrawIndexedIndirectCount` - **no matches in the tree** |
| `occlusionQueryPrecise` | **false** | queries exist (`CmdBegin/EndQuery`) but not precise |
| `variablePointers` | **false** | no implementation |
| `vulkanMemoryModel` | **false** | no implementation |

So these are **honestly reported as missing**, not over-claimed. Combined with the fp16 and
8-bit fixes, every capability the driver advertises is now backed by an implementation, and
every capability it lacks is reported false. The two over-claims found this session were the
exceptions, and both are fixed.

## Two more perf hypotheses eliminated

* **weston re-importing/allocating the client's buffer per frame** - `PVR_ALLOC_TRACE=1` on
  weston: **0 allocations** across a 20 s client run. Dead.
* **SLC / DM-overlap gate** - `fw_sysdata_init` disables DM overlap only when
  `slc_size_in_kilobytes < 4` (`ROGUE_FWIF_SLC_MIN_SIZE_FOR_DM_OVERLAP_KB`), and there is no
  `WARN_ON(PVR_FEATURE_VALUE(...))` in dmesg, so the value is being read successfully and the
  gate is not tripping. Dead.

## Addendum 13b: three more perf suspects eliminated

* **GPU clock** - the open module only ever *reads* it (`clk_get_rate`, no `clk_set_rate`
  anywhere), which looked alarming. It is fine: `clk_summary` shows the `gpu0` clock at
  **1104000000 Hz (1104 MHz)** with parent `pll-gpu`, matching the vendor. The 26000000
  entries are a clk-framework artifact on the *gates* (`gpu0-gate` etc.), not the core clock.
  The DT default is already correct, so nothing needs setting.
* **The `/* Massive copy :( */`** in the PR setup - `pvr_winsys_fragment_state::fw_stream` is
  `uint8_t[440]`, so the copy is ~450 bytes per submit. Negligible; not worth touching.
* **weston re-importing the client buffer per frame** - `PVR_ALLOC_TRACE=1` on weston:
  **0 allocations** over a 20 s client run.

---

# ADDENDUM 14 — 2026-10-08: addendum 10 was WRONG - the 8-bit disable is reverted

**Addendum 10 is superseded. The 8-bit disable was unjustified and has been reverted.**

It was based on static reasoning alone: `extract_u8`/`insert_u8`/`extract_i8`/`insert_i8` have
no cases in `pco_trans_nir.c`, `trans_conv()`'s default is `UNREACHABLE`, and nothing sets
`lower_bit_size`. That reasoning was not tested, and it was wrong.

The bench repo's **`vk16` probe actually exercises 8-bit storage and int8/uint8 arithmetic in
a compute shader**, and every value is correct:

```
results: f16 1.5*2.25*8 = 27 (want 27), i8 -100/3+128 = 95 (want 95), u8 200+100 = 44 (want 44), f16 cmp = 1 (want 1)
ok  int8 division truncates and the value survives (95)
ok  uint8 addition wraps (44)
```

So the 8-bit ops that matter are emitted; the ones I worried about are simply not reached.
`storageBuffer8BitAccess`, `uniformAndStorageBuffer8BitAccess`, `storagePushConstant8` and
`shaderInt8` are restored (mesa `43247f6`), and `vk16` goes 2 failures -> 1 -> **0**.

**This is the second time static absence of a case was mistaken for a bug.** The first was
`shaderFloat16` - except there the failing test (`--validate`: 20/27 wrong) existed, so that
fix stands. The rule: a missing case is a hypothesis, not a finding; run the probe that
exercises it before changing an advertised capability.

## What stands from the audit

| feature | state | evidence |
|---|---|---|
| `shaderFloat16` | **false** (deliberate) | 20/27 glmark2 scenes wrong with it on, 0/27 off; +42.8% mean FPS |
| 8-bit storage, `shaderInt8` | **true** (restored) | `vk16` computes int8/uint8/f16 correctly |
| `shaderInt16`, `shaderInt64`, `shaderFloat64` | false | genuinely unimplemented |
| `drawIndirectCount`, `occlusionQueryPrecise`, `variablePointers`, `vulkanMemoryModel` | false | genuinely unimplemented |

## Bench probes are now part of the loop

`vk16`, `bda`, `vk13`, `pctest` all PASS against the current ICD. They caught a regression the
GL-level tests could not, which is exactly why they exist. `vk16`'s `shaderFloat16`
expectation was stale and has been updated to expect 0 with the reason inline.

---

# ADDENDUM 15 — 2026-10-08: vkaudit diff vs vendor; timestamps scoped; limits under-reported

Ran `vkaudit` against the fixed ICD and diffed with `audit-2026-09-23-vendor.txt`.
`audit-open-driver-current.txt` is refreshed with the post-fix surface.

## timestampPeriod = 0 is CORRECT, not a bug

It looked like a spec violation (vendor: 512.0, open: 0.0), but `pvr_query.c` says explicitly
"We don't currently support timestamp queries. VkQueueFamilyProperties->timestampValidBits = 0",
and `CmdWriteTimestamp2` is a stub that calls `UNREACHABLE`. With no valid bits, the period is
moot. **Checked before changing it** - which is the lesson from addendum 14 working.

**But timestamp queries are a real missing capability (the vendor has them), and they are the
GPU-timing instrument this investigation has been missing all along.** The mechanism is
identified and bounded:

* firmware has a dedicated CCB command: `RGXFWIF_CCB_CMD_TYPE_VK_TIMESTAMP` (223),
  "Process a vulkan timestamp", with `PRGXFWIF_TIMESTAMP_ADDR` - the firmware does the work
* the **mainline kernel module does not have it**: its CCB list stops at 218
  (`pvr_rogue_fwif.h:1542` is the last), so this is a **cross-stack** change (kernel CCB type +
  uapi + Mesa `CmdWriteTimestamp2` + query-pool path + `timestampValidBits`/`timestampPeriod`),
  and testing it needs a module reload.

Scoped, not started: too large to land safely in one round, and it is the single change that
would also unblock profiling the compositor pass.

## Limits the open driver under-reports vs the vendor

| limit | vendor | open |
|---|---|---|
| `maxComputeWorkGroupInvocations` | 512 | **128** |
| `maxComputeWorkGroupSize` | 512/512/64 | **128/128/64** |
| `maxPerStageDescriptorSamplers` | 32 | **16** |
| `maxDescriptorSetSamplers` | 256 | **48** |
| `maxBoundDescriptorSets` | 8 | **4** |
| `core.occlusionQueryPrecise` | 1 | 0 |
| `vk11.variablePointers(-StorageBuffer)` | 1 | 0 |
| `vk12.bufferDeviceAddressCaptureReplay` | 1 | 0 |

Same silicon, so the vendor's numbers are achievable. 128 is exactly the Vulkan *minimum*, which
suggests a floor rather than a hardware limit - but **raising a limit without proving the
hardware path works is the over-claim mistake**, so it needs a probe (a compute dispatch with a
256/512-invocation workgroup) before any change. Not done yet.

The `subgroupSize` difference (vendor 1, open 32) and lower descriptor limits explain why some
applications that work on the vendor stack may refuse to run on the open one.

## Addendum 15b: the push-constants bug does NOT reproduce any more

Two stale notes pointed at it:

* `bda.c:236` - "...they depend on a separate, open driver bug in 64-bit push constant
  handling..."
* `pvranimate` previously reported "a push made BEFORE vkCmdBindPipeline is ignored - the
  driver uploads push constants while setting up the pipeline"

Neither reproduces. `pctest` **PASSES (0 failures)** on every case, including both block
declarations and the partial updates that the bookkeeping would break:

```
uvec4     full 16 bytes at offset 0    PASS
uvec4     4 bytes at offset 0          PASS   got aaaaaaaa 00000000 00000000 00000000
uvec4     8 bytes at offset 8          PASS   got 00000000 00000000 bbbbbbbb cccccccc
uint64_t  full 16 bytes at offset 0    PASS
uint64_t  4 bytes at offset 0          PASS
uint64_t  8 bytes at offset 8          PASS
PASS (0 failures)
```

Code reading agrees: `pvr_cmd_upload_push_consts()` early-returns unless `dirty`, uploads
`bytes_updated` bytes and clears `dirty`; and the draw path calls it for the
vertex/geometry and fragment stages **at the top level of the draw**, not inside the
`dirty.gfx_pipeline_binding` block - so a push both before and after the bind reaches the GPU
on the next draw. `update_push_constants()` correctly keeps `bytes_updated` as the high-water
mark, so partial pushes upload the whole touched range.

Conclusion: these two comments are **stale**; the bug was either fixed in an earlier session or
was misdiagnosed. They should be updated rather than re-investigated. (`bda`'s `--bda-only`
flag still exists for a different reason and is harmless.)

---

# ADDENDUM 16 — 2026-10-08: compute workgroup limit was under-reported 4x; now fixed and matching the vendor

## The finding

`vkaudit` showed the driver reporting limits that were exactly the Vulkan *minimums* while the
vendor reported far more on the same silicon. The most consequential:

| limit | open (before) | vendor | open (now) |
|---|---|---|---|
| `maxComputeWorkGroupInvocations` | **128** | 512 | **512** |
| `maxComputeWorkGroupSize` | **128 128 64** | 512 512 64 | **512 512 64** |
| `maxComputeWorkGroupCount` | 65535 65535 65535 | 65536 65536 65536 | 65535 (unchanged) |

## Probed, not assumed

`wgsize` (`wgsize.c` + `wgsize.comp`) is a compute shader whose every invocation writes
`id ^ 0xa5a5a5a5` to its own slot. The readback therefore proves **both** how many invocations
actually ran **and** that each wrote the correct slot - a shader that silently dropped
invocations would leave zeros, and one that wrote the wrong lane would show a mismatch.

```
local_size_x = 512  ->  512 invocations: 512 correct, 0 untouched(0), 0 wrong
local_size_y = 512  ->  512 invocations: 512 correct, 0 untouched(0), 0 wrong
local_size_z = 64   ->   64 correct, rest untouched (Z is capped at 64, as advertised)
```

So X and Y are 512, Z is 64, and the total is 512 per workgroup - exactly the vendor's numbers.
Applied in mesa `c8523c2`.

## Why under-reporting is not neutral

It is not a conservative choice. An application that needs more than 128 invocations per
workgroup (a common requirement for GPU compute) **refuses to run at all** on the open stack
while working on the vendor stack - a pure compatibility loss for a capability the hardware has.
This is the mirror image of the over-claim bugs: the fix is to report what the hardware does,
in either direction.

## No regressions

`bda`, `vk13`, `pctest`, `vk16` all still PASS; `glmark2 --validate` still 0 failures / 27 pass.
`audit-open-driver-current.txt` refreshed.

Remaining known limit gap: `maxComputeWorkGroupCount` 65535 vs 65536 (Vulkan minimum vs the
vendor). Not changed - it would need a 65536-workgroup dispatch to prove, and the delta only
affects an app needing exactly 2^16 workgroups in one dimension.

---

# ADDENDUM 17 — 2026-10-08: the driver's GPU render path is fast; weston's composite is 19x slower than it

## pvranimate is a real GPU render (verified, not assumed)

I nearly recorded that `pvranimate` fills pixels from the CPU (it calls
`vkGetImageSubresourceLayout` and takes a pitch) - which would have invalidated its 368 Mpix/s
figure as a *CPU* number. Checked before claiming it: **it is a genuine GPU render** -

```
209: vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
214: vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
217: vkCmdDraw(cmd, 3, 1, 0, 0);        /* one full-screen triangle */
```

So the honest GPU-side comparison is:

| workload | Mpix/s | what it is |
|---|---|---|
| `pvranimate` full-screen triangle, 4K, page flip | **368** | native Vulkan graphics, no texturing |
| `vkgears` 800x600 | ~260 | native Vulkan graphics, geometry, no texturing |
| zink off-screen | 90 | zink over the same driver |
| **weston's composite** | **19** | zink, one full-window **textured blend** pass |

**The driver renders a full-screen triangle at 4K at 368 Mpix/s, so its graphics path is not
slow.** weston's composite is 19x slower for a far simpler draw (one quad, 0.48 Mpix of damage),
on the same GPU through zink.

## So the difference is the workload, and the prime suspect is now sampling/blending

`pvranimate` does not sample a texture and does not blend. weston's pass does both. That is the
remaining structural difference between a 368 Mpix/s pass and a 19 Mpix/s pass, and it is
**testable**: render the same full-screen triangle natively with (a) no texture, (b) a texture
sample, (c) a texture sample with blending, and compare. Not yet run.

Note this also re-frames everything earlier: the ~19 ms "present cost" is not a present cost at
all - it is the cost of the compositor's textured blend pass, which is the thing every GL
application waits for.

## Instrument note

`pvr_fw/trace_0` with mask `0xC97` gives init/DM events with firmware timestamps
(`[214] : Core clock set to 1104000000 Hz` - independently confirming the 1104 MHz reading from
addendum 13b) but does not expose per-job durations, so it cannot time the composite pass.
`pvranimate.c:866` is also the source of the earlier "push made BEFORE vkCmdBindPipeline is
ignored" report, which addendum 15b showed no longer reproduces.

## Addendum 17b: it is not texture sampling - it is per-render-pass cost

Ran the discriminating test off-screen (no compositor, no WSI, pure driver render) at 800x600:

| scene | FPS |
|---|---|
| `build:use-vbo=false` (untextured) | 178 |
| **`texture`** | **206** |
| `texture:texture-filter=linear` | 207 |
| `shading:shading=phong` | 105 |
| **`desktop:effect=blur:passes=1:separable=true:windows=4`** | **20** |

**Texture sampling is not the bottleneck - the textured scene is the fastest scene of all.**
The hypothesis from addendum 17 is dead.

What is slow is the **multi-pass** scene: `desktop` (4 windows + a separable blur = many render
passes and FBO ping-pong) runs at 20 FPS while a single-pass untextured scene runs at 178. That
is ~50 ms for a scene that draws almost nothing, i.e. **the cost is per render pass, not per
pixel or per sample**.

This finally gives goal item 3 real weight. Every render submit issues **3.00 hardware jobs**
(geometry + partial-render + fragment) - a TA->3D transition per pass - and that is exactly the
per-pass overhead that a multi-pass workload pays N times over. It is also consistent with
`desktop` being the slowest scene in the full benchmark and with the compositor (one pass per
client frame, ~1.8-5 ms) being a hard ceiling on windowed frame rate.

Attacking the unconditional PR job is therefore the best-justified remaining performance item,
and the open question that stopped it earlier was whether SPM sizing can be determined up front.

---

# ADDENDUM 18 — 2026-10-08: the PR blocker is now definitive; pixel merging tested and rejected

## PR elimination (goal item 3): SPM really is a firmware runtime decision

The blocker was whether the driver can know up front that SPM will not be entered. Traced it:

* `pvr_arch_calc_fscommon_size_and_tiles_in_flight()` computes `max_tiles_in_flight` from **USC
  shared memory** (`available_shareds / (fs_common_size * 2) / num_allocs`), capped at
  `isp_max_tiles_in_flight` (= 6 on bxm-4-64). It is not a partition-store quantity.
* At job setup: if `max_tiles_in_flight == isp_max_tiles_in_flight`, the driver sets
  `job->max_tiles_in_flight = 0` with the comment **"Use the default limit based on the
  partition store"** - i.e. it hands the decision to the firmware, which is exactly when SPM can
  be entered. Otherwise it pins the value and SPM cannot be.

So the driver knows *that it is deferring to the firmware*, but the partition-store capacity the
firmware uses is not a quantity the driver holds. **PR elimination therefore cannot be done
safely from userspace today** - this is now a settled answer rather than an open question, and it
is consistent with the driver's own TODO wording ("in some cases we could eliminate the pr").
Closing this line unless the partition-store capacity becomes available.

## Pixel merging: tested, and rejected on measurement

`pvr_arch_cmd_buffer.c` has `job->disable_pixel_merging = true` with "TODO: Enable pixel merging
when it's safe to do", and the device **has** the enhancement the transfer path gates on
(`has_ern42307 = true`, `pvr_arch_job_transfer.c:3413`). Since the compositor's workload is one
full-window blended quad, this looked like a direct win. It is not:

| scene (off-screen 800x600) | before | with merging enabled |
|---|---|---|
| `build` | 179 | **165** |
| `texture` | 218 | **177** |
| `desktop:blur` | 19 | 20 |

**A regression, so it was reverted** (tree clean, `--validate` still 0/27 failures).

Root cause of why enabling the flag is wrong on its own: the **PPP state already gates** pixel
merging independently. `pds_tri_merge_disable` is set conditionally for lines, punch-through and
DWD-with-depth-always (`pvr_arch_cmd_buffer.c:7277-7282`), and the other two sites
(7285, 7498) build a *mask* with the bit set and then do `merge_word |= state & ~mask` - they
**preserve** the accumulated value rather than disabling it. So the flag and the TA state are two
independent controls, and flipping only the flag puts them in disagreement, which is slower.

The TODO means "enable it on both sides together", not "this flag is mistakenly false". Left
alone deliberately.

---

# ADDENDUM 19 — 2026-10-08: the firmware trace works, and the bottleneck is NOT GPU throughput

This is the GPU-side instrument that was missing for many rounds. It finally works.

## Enabling it

`fw_trace_mask` bits are `ROGUE_FWIF_LOG_TYPE_*` (`pvr_rogue_fwif.h`), so the useful ones are:

```
TRACE = 0x1   GROUP_MAIN = 0x2 (TA/3D kick+finish)   GROUP_SPM = 0x100
echo 0x103 > /sys/kernel/debug/dri/1/pvr_params/fw_trace_mask
cat /sys/kernel/debug/dri/1/pvr_fw/trace_0        # trace_1 is a second thread, often empty
```

Each line is `[<timestamp>] : <event> ... (PID:<pid>, ...)` - **the PID is in the line, so events can
be attributed to the compositor or a client.**

## The timestamp unit

Frame period 24254 units at a measured 178 FPS. The core clock is 1104 MHz, and 1104/256 = 4.3125 MHz.
24254 x (1/4.3125e6) = 5.63 ms -> **177.6 FPS, matching the measurement**. So **1 unit = 256 core
clocks = 232 ns** on this part.

## What the GPU is actually doing (windowed, 800x600, client at 37 FPS = 27 ms/frame)

| | TA | 3D | total |
|---|---|---|---|
| client (PID 201394) | 6808 u = **1.58 ms** | 2645 u = **0.61 ms** | 2.19 ms |
| compositor (PID 147286) | 288 u = 0.067 ms | 11506 u = **2.67 ms** | 2.74 ms |

**Total GPU execution is ~5 ms of a 27 ms frame. The other ~22 ms the GPU is idle.** So the
windowed bottleneck is **not GPU throughput** - it is latency/queueing in the
client -> Xwayland -> compositor -> client loop.

That also explains the two-client result from addendum 9 (19+18 = 37 FPS vs 40 alone) better than a
throughput model did: a latency-bound loop does not speed up by adding clients, and it does not slow
down proportionally either. The per-frame round trip is simply ~27 ms.

Worth noting the compositor's TA is tiny (0.067 ms) but its **3D is 4x the client's** - consistent
with one big blended quad (poor binning efficiency: the whole screen is one huge triangle) versus
the client's many small ones. It is the compositor's most expensive single item, but at 2.67 ms it is
still only 10% of the frame.

## SPM / partial render is not happening

Every single 3D kick in the trace reports **`Partial render:0`**, and there are **no
`RGXFW_SF_MAIN_TA_RESTART_AFTER_PRENDER` events** ("Restart TA after partial render"). So for these
workloads SPM is not being entered at all.

This does not by itself unblock PR elimination (the driver still cannot know in advance), but it does
mean the PR job is pure overhead in the common case - and it moves the PR question down the priority
list, because GPU execution is not the bottleneck anyway.

## Corrected priority

Per-pass cost and the PR job were the leading hypotheses. With GPU execution at ~5 ms of 27 ms, the
remaining ~22 ms is **latency**, and that is where effort should go: the client waits for the
compositor and vice versa. The next measurement is where inside that round trip the time sits
(weston's repaint scheduling, Xwayland's present handling, or swapchain buffer handback).
