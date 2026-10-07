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

---

# ADDENDUM 20 — 2026-10-08: CORRECTION - the addendum-19 timings are not trustworthy

## Retraction

Addendum 19 published per-phase GPU durations ("client TA 1.58 ms, 3D 0.61 ms; compositor TA
0.067 ms, 3D 2.67 ms") and concluded "the GPU is idle ~22 ms of every 27 ms frame". **Those
durations were produced by pairing each `X finished` line with the preceding kick by eye, and
that pairing is invalid.**

The trace is **lossy for finish events**: in a measured window there were **119 `Kick 3D` lines
but only 65 `3D finished` lines - 64 kicks never report a finish**. Pairing by position therefore
attributes a finish to the wrong kick, and the resulting durations are nonsense (a FIFO pairing
pass produced 9761 ms of 3D in a 660 ms window, i.e. 1780% busy).

**So: kick counts from this trace are reliable; kick-to-finish durations are not.** The
"GPU idle 22 ms" conclusion is withdrawn. Correct FIFO pairing was attempted as well and is also
invalid because finishes are simply missing, not merely reordered.

## What IS reliable: kick counts

Kick lines carry `(PID:...)`, and they are all present.

| workload | span | client TA | client 3D | other TA | other 3D |
|---|---|---|---|---|---|
| **off-screen** 800x600 @ 182 FPS | 522 ms | 100 | 101 | - | - |
| **windowed** 800x600 @ 35 FPS | 660 ms | 22 | 22 | 54 (147286) | 119 (147286) |

* Off-screen the client does **exactly 1 TA + 1 3D per frame** (about 95 frames in 522 ms).
* Windowed the client still does **1 TA + 1 3D per frame** - identical render cost.
* But a **second process, PID 147286 = Xwayland** (`ps` confirms: `Xwayland :0 -rootless`,
  parent weston), issues **54 TA + 119 3D in the same window - roughly 5 3D passes per client
  frame.**

## And Xwayland renders even with no X11 client

Running a **native Wayland** client (`weston-simple-egl -b`, no X11 client at all):

```
57 Kick TA  PID:147286      <- Xwayland
57 Kick 3D  PID:147286
26 Kick TA  PID:206610      <- the native Wayland client
27 Kick 3D  PID:206610
 4 Kick TA  PID:147253      <- weston itself
 4 Kick 3D  PID:147253
```

**Xwayland keeps issuing ~57 TA + 57 3D passes on its own**, independent of whether any X client
exists - more render passes than the actual client. That is a stable, reproducible fact and it
looks like the real source of the windowed overhead, not the driver's per-pass cost or the PR job.

(Caveat, stated plainly: without reliable durations I cannot yet say how much GPU *time* that is.
The kick count is the trustworthy half of the measurement. A duration measurement needs a
non-lossy source - a larger trace buffer, or the `frame:` counter, or Vulkan timestamps.)

## Next step

Get a trustworthy duration for the Xwayland passes, because the kick counts say Xwayland is doing
several times the render work of the client while the client is capped near 35-40 FPS. If those
passes are as expensive as the compositor's single pass measured earlier (~2.7 ms), Xwayland alone
would account for a large fraction of the frame.

## Addendum 20b: reliable durations via TA->3D kick pairing

The lossy lines are the `finished` ones. But **both `Kick TA` and `Kick 3D` lines carry `(PID:...)`**,
so pairing `Kick TA` -> next `Kick 3D` *of the same PID* gives a reliable tiling-phase duration, and
consecutive `Kick TA` of the same PID gives the render period. Both halves of those pairs are known
good, so this sidesteps the lossiness entirely.

| workload | PID | TA->3D | period | rate |
|---|---|---|---|---|
| off-screen 800x600 (client 182 FPS) | client | **1.67 ms** | **5.20 ms** | 192/s |
| windowed 800x600 (client 35 FPS) | client | **3.43 ms** | **30.20 ms** | **33/s** |
| windowed 800x600 | **Xwayland (147286)** | 0.30 ms | **12.17 ms** | **82/s** |
| native Wayland client | **Xwayland (147286)** | 0.33 ms | **11.12 ms** | **90/s** |
| native Wayland client | client | 3.36 ms | 23.10 ms | 43/s |

### What this says

1. **Xwayland renders continuously at ~82-90 passes/s, whether or not any X client exists.** It
   does this with a native Wayland client running and no X11 client at all. Its TA->3D is cheap
   (0.3 ms) because it is one full-screen quad, so this is not geometry - it is a repaint loop
   that never stops. On a stack where every pass costs real time, that is continuous GPU work
   serving nothing.
2. **The client's own cost doubles when windowed**: TA->3D goes 1.67 ms -> 3.43 ms
   (off-screen -> windowed) for the *same* scene and size. The scene did not change; contention
   with Xwayland's loop did.
3. The client's frame period goes 5.20 ms -> 30.20 ms, i.e. **5.8x**, purely from being windowed.

The earlier claim that this is all "latency" (addendum 19) was not supported; what is supported is
that a **second process is competing for the GPU continuously** and the client's own phase time
roughly doubles once it is on screen.

### Next step

Find what Xwayland is repainting at 90/s with no clients. If that loop is unnecessary (its root
window should be static), stopping it removes continuous GPU contention and is worth more than any
per-pass micro-optimisation in the driver. Check `xwl_present`/damage handling and whether the root
window is being invalidated every frame.

---

# ADDENDUM 21 — 2026-10-08: RETRACTION - the firmware trace cannot give per-run attribution

**Addenda 19, 20 and 20b are all withdrawn.** The firmware trace is a **persistent ring buffer that
is never cleared**, and there is no reliable way to separate one run's entries from another's.

## The evidence

With **no client of any kind running**, 12 seconds of idle, and a marker taken fresh from the buffer:

```
kick lines total in buffer: 174
kicks after marker while idle: {('221966','TA'):25, ('221966','3D'):25,
                                ('218628','TA'):57, ('218628','3D'):57,
                                ('218561','TA'):3,  ('218561','3D'):3}
```

PID 221966 was the glmark2 client from the **previous** run - it is dead. Its events are still in the
buffer and they pass the `ts > marker` filter.

Why the filter fails: the marker is taken from the **last line** of the buffer, and that last line is
**not** the newest entry - it read 355761 while older surviving entries carry larger values. So buffer
read order is not chronological and a single read cannot be split by timestamp. (`sort -n | tail -1`
is worse: it picked 87921593, a stale value near a wrap, giving "no kicks at all".)

## What this invalidates

* Addendum 19 - per-phase GPU durations (already withdrawn for the separate lossiness reason).
* Addendum 20 - "Xwayland issues 54 TA + 119 3D" and the ~5-passes-per-frame claim.
* Addendum 20b - **"Xwayland renders at 82-90 passes/s even with no X client"**. The clean idle test
  above shows the GPU is genuinely idle when nothing runs; those counts were stale entries. This
  claim was wrong and is retracted.

## The one thing that looked self-validating, and why it is still suspect

The off-screen run gave a single PID with 202 kicks, and a rate computed from it (184/s) that
matched the measured 177 FPS. That is a good sign but **not proof**: if stale entries were included,
both the count and the span inflate together and the ratio can still look right. It is not safe to
lean on.

## Conclusion and next step

**Do not use `pvr_fw/trace_*` for quantitative per-run claims until the epoch problem is solved.**
Options to make it usable, in order of preference:

1. find a way to clear/reset the trace buffer (or to read-and-consume it) - check the firmware trace
   buffer control block in `pvr_fw_trace.c` (it maps `rogue_fwif_tracebuf_space`; the firmware owns
   the write offset, so there may be a host-side offset to resync);
2. read the buffer continuously and diff consecutive reads to isolate new lines;
3. give up on it and implement **Vulkan timestamps** (firmware CCB type 223), which would provide a
   proper, app-controlled GPU timing source.

This round produced no performance finding, but it removed three unsound ones and established the
limit of the instrument - which matters more than a number I cannot trust.

## Addendum 21b: the job-ref epoch filter was tried too, and also fails

Each trace line carries job refs (`ext:0x... int:0x...`), and `int` looked like a usable epoch
counter because the kernel does `OSAtomicIncrement(&psDevInfo->iCCBSubmissionOrdinal)` per
submission. It is not usable across processes:

* idle for 12 s with `int-ref > marker`: **NONE** - looked correct;
* but **off-screen at 182 FPS with `int-ref > 0x75`: also NONE**, i.e. it filtered out a workload
  that was definitely running, and
* the marker itself varies wildly between runs (`0x75` vs `0x03`), because the counter is
  **per-context**, not global.

So both candidate filters fail: timestamps are not chronological in the buffer, and job refs are
per-context. **There is no reliable epoch separator available from userspace for this trace.**

That closes the firmware-trace line for now. It cost three retractions, and the lesson is the same
one as addendum 14: an instrument must be validated before its output is believed - the idle test is
what exposed it, and running it *first* would have saved the whole detour.

The remaining honest options for GPU timing:
1. find a host-side way to reset/consume the firmware trace buffer (it maps
   `rogue_fwif_tracebuf_space`; the firmware owns the write offset);
2. implement **Vulkan timestamps** - firmware CCB type `RGXFWIF_CCB_CMD_TYPE_VK_TIMESTAMP` (223),
   which the mainline module does not have (its CCB list ends at 218) and whose payload the closed
   userspace builds. Cross-stack, but it is the only source that would give an app-controlled,
   unambiguous GPU timestamp.

---

# ADDENDUM 22 — 2026-10-08: sampler limits were under-reported 2x; probed to 128

Same pattern as the compute workgroup limit (addendum 16): the driver reported the **Vulkan
minimum** rather than the silicon's limit.

| limit | open (before) | vendor | open (now) |
|---|---|---|---|
| `maxPerStageDescriptorSamplers` | **16** | 32 | **32** |
| `maxPerStageDescriptorSampledImages` | **16** | - | **32** |
| `maxDescriptorSetSamplers` | 3*16 = 48 | 256 | 3*32 = 96 |
| `maxDescriptorSetSampledImages` | 3*16 = 48 | - | 3*32 = 96 |

## Probed, not assumed

New probe `samplers.c` + `samplers.comp`: an N-element `sampler2D` array where texture *i* is a
1x1 image holding the value *i*, and the shader writes each sampled value to slot *i*. The
readback therefore proves **both** how many samplers are usable **and** that each index reads the
right texture - an array that silently aliased or ran short would show wrong values, not zeros.

```
NS=32    32 correct, 0 untouched, 0 wrong
NS=64    64 correct, 0 untouched, 0 wrong
NS=96    96 correct, 0 untouched, 0 wrong
NS=128  128 correct, 0 untouched, 0 wrong
```

So **at least 128 samplers work per stage and per set** while the driver advertised 16. Set to 32
per stage - vendor parity, comfortably inside what was probed - and 3*32 per set.

No regressions: `bda`, `vk13`, `pctest`, `vk16`, `wgsize` all PASS; `glmark2 --validate` still
0 failures / 27 pass.

## Why this matters for the objective

Under-reporting a limit is not conservative: an application that needs 17-32 samplers in a stage
**refuses to run at all** on the open stack while working on the vendor stack. This is the third
instance of the same defect class found by probing rather than reading code (workgroups 128->512,
samplers 16->32, and the two over-claims in the other direction: fp16 and 8-bit storage).

The general rule now demonstrated four times: **the driver's advertised capability surface must be
measured, not read.** `wgsize` and `samplers` are the two probes that do it; both found the
reported value to be the Vulkan minimum rather than the hardware's.

---

# ADDENDUM 23 — 2026-10-08: storage-image limit was 4 (the Vulkan minimum); probed to 32

| limit | open (before) | open (now) |
|---|---|---|
| `maxPerStageDescriptorStorageImages` | **4** | **32** |
| `maxDescriptorSetStorageImages` | 3*4 = 12 | 3*32 = 96 |

4 is exactly the Vulkan minimum, and unlike `maxBoundDescriptorSets` (= 4, which IS backed by
`PVR_MAX_DESCRIPTOR_SETS` sizing fixed arrays) there is **no backing constant anywhere** for this
one - so it was a floor, not a limit.

## Probed, not assumed

`storageimages.c` + `.comp`: the shader stores index *i* into storage image *i*, then reads each
image back into an SSBO. A single buffer readback therefore proves both that image *i* is
addressable and that it holds the value written to image *i* - aliasing or a short array would
show wrong values, not zeros.

```
NIMG=4    4 correct, 0 untouched, 0 wrong
NIMG=8    8 correct, 0 untouched, 0 wrong
NIMG=16  16 correct, 0 untouched, 0 wrong
NIMG=32  32 correct, 0 untouched, 0 wrong
```

Set to 32 per stage and 3*32 per set, matching the sampler values.

No regressions: `bda`, `vk13`, `pctest`, `vk16`, `wgsize`, `samplers` all PASS;
`glmark2 --validate` still 0 failures / 27 pass.

## The audit is converging on a clear pattern

Four advertised limits have now been measured, and **every one that sat exactly on the Vulkan
minimum turned out to be a floor**:

| limit | reported | measured | outcome |
|---|---|---|---|
| `maxComputeWorkGroupInvocations` | 128 (= min) | 512 | fixed |
| `maxPerStageDescriptorSamplers` | 16 (= min) | >=128 | fixed to 32 |
| `maxPerStageDescriptorStorageImages` | 4 (= min) | >=32 | fixed to 32 |
| `maxBoundDescriptorSets` | 4 (= min) | 4 | **honest** - backed by `PVR_MAX_DESCRIPTOR_SETS` arrays |

The rule that predicts the outcome: **if a reported limit equals the Vulkan minimum AND the driver
has no constant or array sized to it, it is almost certainly a floor.** `maxBoundDescriptorSets`
is the counter-example that proves the test - it is genuinely 4, and changing the number alone
would overrun fixed arrays.

Remaining unprobed minimum-valued limits: `maxPerStageDescriptorInputAttachments` (4),
`maxComputeSharedMemorySize` (16 KB), `maxPerStageDescriptorUniformBuffers` (13, above min 12).

---

# ADDENDUM 24 — 2026-10-08: maxColorAttachments was hardcoded 4 while the driver's own constant says 8

| limit | open (before) | vendor | open (now) |
|---|---|---|---|
| `maxColorAttachments` | **4** | 8 | **8** |

Not a "floor vs limit" judgement call this time - a **hardcoded number contradicting the driver's
own constant**:

```
pvr_limits.h:     #define PVR_MAX_COLOR_ATTACHMENTS PVR_NUM_PBE_EMIT_REGS
rogue_hw_defs.h:  #define PVR_NUM_PBE_EMIT_REGS 8U
pvr_physical_device.c: .maxColorAttachments = 4U          <-- ignored the constant
```

and every array and assert in the render path is already sized for 8:

```
pvr_job_render.h:  uint64_t pbe_reg_words[PVR_MAX_COLOR_ATTACHMENTS];
pvr_job_render.h:  uint64_t pr_pbe_reg_words[PVR_MAX_COLOR_ATTACHMENTS];
pvr_arch_cmd_buffer.c: assert(pbe_emits <= PVR_MAX_COLOR_ATTACHMENTS);
pvr_arch_hw_pass.c:    live_outputs[PVR_NUM_PBE_EMIT_REGS];
pvr_arch_hw_pass.c:    assert(num_live_outputs <= PVR_NUM_PBE_EMIT_REGS);
```

So the code already handled 8; only the advertised number said 4. Now reports 8, matching the
vendor. No regressions in any probe or in `glmark2 --validate`.

**Caveat stated plainly:** no >4-MRT render probe has been written, so unlike the workgroup /
sampler / storage-image fixes this one rests on the driver's own constant, its array sizes and
asserts, and vendor parity - not on a measurement. It is a one-line change to a reported number
that enables a path the driver was already written for.

## Limits audited so far, and which were actually wrong

| limit | reported | verdict |
|---|---|---|
| `maxComputeWorkGroupInvocations` | 128 | **floor** -> 512 (probed) |
| `maxComputeWorkGroupSize` | 128/128/64 | **floor** -> 512/512/64 (probed) |
| `maxPerStageDescriptorSamplers` | 16 | **floor** -> 32 (probed to 128) |
| `maxPerStageDescriptorStorageImages` | 4 | **floor** -> 32 (probed) |
| `maxColorAttachments` | 4 | **hardcoded, contradicted its own constant** -> 8 |
| `maxComputeSharedMemorySize` | 16384 | **honest** - vendor is also 16384 |
| `maxBoundDescriptorSets` | 4 | **honest** - backed by `PVR_MAX_DESCRIPTOR_SETS` arrays |

Five of seven were wrong, in the same direction. The two honest ones are exactly the two that
have a real backing constant or array - which remains the reliable predictor.

## Addendum 24b: the maxColorAttachments fix is now MEASURED

Addendum 24 flagged that the `maxColorAttachments` 4 -> 8 fix rested on static evidence (the
driver's own constant, its array sizes and asserts, and vendor parity) with no probe. That gap is
closed.

`mrt.c` + `mrt.vert`/`mrt.frag` renders into **eight 1x1 colour attachments** with a fragment shader
whose eight outputs each write their own index into red, then reads every attachment back.
Attachment *i* must read back *i*. It uses `VK_KHR_dynamic_rendering` (which the driver reports) so
there is no render pass or framebuffer object in the way.

```
reported maxColorAttachments = 8
vkCreateGraphicsPipelines (8 colour attachments) -> 0
8 attachments: 8 correct, 0 still at clear value, 0 wrong
  attachment 0: ok  red=0 want=0   ...   attachment 7: ok  red=7 want=7
VERDICT: 8 colour attachments render correctly
```

The two negative counters matter: **0 still at clear value** proves attachments 4..7 were genuinely
written (a driver that only bound 4 would leave them cleared), and **0 wrong** rules out aliasing
between attachment indices.

So `maxColorAttachments = 8` is now supported by measurement as well as by the driver's constant,
matching the vendor.

## Addendum 24c: a spec violation between two advertised limits

While checking whether `maxPerStageDescriptorInputAttachments = 4` was another floor, I audited the
**consistency** of the limits struct instead - and found a violation rather than an under-report.

Vulkan requires every `maxDescriptorSet*` to be >= its `maxPerStage*` counterpart. The driver had:

| per-stage | per-set | |
|---|---|---|
| `maxPerStageDescriptorStorageBuffers` = **16** | `maxDescriptorSetStorageBuffers` = **3*4 = 12** | **VIOLATION** |

12 < 16, so an application using the 16 storage buffers per stage that the per-stage limit permits
would have exceeded the per-set limit - **the driver's own two advertised numbers contradicted each
other.** The `3*4` is a stale derivation from when the per-stage value was 4. Now `3*16 = 48`.

All six pairs checked and consistent now: samplers 96/32, uniform buffers 36/13, storage buffers
48/16, sampled images 96/32, storage images 96/32, input attachments 4/4.

No regressions in any probe or in `glmark2 --validate`.

### Method note

This is the third distinct defect class the same discipline has surfaced, and each needed a
different check:

1. **over-claim** - an advertised feature with no implementation (fp16, 8-bit storage) - found by
   running a probe;
2. **under-report** - a limit set to the Vulkan minimum with no backing constant (workgroups,
   samplers, storage images, colour attachments) - found by comparing against a driver constant or
   probing the real capability;
3. **internal contradiction** - two advertised limits that cannot both be satisfied - found by
   checking the limits struct against the spec's invariants, which needs no probe at all.

Class 3 is the cheapest to check and had not been done before now.

---

# ADDENDUM 25 — 2026-10-08: two limits violated Vulkan's required minimums

Class 3 (internal contradictions) generalised: instead of only checking the limits against each
other, dump them and check against **a known-good implementation**. `vkaudit` only prints a subset,
so I added `vlimits.c`, which prints every limit the spec's invariants depend on. With no Vulkan CTS
on this board, **llvmpipe** (an independent implementation that passes conformance) is the reference.

| limit | llvmpipe | open pvr (before) | open pvr (now) |
|---|---|---|---|
| `maxFragmentInputComponents` | **128** | **64** | **128** |
| `maxFragmentCombinedOutputResources` | 104 | **4** | **64** |
| `maxVertexOutputComponents` | 128 | 64 | 64 (legal: minimum is 64) |
| `maxColorAttachments` | 8 | 8 | 8 |
| `maxPerStageResources` | 1000000 | 57 | 57 |

* **`maxFragmentInputComponents = 64` is below the required minimum of 128.** This is not a
  judgement call - the spec mandates >= 128 and llvmpipe reports exactly that.
* **`maxFragmentCombinedOutputResources = 4`** must be at least the sum of
  `maxPerStageDescriptorStorageBuffers` (16) + `maxPerStageDescriptorStorageImages` (32) +
  `maxColorAttachments` (8) = 56. 4 is below `maxColorAttachments` alone. Now 64.

Both were hardcoded with no backing constant. No regressions in any probe or `glmark2 --validate`.

## Updated defect taxonomy

| class | example | how to find it |
|---|---|---|
| **over-claim** | fp16, 8-bit storage | probe the capability |
| **under-report** | workgroups 128, samplers 16, storage images 4, colour attachments 4 | compare to a driver constant, or probe |
| **internal contradiction** | per-set storage buffers 12 < per-stage 16 | check limits against each other |
| **below required minimum** | fragment inputs 64 < 128, combined outputs 4 < 56 | **diff against a known-good implementation (llvmpipe)** |

The fourth class is the newest and the cheapest to apply broadly: `vlimits` + llvmpipe gives a
reference table, and any limit where pvr is *lower* than llvmpipe deserves a look.

## Addendum 25b: maxFramebufferLayers hardcoded 256 while the render path asserts more

Third instance of the "hardcoded value contradicting the driver's own constant" pattern (after
`maxColorAttachments`):

```
pvr_arch_job_render.c:632: assert(layers > 0 && layers <= PVR_MAX_FRAMEBUFFER_LAYERS);
pvr_limits.h:  #define PVR_MAX_FRAMEBUFFER_LAYERS PVR_MAX_ARRAY_LAYERS
pvr_physical_device.c: .maxFramebufferLayers = 256U;      <-- ignored it
```

`PVR_MAX_FRAMEBUFFER_LAYERS` is the same quantity the neighbouring `maxImageArrayLayers` already
takes from `rogue_get_render_size_max_z(dev_info)`, so that is now used for both. **2048** instead
of 256, matching `maxImageArrayLayers`.

Implementation note: using the macro directly does not compile here - it expands to
`ROGUE_TEXSTATE_IMAGE_WORD1_DEPTH_MAX_SIZE`, a csbgen symbol this file does not include. The
device-derived function is both correct and consistent with the line above it.

## Class-4 sweep result: clean after the fixes

Diffing every limit against llvmpipe, **everything where pvr is lower is legal** - llvmpipe reports
above the required minimum in most cases (`maxFramebufferWidth` 8192 vs 16384 where the minimum is
4096, `maxMemoryAllocationCount` 4096 vs ~4 billion where the minimum is 4096, and so on). The only
two that were genuinely below a required minimum were `maxFragmentInputComponents` (64 < 128) and
`maxFragmentCombinedOutputResources` (4 < 56), both now fixed.

Limits that sit exactly on the minimum were checked for backing constants, and the ones that have
one are honest: `maxPushConstantsSize` (`PVR_MAX_PUSH_CONSTANTS_SIZE`),
`maxImageArrayLayers` and now `maxFramebufferLayers` (both `rogue_get_render_size_max_z`).

---

# ADDENDUM 26 — 2026-10-08: native Wayland clients LIVELOCK (X11 works)

## The finding

A native Wayland client on the open stack never presents a frame, while an X11/Xwayland client
through the same weston runs normally.

```
glmark2-es2 (X11/Xwayland)  -s 800x600 : 38 FPS, works
weston-simple-egl -b (native Wayland)  : no frame output at all, runs forever
es2gears_wayland          (native Wayland) : same - no frame output
```

Both clients initialise the GPU (they hold `/dev/dri/renderD128` fds) and then never present.
The client is **not blocked** - it is **spinning**:

```
state=R  wchan=futex_wait_queue      81% of a core
threads: 1 running (futex), 1 running (no wchan), 5 sleeping in futex_wait_queue
fds: /dev/dri/renderD128 x5, /memfd:wayland-cursor
```

That is a **livelock**, not a wait: 81% CPU with the main thread in state R. It reproduces after a
clean weston restart, so it is not a stale-compositor artefact.

## Why this matters

The objective explicitly includes making the open stack work through Wayland. X11/Xwayland works;
native Wayland does not. Any native Wayland application (weston clients, GTK/Qt Wayland apps) is
currently unusable, which also means the Xwayland path is the only working route.

## Cause: not yet determined - do not guess

The difference between the two paths is zink's **kopper backend** (KOPPER_X11 vs KOPPER_WAYLAND),
not the driver's rendering, so the most likely area is `zink_kopper.c`'s Wayland path. But this
must be measured, not assumed, and there is one concrete possibility to rule out first: **this may
be a regression from one of the eleven driver changes made in this session.** The earlier
`weston-simple-egl -b` measurements (153-213 FPS) predate them.

The definitive test is a `git stash` + rebuild + retest to see whether the livelock predates this
session's changes. That has not been run yet, so no cause is claimed.

## Addendum 26b: the Wayland livelock is NOT a regression - it predates this session

Tested by checking out `b288374`, the commit **before** all eleven driver changes made in this
session, rebuilding, and running the same client:

| driver | client | CPU | state | frames |
|---|---|---|---|---|
| `f5ee3cf` (current, 11 changes) | new pid | 81% of a core | R | none |
| **`b288374` (pre-session)** | new pid 273291 | **77% of a core** | **R** | **none** |

**The livelock reproduces identically on the pre-session driver**, so it is a pre-existing bug in
the native Wayland path, not something this session introduced.

(First attempt at this test was invalid and I caught it: `pkill -x weston-simple-` does not match
because `comm` is truncated to 15 characters, so the *old* client was still running and I measured
pid 269817 twice. Killed by PID and re-ran with a fresh pid, which is what the table above shows.)

This also means the earlier `weston-simple-egl -b` figures of 153-213 FPS recorded earlier in the
session were **not** measuring this configuration - a client that never presents a frame cannot run
at 153 FPS. Those numbers should not be used as a Wayland baseline.

Next step is now well-defined: the livelock is in the native Wayland path only (X11/Xwayland works
at 38 FPS), the driver is exonerated, so the area is zink's `KOPPER_WAYLAND` path or the
weston/zink interface - and it is a hard blocker for every native Wayland application.

## Addendum 26c: the livelock is a SPIN in zink_flush's sync_flush, on flush_completed

Instrumented both `util_queue_fence_wait` calls reachable from `zink_flush` with an env-gated
`ZINK_FENCE_TRACE` (which reports whether the fence is signalled at the moment of the wait):

```
[zf] flush_batch: waiting on unsync_fence  (signalled=1)   <- healthy
[zf] sync_flush:  waiting on flush_completed (signalled=0)  <- the stall
...
4898 iterations in ~8 s  (~612/s)
```

So the visible stall is `sync_flush()` waiting on `bs->flush_completed` - the flush-queue job
completion fence - **with the fence not signalled**, and the client iterates it ~612 times a
second, which is the 78% CPU.

### A real bug found and fixed along the way (but not this one)

`zink_copy_image_buffer()` resets `ctx->unsync_fence` when `unsync` is set and signals it at the
end, but had an early return in between:

```c
   if (unsync) { ... util_queue_fence_reset(&ctx->unsync_fence); }
   if (buf2img) {
      if (zink_is_swapchain(img)) {
         if (!zink_kopper_acquire(ctx, img, UINT64_MAX))
            return;                       /* <-- left unsync_fence reset forever */
      }
   ...
   if (unsync) util_queue_fence_signal(&ctx->unsync_fence);
```

Left unfixed, the next `flush_batch()` blocks in `util_queue_fence_wait(&ctx->unsync_fence)`
with no recovery path. Fixed (signal before returning). **It did not resolve the observed hang** -
the trace shows `unsync_fence` is signalled on every check - but it is a genuine bug and is kept.

### What is now known, and what is not

* The stall is in `sync_flush` on `flush_completed`, i.e. the flush-queue worker is not
  completing the batch job promptly enough (or at all) for the main thread.
* `unsync_fence` is healthy.
* Disabling `threaded_submit` (`ZINK_DEBUG=flushsync`, `GALLIUM_THREAD=0`) does **not** fix it.
* Not yet established: whether the worker completes the job and the client simply never presents
  (frame callbacks), or whether the worker genuinely stalls. The worker thread was observed
  earlier inside `submit_queue -> reset_batch_state_internal -> pvr_cmd_buffer_reset ->
  pvr_bo_free -> drmIoctl`, i.e. running, which points at the former.

Next step: instrument `submit_queue` to confirm the job completes and the fence is signalled, and
check whether weston sends frame callbacks to the client at all.

---

# ADDENDUM 27 — 2026-10-08: RETRACTION - native Wayland is FINE. Addenda 26/26b/26c were my error.

**Native Wayland works. There is no livelock.** Measured over a known 5-second wall-clock window by
counting `wl_surface.commit` in the Wayland protocol trace:

| native Wayland client | frame rate |
|---|---|
| `weston-simple-egl -b` (interval 0, uncapped) | **301 fps** |
| `weston-simple-egl` (interval 1, vsync) | **60 fps** |

60 fps on a 60 Hz output with vsync on, 301 fps uncapped. That is a healthy Wayland path.

## How I got it wrong - three compounding measurement errors

1. **Wrong grep character.** I searched for `wl_surface@N.commit` and `wl_callback@N.done`. The
   protocol trace prints `wl_surface#N.commit`. So my "0 commits, 0 frame callbacks" conclusion -
   the entire basis of the "never presents a frame" claim - was a bad regex. The trace actually
   contained **2055 commits and 2051 buffer releases**.
2. **Wrong output format.** I grepped for `frames in N seconds = X fps`. The client prints
   `frames in N seconds: X fps` - a colon. So "no fps output" was also a bad pattern.
3. **Misread CPU as a hang.** 78% CPU with the main thread in state R is exactly what
   `weston-simple-egl -b` is *for*: `-b` means "Don't sync to compositor redraw (eglSwapInterval 0)",
   i.e. deliberately uncapped. Rendering at 300 fps costs CPU. I read it as a spin.

And the `gdb` backtrace - which looked like strong evidence - was a truthful snapshot of
`eglSwapBuffers` inside `zink_flush`, but that is simply where an uncapped client spends its time.
A real stack trace in a normal code path is not evidence of a bug.

## Consequences

* **Addenda 26, 26b and 26c are withdrawn**, including "X11 works, Wayland does not". If anything
  Wayland is the *better* path here: 60 fps vsync / 301 fps uncapped, against 39-42 fps for the
  X11 glmark2 runs (different scenes, so not a like-for-like comparison, but certainly not a
  Wayland deficit).
* The earlier `weston-simple-egl -b` figures of 153-213 fps recorded earlier in the session were
  **right**, and my addendum-26b claim that they "cannot have measured this configuration" was
  wrong.
* The `zink_copy_image_buffer` early-return fix (mesa `863bd72`) **stays**: an unbalanced
  `util_queue_fence_reset` is a genuine bug regardless of how it was found. It was not the cause of
  anything observed.
* `ZINK_FENCE_TRACE` instrumentation stays; it is env-gated and it did correctly report fence
  states, it just answered a question that was not the real one.

## The lesson, which is now the fourth instance this session

Every one of these was a measurement error, not a driver bug:

| round | false claim | actual error |
|---|---|---|
| 8 | 8-bit storage cannot compile | static absence of a case, never probed |
| 15 | firmware trace gives per-run attribution | ring buffer persists; no epoch separator |
| 20-22 | native Wayland clients livelock | **two bad grep patterns and a misread CPU figure** |

**Rule: before concluding "X never happens", verify the pattern you are counting actually matches
the format you are counting.** A zero count from a regex is not evidence until the regex is shown
to match a known-present example.

## Addendum 27b: a reliable frame-rate method, and the Wayland baseline

Client stdout on this stack is not trustworthy for frame rate - several clients print nothing at
all (`es2gears_wayland`, `es2gears_x11`), and formats differ between clients. **Count the protocol
commits instead**, which cannot be missed or mis-formatted:

```sh
WAYLAND_DEBUG=1 weston-simple-egl -b > trace 2>&1 &
sleep 3                       # let it initialise
N0=$(grep -cE 'wl_surface#[0-9]+\.commit' trace)
sleep 5                       # measured window
N1=$(grep -cE 'wl_surface#[0-9]+\.commit' trace)
echo $(( (N1-N0)/5 )) fps
```

Use `#`, not `@` - the trace prints `wl_surface#15.commit()`. And check `wl_buffer#N.release`
alongside it: commits show what the client sent, releases show weston actually consuming them.

### Native Wayland baseline (measured this way)

| client | mode | frame rate |
|---|---|---|
| `weston-simple-egl` | `-b` (interval 0) | **301 fps** |
| `weston-simple-egl` | interval 1 (vsync) | **60 fps** |
| `es2gears_wayland` | vsync | **30 fps** |

`es2gears_wayland` at 30 fps on a 60 Hz output means it misses every other vblank - its frame
costs more than 16.6 ms. That is a real (if modest) performance observation and it is consistent
with the 22 fps recorded for `es2gears_x11` earlier, i.e. Wayland is the faster of the two paths
here, not the slower one.

So the picture is the opposite of addendum 26: **Wayland is healthy and is the better path**, and
the X11/Xwayland route is where the extra cost lives.

---

# ADDENDUM 28 — 2026-10-08: the windowed penalty is damage-area x 56 ms/Mpix, scene-independent

## The measurement

Same size (800x600), same window, only the scene varied. `glmark2-es2` reports FPS reliably (unlike
`es2gears`, which prints nothing), so both numbers come from the client itself:

| scene | windowed | off-screen | off-screen frame time |
|---|---|---|---|
| `build` | 37 | 185 | 5.4 ms |
| **`texture`** | **37** | **248** | 4.0 ms |
| `phong` | 34 | 104 | 9.6 ms |
| `desktop` blur | 16 | 22 | 45 ms |
| `terrain` | 4 | 5 | 200 ms |

**`texture` renders *faster* off-screen than `build` (248 vs 185 fps) yet both land on exactly
37 fps windowed.** The windowed rate is therefore not set by the client's render work at all.

## The model

Subtracting off-screen from windowed frame time gives the presentation overhead:

```
build    21.6 ms     phong   19.8 ms
texture  23.0 ms     desktop 17.0 ms
```

Almost constant at 800x600 despite a 50x spread in render cost. Adding a 320x240 point
(3.6 ms overhead, 0.0768 Mpix) gives a clean linear fit in **damage area**:

```
overhead = -0.8 ms + 56.4 ms/Mpix        (r^2 effectively 1 on the 800x600 cluster)
        => compositor throughput ~17.7 Mpix/s = 71 MB/s
```

## Cross-validation

The fit was made from glmark2/X11 data. Using it to predict a *different* client, window size and
API path - `weston-simple-egl` on **native Wayland**, ~250x250 (~0.0625 Mpix), which measures
3.3 ms/frame (301 fps):

```
predicted: -0.8 + 56.4*0.0625 + ~1 ms client render = 3.8 ms/frame = 266 fps
measured : 3.3 ms/frame = 301 fps
```

13% agreement across an independent path. That is the first model of this cost that predicts an
unseen measurement rather than just fitting the data it came from.

## What this means for objective item 1

The "~28 ms present cost" is **damage area x 56 ms/Mpix**, i.e. the compositor's composite running
at **~18 Mpix/s / 71 MB/s**, and it is **scene-independent** - it is not the client's render, not
the driver's per-pass cost, and not the partial-render job. For comparison, the driver itself
renders a full-screen triangle at 4K at 368 Mpix/s (`pvranimate`, verified as a real GPU render in
addendum 17), so the composite is ~20x slower than the hardware's demonstrated capability.

That makes the question sharp and narrow: **why does one full-window textured blend pass run at
18 Mpix/s when the same GPU does 368 Mpix/s on a full-screen triangle?** Everything else on the
windowed path has now been excluded by measurement.

## Addendum 28b: 7-point fit, and CORRECTION - the cross-validation in 28 does not hold

Swept the window size with the `build` scene, windowed vs off-screen, `glmark2-es2` reporting FPS:

| size | Mpix | overhead |
|---|---|---|
| 320x240 | 0.077 | 6.9 ms |
| 480x360 | 0.173 | 13.0 ms |
| 640x480 | 0.307 | 12.2 ms |
| 800x600 | 0.480 | 20.8 ms |
| 1024x768 | 0.786 | 35.7 ms |
| 1280x720 | 0.922 | 37.3 ms |
| 1600x1200 | 1.920 | 63.9 ms |

Least squares over all seven:

```
overhead = 6.30 ms + 31.2 ms/Mpix        r^2 = 0.978
        => damage-area throughput ~32 Mpix/s = 128 MB/s, plus a 6.3 ms fixed cost
```

**Correction to addendum 28:** the 56.4 ms/Mpix figure there came from a two-point fit (the 800x600
cluster plus 320x240) and it happened to predict `weston-simple-egl` well. With the seven-point fit
that agreement **disappears** - this model predicts ~108-150 fps for `weston-simple-egl` against 301
measured. So **the cross-validation claimed in addendum 28 is withdrawn**; the model fits glmark2's
own windowed/off-screen pairs (r^2 = 0.978, which is real) but has not been shown to predict a
different client.

The likely reason, which is testable and not yet tested: the relevant quantity is the **damaged**
area, not the window area. `weston-simple-egl` draws a rotating triangle, so weston only composites
the triangle's bounding box, while `glmark2` damages essentially its whole window. That would
reconcile the numbers - but it is an explanation, not evidence, until weston's actual damage region
is measured.

So the honest statement of what is established: **for a client that damages its whole window, the
windowed penalty is ~6 ms plus 31 ms per Mpix of damage, essentially independent of the scene's
render cost.** The fixed 6.3 ms component is new information - it is too large to be noise and too
small to be a vblank, and it is a candidate for the round-trip latency of the
client -> Xwayland -> weston -> client handoff.

## Addendum 28c: the model does NOT generalise - scope it or drop it

Tested the damage-area explanation against two other clients. It fails on both.

| client | damage | measured | model predicts |
|---|---|---|---|
| `weston-simple-egl` (rotating triangle, interval 0) | triangle bbox | **301 fps** | 108-150 fps |
| `weston-simple-shm` (full window, interval 1) | 210x210 = 0.0441 Mpix | **30 fps** | 130 fps |

`weston-simple-shm`'s damage region is visible in the protocol trace (`wl_surface#3.damage(20, 20,
210, 210)`), so this is not a guess about what it damages - it damages its whole window every frame,
and it still runs at 30 fps against a predicted 130.

So the fit in addendum 28b is **real for the data it was fitted to** (glmark2's windowed vs
off-screen pairs, r^2 = 0.978 over seven sizes) but it is **not a general law of the windowed path**,
and the damage-area hypothesis is **not confirmed**. Two independent clients contradict it in
opposite directions.

The honest statement is therefore narrower than addendum 28 claimed:

> For `glmark2` at 800x600 and other sizes, the difference between its windowed and off-screen frame
> rates is described by 6.3 ms + 31.2 ms/Mpix of window area. That is a property of glmark2's
> windowed path, and it does not transfer to `weston-simple-egl` or `weston-simple-shm`.

What survives from all of this: the windowed rate for glmark2 is **scene-independent** (texture
renders faster off-screen than build, yet both are 37 fps windowed), which remains a solid and
useful observation because it rules out the client's own render cost as the limiter. The *mechanism*
is still unidentified, and three attempts to model it (area-proportional, vblank-paced, damage-area)
have now each failed a cross-check.

**Lesson, and it is the same one as addenda 21 and 27:** a model that fits its own data is not a
finding until it predicts a measurement it was not fitted to. Both of addendum 28's cross-check
claims have now been withdrawn on exactly that ground.

---

# ADDENDUM 29 — 2026-10-08: weston does almost no host work; the cost is a shared serialising resource

## Weston's syscall time, measured directly

`strace -T -p <weston>` during a windowed 800x600 client run (8 s window, client at 36 FPS):

| syscall | calls | total time | share |
|---|---|---|---|
| **`epoll_pwait`** | 507 | **7.403 s** | **98%** |
| `ioctl` (GPU) | 564 | 0.074 s | 1% |
| `timerfd_settime` | 506 | 0.017 s | - |
| `sendmsg` / `recvmsg` | 253 / 256 | 0.014 / 0.010 s | - |
| `read` | 126 | 0.004 s | - |

**Weston spends 98% of its syscall time waiting in `epoll_pwait` and 1% in GPU ioctls.** Combined
with its CPU being 3-6% (addendum 12), weston does almost no host work at all.

**Stated limitation, because it matters:** GPU work is submitted *asynchronously* - the ioctl returns
after handing the work to the GPU, and execution happens afterwards. So "1% in ioctl" does **not**
prove the GPU is idle, and this measurement cannot separate "weston waits for the GPU" from "weston
waits for the client". It does rule out weston being busy on the host, which is what it was taken
for.

## The constant-latency model fails

Tested `windowed_ms = offscreen_ms + c` (one handoff latency) against the full scene table:

| scene | win ms | off ms | c |
|---|---|---|---|
| build | 27.0 | 5.4 | 21.6 |
| texture | 27.0 | 4.0 | 23.0 |
| phong | 29.4 | 9.6 | 19.8 |
| desktop | 62.5 | 45.5 | 17.0 |
| terrain | 250.0 | 200.0 | 50.0 |

c ranges 17-50 ms, and across the size sweep it ranges 6.9-63.9 ms. **Not constant** - the added cost
is genuinely area-dependent, which is what addendum 28b's fit captured. So the fourth model also
fails in its simple form.

## What does hold, and the strongest clue so far

* The windowed rate for `glmark2` is **scene-independent** (texture renders faster off-screen than
  build; both are 37 FPS windowed). The client's own render cost is not the limiter for light scenes.
* **Two concurrent clients do not halve each other.** Round 9 measured one client at 40 FPS and two
  at 19 + 18 = **37 total**. If a shared resource were divided, the total would stay ~40 and each
  would get ~20 - but the *total* stayed at one client's rate. That is the signature of a **shared
  serialising resource** consumed once per composite, not a bandwidth split.
* Display mode verified as **3840x2160@60** (previously assumed, now checked): `3840x2160@60.0,
  preferred, current, 533.1 MHz` in the weston log.
* Weston's repaint timer is armed with absolute `timerfd` times ~184-218 ms apart in the sample,
  which does not correspond to a 60 Hz cadence and does not cleanly give a repaint period.

## Where this leaves it

Five attempts to model the windowed penalty have now been made (area-proportional, vblank-paced,
damage-area, constant-latency, plus the scene-independence observation). The one thing every
measurement agrees on is that **weston does very little work and something serialises at ~37-40
composites/s when a client damages its whole window**. The next measurement has to be *inside*
weston's repaint path rather than around it - weston is stripped, so that means building weston with
instrumentation, or using its `weston-debug` facility if it exposes a repaint timeline.

---

# ADDENDUM 30 — 2026-10-08: weston is EXONERATED. The ~20 ms is client-side.

This is the measurement that should have been taken many rounds ago: `weston-debug timeline` gives
weston's repaint event points with nanosecond timestamps, i.e. the view from *inside* the compositor
rather than inferred from client frame rates.

```sh
# weston must be started with --debug
weston-debug --list                     # log, scene-graph, timeline, proto, drm-backend, gl-renderer
weston-debug timeline -o /tmp/tl.txt &  # JSON lines: {"T":[sec,nsec],"N":"core_repaint_begin",...}
```

## The result

Parsed from 1262 events during a windowed 800x600 run (client at 36 FPS):

| term | samples | median |
|---|---|---|
| weston's composite (`core_repaint_begin` -> `core_repaint_posted`) | 170 | **0.58 ms** |
| flip wait (`core_repaint_posted` -> `core_repaint_finished`) | 170 | 6.88 ms |
| commit -> on screen (`core_commit_damage` -> `core_repaint_posted`) | 117 | 9.61 ms |
| **weston-owned total (composite + flip)** | | **7.46 ms** |
| client's actual frame time at 36 FPS | | **27.8 ms** |
| **not weston's** | | **~20.3 ms** |

**Weston composites a full-window frame in 0.58 ms.** Its entire owned cost - composite plus waiting
for the flip - is 7.46 ms, which would support **134 composites/s**. The client only achieves 36 FPS.
So **~20 of the 27.8 ms is spent outside the compositor**, on the client side.

Weston's own timeline confirms it: the largest single gap is
`core_repaint_exit_loop -> core_commit_damage` = **37.1 ms median**, i.e. weston finishes a repaint
and then sits waiting for the client's next commit.

## This corrects addenda 9, 12 and 17

Those concluded "the compositor's composite is the shared bottleneck" from indirect reasoning:
weston's CPU being flat while client FPS fell, and the cost scaling with damage area. The CPU-flat
observation was right, but the inference was wrong - weston is idle because it is *fast*, not because
it is GPU-bound. Addendum 12 even said "so it is GPU-bound, not CPU" on the strength of a CPU
measurement, which cannot establish that.

Direct measurement beats inference: 0.58 ms of composite work cannot be the 20 ms that is missing.

## Where the ~20 ms is

Client-side, and almost certainly the swapchain wait: the client cannot begin its next frame until it
has a free swapchain image, and that depends on the compositor releasing the previous one. This is
consistent with the two-client result from round 9 (one client 40 FPS, two clients 19+18 = 37 total -
each waits on its own buffer, and the totals do not halve because they are not sharing bandwidth).

Next measurement, now narrow: instrument zink's `kopper` Wayland path - `zink_kopper_acquire`,
the frame-callback wait, and `vkAcquireNextImageKHR` - to see which of them accounts for ~20 ms.
The driver's own render is 0.58 ms of composite on the same GPU, so this is a zink/WSI question.

---

# ADDENDUM 31 — 2026-10-08: the windowed penalty is in the CLIENT'S DRAW PATH

Instrumented `kopperSwapBuffersWithDamage` (`src/gallium/frontends/dri/kopper.c`) with an
env-gated `SWAP_TIMING` that reports the frame period and the time spent inside `eglSwapBuffers`.
That separates "waiting in present" from "rendering" without any inference.

## Result

```
[swap] frame N: period 26.30 ms, inside swap 4.90 ms, outside 21.40 ms
```

| term | median |
|---|---|
| inside `eglSwapBuffers` (present path) | **4.34-4.90 ms**, tightly bounded (3.95-8.29) |
| glmark2's own frame time | **26.3 ms** |
| **outside the swap call - the GL draw path** | **~22 ms** |

So of a 26.3 ms windowed frame: **~4.5 ms is the present call and ~22 ms is the client's own draw
path.** Combined with addendum 30 (weston's composite is 0.58 ms), the whole windowed penalty is now
localised:

| component | cost |
|---|---|
| weston's composite | 0.58 ms |
| flip wait | 6.88 ms (weston-owned, overlapped) |
| client's `eglSwapBuffers` | 4.5 ms |
| **client's GL draw path** | **~22 ms** |
| same scene **off-screen** | **5.4 ms total** |

**The client's draw path is ~4x more expensive when rendering into a swapchain image than into an
off-screen image.** That is the thing to attack, and it is a driver/zink interaction, not a
compositor or WSI problem.

## Incidental finding: dead code on the kopper path

`kopperSwapBuffersWithDamage` **always** returns at its "no front texture" check - 391 of 391 calls:

```c
   if (!drawable->textures[ST_ATTACHMENT_FRONT_LEFT]) {
      return 0;                                  /* always taken for kopper drawables */
   }
   /* have to manually swap the pointers here to make frontbuffer readback work */
   drawable->textures[ST_ATTACHMENT_BACK_LEFT] = drawable->textures[ST_ATTACHMENT_FRONT_LEFT];
   drawable->textures[ST_ATTACHMENT_FRONT_LEFT] = ptex;
```

`ST_ATTACHMENT_FRONT_LEFT` is never populated for a kopper drawable, so the pointer swap never
happens. Not necessarily a bug - the present already happened in `kopper_copy_to_front` above it -
but it is why the first attempt at this instrumentation produced no output at all: the print was
placed after that return. **When instrumentation prints nothing, check for an early return before
concluding the function is not called.**

## Next

Compare the client's draw cost rendering into a swapchain image vs an off-screen image of the same
size, to find why the former is ~4x more expensive. Candidates: a per-frame layout transition on the
swapchain image, a different image layout/tiling for swapchain images, or extra synchronisation in
zink's render-to-swapchain path.

---

# ADDENDUM 32 — 2026-10-08: tiling and copy ruled out; ppoll is the discriminator

Following addendum 31 (the windowed penalty is the client's draw path, ~22 ms of 26.3 ms), the two
obvious explanations for "the same scene is 4x slower into a swapchain image" were tested and both
are **ruled out**.

## Ruled out: the swapchain image's tiling

New env-gated `SWAPCHAIN_INFO` in `zink_kopper.c` prints the render target's properties at the point
where the swapchain image is bound:

```
[swi] swapchain target: fmt=105 800x600 linear=0 modifiers=0 m0=0x0 vkusage=0x97 layout=0
```

**`linear=0` - it is `VK_IMAGE_TILING_OPTIMAL`, with no DRM modifiers**, i.e. the same tiling class
as an off-screen render target. So the old "only LINEAR modifiers are supported, and rendering into
a LINEAR image is slow" theory does not apply here.

## Ruled out: an extra copy

`zink_kopper.c:650` binds the swapchain image directly:

```c
res->obj->image = cdt->swapchain->images[res->obj->dt_idx].image;
```

The display-target resource *is* the swapchain image, so there is no blit or copy from an internal
render target. The `kopper_copy_to_front` on the swap path is the present, and it costs 4.5 ms
(addendum 31), not 22.

## New discriminator: `ppoll`

`strace -f -c` on the client, windowed vs off-screen:

| syscall | windowed | off-screen |
|---|---|---|
| **`ppoll`** | **1274 calls, 1.52 s (1193 us avg)** | **136 calls, 0.8 ms** |
| `ioctl` | 29740 calls, 0.74 s | 65188 calls, 2.36 s |
| `futex` | 6806 calls, 11.4 s | 13505 calls, 44.6 s |

`ppoll` is essentially absent off-screen and substantial windowed - the only syscall that behaves
that way. **Caveat, stated because it burned me before: `strace -f -c` sums per-call time across all
threads, so the futex totals are dominated by idle worker threads and are NOT comparable between the
two runs** (off-screen's futex total is larger simply because it ran 4x more frames). Only the
`ppoll` presence/absence is meaningful, and it needs per-thread attribution before it can be
interpreted - which is exactly the mistake made in addendum 20, where an idle WSI event thread's
`ppoll` was mistaken for the main thread blocking.

## Next

Attribute the windowed `ppoll` to a thread: if it is the main render thread, it is the client waiting
on something (X connection, fence, or frame callback) and that is the 22 ms; if it is the WSI event
thread again, it is idle background behaviour and must be discarded as it was in addendum 20. Use
`strace -f` without `-c` and filter by TID rather than aggregating.

## Addendum 32b: ppoll attributed per thread - it is NOT the 22 ms either

The `-c` run suggested ppoll was windowed-specific (1.52 s vs 0.8 ms). Re-ran with
`strace -f -e trace=ppoll -T` (no `-c`) and attributed every call to its TID:

| TID | calls | total | avg | max | fds polled |
|---|---|---|---|---|---|
| 336441 | 10 | 134.6 ms | **13.5 ms** | 59.1 ms | fd 3 |
| **336419** (main - also does ioctl on fd 7) | 512 | 119.2 ms | 233 us | 33.7 ms | fd 3, **fd 7** |
| 336483 | 10 | 31.1 ms | 3.1 ms | 15.7 ms | fd 3 |
| 336482 | 306 | 9.4 ms | 31 us | 1.2 ms | fd 3 |
| 336440 | 28 | 2.6 ms | 93 us | 2.0 ms | fd 3 |

**Total ppoll across all threads is ~300 ms in a 10 s run (~3%).** The main thread (336419, the one
that also does DRM ioctls on fd 7) spends only 119 ms in 512 ppoll calls, averaging 233 us. So
**ppoll is not the missing 22 ms** and the discriminator from addendum 32 does not survive
attribution.

Note the 5x discrepancy: the `-c` aggregate reported 1.52 s for the same syscall. That is the
per-thread-summation trap again, in a different guise - `-c` totals are not wall-clock time and
should not be compared between runs of different length.

## Where this leaves the windowed penalty

Narrowed, with several things now positively excluded by measurement rather than inference:

| excluded | how |
|---|---|
| weston's composite | 0.58 ms (weston timeline) |
| the present call | 4.5 ms (SWAP_TIMING) |
| swapchain image tiling | `linear=0`, OPTIMAL, no modifiers (SWAPCHAIN_INFO) |
| an extra copy to the swapchain | `res->obj->image` *is* the swapchain image |
| `ppoll` / client syscalls | ~3% of runtime, main thread 119 ms/10 s |

What remains is that the client's own GL draw path costs ~22 ms windowed against 5.4 ms for the same
scene off-screen, and none of the obvious mechanisms account for it. The next measurement should be
inside the draw path itself - per-frame timing of zink's draw/flush/batch-submit on the windowed
path versus off-screen - rather than around it.

---

# ADDENDUM 33 — 2026-10-08: ruled out batch_usage_wait; and a flaw in my own thread attribution

## Ruled out: `batch_usage_wait` (resource reuse in the draw path)

Instrumented `batch_usage_wait` in `zink_batch.c` (the function that calls `zink_wait_on_batch`)
with an env-gated `BATCH_WAIT_TRACE` that counts and times every call. Ran both configurations for
18 s:

```
win: fps=42  waits=<500
off: fps=180 waits=<500
```

**No `[bw]` line was ever printed**, and the counter only reports every 500 calls - so
`batch_usage_wait` is called fewer than 500 times in 18 seconds in either configuration. It is not
where the client stalls. Ruled out.

## The client is waiting, not spinning - but my attribution was wrong

| | windowed | off-screen |
|---|---|---|
| FPS | 36 | 174 |
| **client CPU (all threads)** | **25% of a core** | **67% of a core** |
| threads | 9 | 11 |

Windowed the client uses *less* CPU, so it is waiting rather than spinning - consistent with the
~22 ms of "outside swap" time from addendum 31.

But the main-thread-only `strace -c` (no `-f`) showed only **0.80 s of syscall time in a 12 s run
(6.7%)** windowed, with `futex` at 3.3%. **A thread that is blocked for 21 ms of every 28.6 ms frame
must be inside a syscall most of the time.** It isn't, so the main thread is not the thread doing the
waiting - the client runs **9 threads**, and I was profiling the wrong one.

**That invalidates the thread attribution in addenda 32/32b as a basis for conclusions about the
client's blocking** - not the measurements themselves, but the assumption that the main thread
carries the frame loop. The next attempt has to identify the render thread first (e.g. by finding
which TID issues the DRM ioctls, or which one accumulates the frame's CPU time) and profile *that*
one, rather than assuming TID==PID.

## Method note, and it is the recurring one

Sixth instance of the same class this session: **the proxy was wrong, not the code.**

| round | what I measured | why it misled |
|---|---|---|
| 20-22 | client stdout / CPU state | bad grep patterns; `-b` means uncapped |
| 15 | firmware trace per-run attribution | persistent ring buffer |
| 24 | weston CPU flat | inferred "GPU-bound" from a CPU number |
| 26 | `strace -f -c` totals | per-thread summation, not wall clock |
| **27** | **main-thread syscall profile** | **the main thread is not the render thread** |

Each time the fix was the same: measure the specific thing being claimed, and verify the
measurement instrument actually observes it.

## Addendum 33b: the windowed path is WSI-sync-heavy, but that is not 22 ms either

Comparing the ioctl mix (`strace -f -e trace=ioctl`, then counting by name):

| ioctl | windowed (32 FPS) | off-screen (121 FPS) |
|---|---|---|
| **`DRM_IOCTL_SYNCOBJ_TRANSFER`** | **3810** | **absent from the top 6** |
| `SYNCOBJ_DESTROY` | 3550 | 3655 |
| `SYNCOBJ_CREATE` | 3532 | 3643 |
| `PVR_VM_UNMAP` / `PVR_VM_MAP` | 2445 each | 9506 each |
| `HL_CB` | 2288 | 8902 |

`DRM_IOCTL_SYNCOBJ_TRANSFER` is **windowed-only**, and it comes from
`src/util/u_sync_provider.c:100` (`drmSyncobjTransfer`) - i.e. **Mesa's generic sync provider used by
the WSI swapchain**, not from anything pvr-specific. Per frame that is ~12 transfers, plus ~11
syncobj creates/destroys windowed against ~3 off-screen.

**But it does not account for the 22 ms:** each ioctl averages 29 us in the same strace, so ~12 per
frame is roughly 0.35 ms. The windowed path genuinely is WSI-sync-heavy, and that is a real
difference between the two paths worth knowing, but it is two orders of magnitude short of the
missing time.

## Correcting addendum 33's attribution claim

Addendum 33 said "the main thread is not the render thread". That was **wrong**. Counting ioctls per
TID:

| TID | ioctls | notable |
|---|---|---|
| 344654 (**= process PID, main thread**) | 11417 | `PVR_VM_MAP` 2626, `HL_CB` 2457, `SYNCOBJ_CREATE` 1050 |
| 344656 | 17597 | `SYNCOBJ_TRANSFER` 3423, `SYNCOBJ_DESTROY` 3078, `SYNCOBJ_CREATE` 2745 |

The main thread does 11417 DRM ioctls, so it *is* a render thread. The real problem with the
main-thread-only profile was different: its ioctls are **fast** (29 us average), so 11417 of them is
only ~0.33 s in a 12 s run. The main thread is not blocked in syscalls because the *waiting* happens
elsewhere - most likely on the WSI syncobjects being transferred, which are handled through the
second thread and the sync provider.

## State of the search

Excluded by measurement: weston's composite (0.58 ms), the present call (4.5 ms), swapchain tiling,
an extra copy, `batch_usage_wait`, client `ppoll` (~3%), and now the WSI syncobj churn (~0.35 ms).
Still unaccounted: ~22 ms of the client's 26.3 ms windowed frame.

The remaining candidates are all in the client's GL/draw path, and the next instrument has to time
zink's own draw and batch-submit functions per frame rather than looking at syscall traces.
