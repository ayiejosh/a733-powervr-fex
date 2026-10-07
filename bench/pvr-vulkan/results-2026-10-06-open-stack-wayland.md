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
