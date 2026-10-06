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
