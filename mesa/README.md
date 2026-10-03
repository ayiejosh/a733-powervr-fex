# Mesa pvr on the Radxa Cubie A7A (Allwinner A733, PowerVR BXM-4-64)

Mesa's `pvr` Vulkan driver is the only userspace that can drive the **mainline**
`powervr` DRM driver. Getting it to run on this board needed two things beyond a
stock build, because this GPU (BVNC 36.56.104.183) sits in a gap between Mesa
releases. Both are in this directory.

## Which Mesa version

| version | knows BVNC 36.56.104.183 | device enumeration | builds without LLVM | verdict |
|---|---|---|---|---|
| 25.0.7 | **no** — ships only `axe-1-16m`, `bxs-4-64`, `gx6250` | DT `compatible` table | yes | driver also stubs its shader compiler (`pco_nir.c` has 4 `finishme`s, plus `pvr_hardcode.c`) |
| 25.1 / 25.2 | no | DT `compatible` table | yes | same gap |
| **25.3.0** | **yes** — `device_info/bxm-4-64.h` | DT `compatible` table | no: pvr needs CLC → LLVM + LLVMSPIRVLib + libclc + SPIRV-Tools | works; see build below |
| main | yes | capability-based (DRM name + dumb-buffer/PRIME caps), no table | no (same CLC chain) | no patches needed |

`imagination-uscgen-devices` does not list `bxm-4-64`, so no USC programs are
pre-built for this GPU; 25.3.0 compiles them at runtime, which is why 25.0.7's
hard-coded-program path had to go.

## Patches

- `0001-pvr-add-A733-img-gpu-platform.patch` — adds
  `DEF_CONFIG("img,gpu", "allwinner,sunxi-drm")` to `pvr_drm_configs[]`. Without
  it the driver enumerates **zero** devices on this board, because the table is a
  DT-`compatible` whitelist and this device tree names the GPU `img,gpu` and the
  display engine `allwinner,sunxi-drm`. Needed by 24.3–25.3; main has no table.
- `0002-pvr-backport-BXM-4-64-device-info.patch` — only if you must stay on
  25.0–25.2: backports the 25.3.0 device-info blob for this BVNC. `struct
  pvr_device_info` is byte-identical between 25.0.7 and 25.3.0, so the copy
  compiles; entries that 25.0.7 cannot represent are removed and listed in the
  file. On 25.3.0 this patch is unnecessary.
- `zink-quads-without-gs.patch` — **not part of the pvr series.** Applies to the gallium
  `zink` GL driver in 25.0.7, the system Mesa this board's desktop GL path uses. It removes
  zink's dependence on a geometry shader. See the section below.

## Zink: primitives without a geometry shader

`zink-quads-without-gs.patch` applies to the **gallium `zink` driver in 25.0.7**, not to pvr,
and is what makes quad-drawing GL work on this blob.

zink lowers `GL_QUADS` with a self-generated geometry shader (`filled quad gs`). The blob
cannot execute a GS - its shader compiler calls `abort()` instead of returning an error - so
the fixed-function quad demos die with `SIGABRT`. The feature-strip layer's `PVR_FAKE_GS=1` is
what puts zink on that path; see [`../gpu/vk-feature-strip/README.md`](../gpu/vk-feature-strip/README.md)
and issue #6.

The patch makes zink treat the blob's `geometryShader` as unusable and lower quads, quad
strips, polygons and line loops with `util_primconvert`, which expands them to triangles on the
CPU. The draw still runs on the GPU; only index generation is on the CPU. Mesa already ships
that conversion - `u_indices.c`'s `generate_quads`, reached by virgl and d3d12 through
`u_primconvert` and by panfrost and lima through `u_vbuf` - and zink was the one driver not
wired into it. **4 files, +144 -7**, in two parts.

**Part 1 - let the driver start** (`zink_screen.c`): remove an assert and an init gate
that both assumed `geometryShader` was present.

Two earlier versions were cut after measurement:

1. A `util_primconvert` wiring, on the assumption that quads still reached zink and
   had to be lowered there. Measured with a marker in the conversion branch: **zero
   hits** across all five primitive types. Mesa's frontend lowers anything the driver
   does not advertise before zink ever sees it, so that branch could never run.
2. Forcing `geometryShader = false` for the driver. Also unnecessary - the PowerVR
   blob already reports `geometryShader = false`, honestly. It is the feature-strip
   layer's `PVR_FAKE_GS=1` that reports it as `true`, and the forcing existed only to
   undo that lie. With no layer, zink sees the truth and takes its ordinary non-GS
   path on its own.

So nothing is forced and no capability is taken away: a driver that reports
`geometryShader = true` is unaffected, and one that reports `false` can now start
instead of being rejected.

**Part 2 - emulate wireframe** (`zink_state.c`, `zink_draw.cpp`, `zink_types.h`).
This driver also has no `fillModeNonSolid`, and that one cannot be fixed by asking
nicely: enabling the feature on the device is refused with
`VK_ERROR_FEATURE_NOT_PRESENT`, and with it merely faked, `glPolygonMode(GL_LINE)` is
ignored - measured with pixel readback, filled and lined pixel counts identical.
Vulkan has no way to say "draw these triangles as lines" other than
`polygonMode = LINE`, so zink now expands the draw itself: each triangle's three
edges are written to an index buffer and the draw is reissued as a line list. The
geometry still runs on the GPU; only index generation is on the CPU. The state path
stops handing the driver a polygon mode it ignores.

Polygon **point** mode was tried and deliberately reverted. Reissuing the draw as a point
list does change the topology, but the pipeline's shader was compiled for triangles and
never writes `gl_PointSize`, so the driver rasterises at an undefined size. Over 20 runs of
the same draw: 18 correct, one at 25 pixels, one at 1961. Non-deterministic output is worse
than consistently ignoring the mode. A plain `GL_POINTS` draw is stable at the same size
(154 pixels for `glPointSize(8)`, 20 of 20 runs), so the limit is the topology override
without a point-aware pipeline - fixing it means building the pipeline with point topology
and a shader keyed on it.

Measured, filled vs wireframe/point pixels, `primtest wireframe`:

| case | before | after |
|---|---|---|
| `tri arrays` | 1682 / 1682 ignored | 1682 / **172** works |
| `tri indexed` | 1815 / 1815 ignored | 1815 / **222** works |
| `strip arrays` | 1740 / 1740 ignored | 1740 / **235** works |
| `strip indexed` | 1740 / 1740 ignored | 1740 / **235** works |
| `fan arrays` | 1815 / 1815 ignored | 1815 / **222** works |
| `fan indexed` | 1740 / 1740 ignored | 1740 / **235** works |
| `multidraw` | 1682 / 1682 ignored | 1682 / **172** works |
| `restart` | n/a | plain 172 = with-restart 172, no edge across the break |
| `point mode` | 1682 / 1682 ignored | 1682 / 1682 ignored - not emulable, see above |
| `instanced` | works either way | 1 instance 33408, 2 instances 66810 - exactly 2x |

Indexed draws, multi-draw, instancing and primitive restart are all handled; a restart
starts a new primitive rather than drawing an edge across the break. The instancing case
is measured with additive blending over a dimmed colour, because a white line already sits
at 255 and adding to it shows nothing.

Two limits, both checked:

- **Transform feedback is skipped**, not emulated: expanding the draw would make TF
  capture the generated lines instead of the app's triangles. `primtest tf` reports
  `SKIP: no transform feedback on this stack` - this stack advertises only
  `GL_ARB_transform_feedback_overflow_query`, which is not transform feedback, and the
  `GL_MAX_TRANSFORM_FEEDBACK_*` limits do not resolve at all. So the guard protects a
  combination that cannot occur here.
- **`GL_EDGE_FLAG` is ignored**: the expansion always emits all three edges, so an app
  using edge flags to select edges gets the full wireframe. Not emulated.
- **Line stipple is dropped**: measured, `solid=172 stippled=172` with the expansion - the
  stipple has no effect. The blob has `bresenhamLines` but not `stippledBresenhamLines`, so
  zink sets `no_linestipple` and falls back to its emulation, which is
  `lower_line_stipple_gs` - a geometry-shader pass this driver cannot run. Emulating it
  without a GS would mean a new vertex/fragment-shader path in zink.

The five primitive types are unaffected and re-verified passing.

Measured with `gpu/vk-feature-strip/primtest.c` (one primitive per process,
because an abort takes the whole process down):

| primitive | pre-fix layer, stock Mesa | patched zink, no layer |
|---|---|---|
| `triangles` | OK | OK |
| `quads` | **SIGABRT, exit 134** | **OK** |
| `quad_strip` | **SIGABRT, exit 134** | **OK** |
| `polygon` | OK | OK |
| `line_loop` | OK | OK |

Only quads and quad strips ever took the geometry-shader path; polygons and line
loops were already handled without one.

Build (25.0.7, zink only; a different configuration from the pvr build below):

```sh
git clone --depth 1 -b mesa-25.0.7 https://gitlab.freedesktop.org/mesa/mesa.git
cd mesa && git apply <this repo>/mesa/zink-quads-without-gs.patch
meson setup build -Dbuildtype=release -Dgallium-drivers=zink -Dvulkan-drivers= \
  -Dglx=dri -Dplatforms=x11 -Dopengl=true -Dgles1=false -Dgles2=true -Dllvm=disabled \
  -Dbuild-tests=false -Dtools= -Dvideo-codecs= -Dvalgrind=disabled
ninja -C build -j3        # -j3: -j8 gets the compiler OOM-killed on this 5.9 GB board
```

Verified on this board with the built `libdril_dri.so` and `libgallium-25.0.7.so`, and with
**no layer and no `PVR_FAKE_GS`**:

| check | before | after |
|---|---|---|
| `peglgears` (GL_QUADS) | `SIGABRT`, exit 134 | **209,772 frames in 5.0 s = 41,954 FPS**, exit 0 |
| geometry shaders compiled (`ZINK_DEBUG=nir`) | 1 | **0** |
| `eglinfo` | - | `zink Vulkan 1.3(PowerVR B-Series BXM-4-64 MC1)` |

Not verified: a software-rendering baseline for the FPS figure, and the other quad demos
(`glxgears`, `glxdemo`) cannot obtain a GLX visual on this board at all.

## Building 25.3.0 here

The CLC chain is the awkward part; on this board it is satisfiable from what
Debian already has except for two pieces:

```sh
# 1. LLVMSPIRVLib 19.1.x (Mesa wants >=19.1, <19.2; this board has LLVM 19.1.7)
git clone --depth 1 -b v19.1.15 https://github.com/KhronosGroup/SPIRV-LLVM-Translator
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_DIR=/usr/lib/llvm-19/lib/cmake/llvm -DLLVM_SPIRV_INCLUDE_TESTS=OFF \
      -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j2 && sudo cmake --install build && sudo ldconfig   # -j2: see note

# 2. libclc bitcode + the pkg-config file Mesa looks up
apt-get download libclc-19 && sudo dpkg-deb -x libclc-19_*.deb /
printf 'prefix=/usr\nlibexecdir=/usr/lib/clc\n\nName: libclc\nDescription: OpenCL C library bitcode\nVersion: 19.1.7\n' \
  | sudo tee /usr/local/lib/pkgconfig/libclc.pc

# 3. Mesa itself (llvm-config must be on PATH for Meson's config-tool lookup)
ln -sf /usr/bin/llvm-config-19 ~/bin/llvm-config
PATH=~/bin:$PATH PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig \
  meson setup build -Dbuildtype=release -Dvulkan-drivers=imagination -Dimagination-srv=true \
    -Dgallium-drivers= -Dopengl=false -Dglx=disabled -Dplatforms= -Dgbm=disabled \
    -Dglvnd=disabled -Dvulkan-layers= -Dvideo-codecs= -Dvalgrind=disabled -Dtools= -Dbuild-tests=false
PATH=~/bin:$PATH PKG_CONFIG_PATH=... ninja -C build -j3
```

Two things that cost time here: the build needs `export PYTHONPATH` pointing at a
site-packages with `mako` if it was pip-installed rather than packaged, and
`-j8` got a compiler killed by memory pressure on this 5.8 GB board — `-j2`/`-j3`
is the reliable setting.

Build outputs used by the tests: `build/src/imagination/vulkan/libvulkan_powervr_mesa.so`
and `powervr_mesa_devenv_icd.aarch64.json` (the **devenv** manifest points at the
build directory; the other manifest points at the install prefix).

## Diagnostic instrumentation

The gates above were found with temporary `fprintf` traces in
`pvr_device.c` (device enumeration, `pvr_winsys_create`, `device_info_init`) and
in `winsys/powervr/pvr_drm.c` (the BVNC the winsys read), enabled by
`PVR_TRACE=1`. They are not part of either patch — Mesa's own
`mesa_loge`/`vk_errorf` messages are the durable signal.

## What must be true around it

The vendor `pvrsrvkm` module and the mainline `powervr` module cannot both own
`1800000.gpu`. See `../kernel/open-driver-spike/stage4-mainline-vulkan.sh` for the
swap, which stops `display-manager.service`, drops the vendor module's 188
references in under a second, runs the test, and restores — desktop included,
without a reboot.
