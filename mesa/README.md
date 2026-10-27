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

Polygon **point** mode needs the vertex shader to write `gl_PointSize` - the driver has
`largePoints` and takes the size from there, and a triangles shader writes no such output,
so the first attempt rasterised at an undefined size (18, 25 and 1961 pixels across 20 runs
of the same draw).

The fix is in `st_atom_shader.c`: `export_point_size` was set from the point-size flags
alone, so it now also exports when the polygon mode is POINT. With that, the same 30 runs
give 26 at 16-18 pixels instead of three different answers.

A residual flakiness remains at a point size of exactly 1.0 - 4 runs in 30, and it is not
specific to this emulation: a plain `GL_POINTS` draw shows it too, and so does stock Mesa
(1961 pixels in one run of 30). The driver's `pointSizeRange` is `[1, 511]`, so 1.0 is the
minimum, which is where the flakiness sits. Larger sizes are rock solid - `glPointSize(8)`
gave 154 pixels in 20 of 20 runs.

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
| `point mode` | 1682 / 1682 ignored | 1682 / **18** works |
| `edge flags` | 1682 / 1682 ignored | 172 all-set vs **58** one-set |
| `instanced` | works either way | 1 instance 33408, 2 instances 66810 - exactly 2x |

Indexed draws, multi-draw, instancing and primitive restart are all handled; a restart
starts a new primitive rather than drawing an edge across the break. The instancing case
is measured with additive blending over a dimmed colour, because a white line already sits
at 255 and adding to it shows nothing.

Two limits, both checked:

- **Transform feedback is emulated and working.** zink builds stream output on
  `VK_EXT_transform_feedback`, which this blob does not advertise, so the capture is done
  with global stores instead: the vertex shader writes the captured varyings to the feedback
  buffer, whose device address arrives in a push constant, at
  `base + vertex_index * stride + offset`. Indexing by vertex keeps the capture order
  deterministic without atomics. Five pieces:

  1. `xfb_addr_lo`/`xfb_addr_hi` in `zink_gfx_push_constant` (split, because a `uint64_t` at
     offset 52 would be misaligned for a push constant).
  2. `lower_xfb_to_stores`, a NIR pass that emits `nir_store_global` per captured output at
     the end of the vertex entrypoint. Each component is resolved through
     `find_var_with_location_frac`, because the varying is scalarised - looking up the
     location alone returns a one-component variable and only the X component gets stored.
  3. The address is pushed per draw from the bound stream-output target.
  4. `max_stream_output_buffers` is reported as 1 when emulating, so the frontend exposes
     the extension again.
  5. The Vulkan transform-feedback commands are skipped - they are NULL entry points without
     the extension, and calling them segfaults.

  Verified: `gpu/tf-test` draws three vertices and reads the buffer back, getting
  `1 2 3 4 5 6 7 8 9 10 11 12` - all four components, in order, reproducibly. The frontend
  now reports `GL_EXT_transform_feedback`.

  Limits: one buffer, no pause/resume, no interleaving, and no stream-output overflow
  queries (those need `VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT`, which needs the
  extension).

- **Transform feedback is skipped**, not emulated: expanding the draw would make TF
  capture the generated lines instead of the app's triangles. `primtest tf` reports
  `SKIP: no transform feedback on this stack` - this stack advertises only
  `GL_ARB_transform_feedback_overflow_query`, which is not transform feedback, and the
  `GL_MAX_TRANSFORM_FEEDBACK_*` limits do not resolve at all. So the guard protects a
  combination that cannot occur here.
- **`GL_EDGE_FLAG` is honoured.** An edge is drawn only when the flag of its first vertex
  is set, so the expansion reads the per-vertex flags itself. st/mesa appends the edge flag
  as the last vertex input, which makes it the last element, and `zink_vertex_elements_state`
  now retains the `pipe_vertex_element` array - the Vulkan-derived state alone loses the
  buffer, offset and stride, so the array was unreachable before. Measured: a triangle drawn
  with all flags set gives 172 pixels, with only the first flagged 58 (one edge of three),
  and an indexed draw 222 against 115.
- **Line stipple is dropped**: measured, `solid=172 stippled=172` with the expansion - the
  stipple has no effect. The blob has `bresenhamLines` but not `stippledBresenhamLines`, so
  zink sets `no_linestipple` and falls back to `lower_line_stipple_gs`.

  That pass is not GS-based out of convenience. It needs both endpoints of each line to
  accumulate the screen-space stipple counter:

      prev = viewport_map(b, prev_pos, vp_scale);
      curr = viewport_map(b, curr, pos_out);
      len  = distance(prev, curr);
      stipple_counter += len;

  A vertex shader sees one vertex at a time and cannot know the other endpoint; a fragment
  shader cannot recover it from interpolated values. So stipple needs a primitive-level
  stage - a geometry or mesh shader - and this driver has neither.

  Note that zink's own `no_linestipple` condition also requires a geometry shader, because
  its fallback is a GS pass. With the GS honestly absent that condition is false, so zink
  left hardware stipple enabled and issued state this driver does not implement - the driver
  warned about it. The patch now sets `no_linestipple` for this driver so stipple is dropped
  rather than attempted, matching what the GS-faked configuration does.

  And there is nothing to substitute for that stage: this driver advertises no
  `VK_EXT_geometry_shader`, no `VK_EXT_mesh_shader`, no `VK_NV_mesh_shader`, no
  `VK_EXT_shader_object` and no `VK_KHR_fragment_shader_barycentric`. Four independent
  angles, same answer.

  Tested from a third angle before concluding: forcing `no_linestipple` off, so zink enables
  `VK_DYNAMIC_STATE_LINE_STIPPLE_EXT` and issues `vkCmdSetLineStippleEXT` regardless of the
  missing feature. The blob ignores it - `solid=172 stippled=172` again, with Mesa warning
  that incorrect rendering will happen. So it is not merely unadvertised: the state is
  accepted and dropped. Three independent angles, same answer.

### Running real applications against the patched build

The default build is `USE_LIBGLVND=0`, which produces `libEGL.so.1` and `libGL.so.1`
directly. Desktop-GL programs that link those (peglgears) use the patched driver, but
glvnd-based programs - anything linking `libEGL.so.1` through the glvnd dispatcher, which
includes every GLES2 application - resolve the *system* `libEGL_mesa.so.0` and so load the
system libgallium, gate and all.

To make the patched build reachable by those, build it as a glvnd vendor library:

    meson configure build -Dglvnd=enabled -Dglx=disabled
    ninja -C build

`glx=disabled` because this board's X server exports no GLX, and building it needs
`glvnd/libglxabi.h` which Debian's stub `libglvnd-dev` does not ship. The glvnd ABI headers
(`libeglabi.h`, `GLdispatchABI.h`) come from upstream libglvnd, and Debian's packages need
`libglvnd0` for the real `libGLdispatch.so`.

Then point EGL at the vendor library with a JSON manifest:

    { "file_format_version": "1.0.0",
      "ICD": { "library_path": "/home/radxa/mesa-glvnd/libEGL_mesa.so.0" } }

    __EGL_VENDOR_LIBRARY_FILENAMES=/home/radxa/mesa-glvnd/50_myzink.json \
    LD_LIBRARY_PATH=/home/radxa/mesa-glvnd:... \
    LIBGL_DRIVERS_PATH=/home/radxa/mesa-glvnd/dri

**glmark2-es2 on the patched build**, no layer and no fakes - all 30-plus scenes completed,
exit 0:

| | score |
|---|---|
| system Mesa + reference layer (current desktop config) | **511** |
| patched Mesa, no layer, no `PVR_FAKE_GS`, no `PVR_FAKE_FILL` | **525** |

The suite covers textures and mipmaps, gouraud/phong/blinn-phong/cel shading, bump mapping,
FBO-based blur and shadow, buffer map and subdata updates, terrain, refraction and shader
loops. That is a considerably broader exercise than anything `primtest` does, and it is the
strongest evidence so far that the driver runs Zink - which is what upstream doubted.

**Robustness.** Upstream rejected this driver as "not robust enough to run Zink at all".
Measured after the patch: 48 consecutive context creations across every mode - the five
primitive types, the wireframe matrix, edge flags and point mode - with zero failures, and a
45-second `peglgears` run completing normally. That is not a claim that the driver is robust
in general, only that it survives sustained use of everything exercised here.

Performance was not measurable: the machine sat at load 8-12 on 8 cores with syncthing at
36% CPU, and repeated runs of the same binary spanned 1,103 to 46,560 FPS. No number from
that environment would mean anything, so none is quoted.

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

---

# The open-driver work: 43 commits on top of Mesa `main`

**Added 2026-10-09.** The branch `open-pvr-work-2026-10-06` in `/home/radxa/mesa/mesa-main` carries
**43 commits** on top of `origin/main` — **+1693 / -255 across 51 files** — and it cannot be pushed anywhere,
because that checkout's only remote is **upstream `gitlab.freedesktop.org/mesa/mesa`** and no fork is configured.

**So it is carried here as patches, in the two forms this directory already uses:**

| file | what it is |
|---|---|
| `mesa-main-open-driver-all.patch` | the whole delta as one patch, like `mesa-main-local-all.patch` |
| `open-driver-series/` | the commits as numbered `format-patch` files, in apply order: 0001-0043 from 2026-10-09, 0044-0098 from 2026-10-11, 0099-0105 from 2026-10-14, 0106-0269 from 2026-10-27 (the native `pvr` driver work and its probe tooling), 0270-0294 from 2026-10-27 (the VK_EXT_transform_feedback port) |

**Apply with:**

```
cd <mesa checkout>            # on the commit this was based on
git am ../open-driver-series/*.patch        # keeps the 43 commits and their messages
# or
git apply ../mesa-main-open-driver-all.patch   # one squashed change
```

## What the 43 commits contain

**Four classes, in the order they are stacked:**

1. **Probed limit fixes** — `pvr` advertised limits that were floors, not the silicon's real values:
   storage buffers 16→96, uniform buffers 13→64, sampled images 32→128, samplers 16→32,
   storage images 4→32, compute workgroup 128→512, vertex output components 64→128,
   input attachments 4→8, color attachments 4→8, maxFramebufferLayers 256→2048.
   **Several violated Vulkan's required minimums.** Each was probed, not assumed.
2. **Memory-safety fixes** — an out-of-bounds write in `pvr_init_vs_attribs`, a silent OOB write in
   `vkCmdBindVertexBuffers`, an unguarded descriptor-set index, an unguarded `colorAttachmentCount`,
   a rejected vertex-input overflow, and an unbalanced `unsync_fence` reset in `zink_copy_image_buffer`.
3. **`pco` shader-compiler fixes** — `max_unroll_iterations` raised 16→64→256→1024 for long loops,
   hoisting repeated immediates into a register, and 16-bit `flrp` / float-conversion fixes.
4. **The timeline-syncobj work, and its withdrawal.** Steps 1–3 taught the winsys submit paths
   timeline syncobjs and ordered GEOM jobs through the queue's persistent timeline syncobj.
   **The final commit REVERTS the GEOM/FRAG conversion**: *it passes every probe but breaks weston.*
   That revert is the most useful commit in the series.

## Honest notes carried with the patches

* **`shaderFloat16` is disabled** — it miscompiles 20 of 27 glmark2 scenes. **A later commit withdrew the
  correctness justification and kept only the performance one**, then a further commit re-asserted it after
  the miscompile was re-confirmed. **The ordering is preserved so the reasoning is visible.**
* **Some commits are instrumentation, not fixes** (`PVR_SUBMIT_MIX`, `SWAP_TIMING`, `ZINK_EXTRA_IMAGES`,
  `WSIREL_POINTS`), and are off by default.
* **Two commits are reverts of earlier commits in the same series** — kept rather than squashed, because the
  reversal is the finding.

## Added 2026-10-11: the performance series, commits 44-98

The same checkout, 55 more commits on top of commit 43 (`d253e35`), carried the same two
ways: `mesa-main-open-driver-all.patch` is regenerated as the whole delta against upstream
`9a2b374`, and `open-driver-series/0044…0098` continue the numbered series in apply order.
Goal of this work: make the open stack (mainline `powervr` kernel driver + Mesa `pvr`/`pco`,
GL through zink) match and beat the vendor `pvrsrvkm` stack on the `bench/` workloads.

**Where it stands (board at 1104 MHz GPU, off-screen, pinned to the A76 cores):**

| workload | open, before this series | open, now | vendor |
|---|---|---|---|
| glmark2-es2 score (zink on open Mesa / vendor native GLES) | 233 | **627** | 930 |
| vkrender 2048 draw passes, ms/frame | — | **1.77** | 1.97 |
| vkheavy 2048, ms/frame | — | **171.7** | 179.8 |
| compute throughput `cstp`, M inv/s | — | **448** | 373 |
| vkrender 512 draw+copy, ms/frame | — | 0.637 | 0.608 |
| vkmany 512 / 2048 (many small draws), ms/frame | — | 0.817 / 5.73 | 0.801 / 5.62 |

Vulkan rows are the 3-round alternating A/B of 2026-10-11 morning, before the last 12 commits;
the two rows where open is 2–5 % behind are host-side latency, not GPU work. The glmark2 gap that
remains is concentrated in a few scenes (desktop blur/shadow ~30 %, uniform-count loops 47 %,
effect2d 50 %); build, texture and gouraud are at 85–95 % of vendor.

**What the 55 commits do, by class:**

1. **Lossless framebuffer compression (TFBC)** for colour targets, then for sampled, input-attachment
   and transfer-destination images and `MUTABLE_FORMAT` views (the zink colour targets), with the
   render-pass fixes it needed: `DONT_CARE` attachments get a render-init entry so a partial render
   area keeps the pixels around it, the hardware-render index bug in `vkCmdClearAttachments`, an
   8×8 minimum extent (a 5×1 image overran its 256-byte block slot).
2. **Fewer per-frame jobs and compiles.** The load-op program cache never matched because a render
   target descriptor left bytes uninitialised; the end-of-tile program cache held one entry. Both
   recompiled a shader on every render pass of a multi-target frame (desktop blur +42 %). Small
   buffer copies (`glBufferSubData` through zink) are batched into one transfer command instead of
   one 3D kick each (buffer subdata scene 100 → 203 FPS, 53 → 3 transfer jobs per frame).
3. **`pco` compiler.** Descriptor null/robustness checks made by the driver at draw time instead of
   in every pixel; colour-write-enable emulation that reads the framebuffer only when an output is
   disabled (every GL fragment shader was a translucent read-modify-write before); ifs and loops
   whose condition is the same in every instance lowered to plain branches instead of
   execution-mask counting, with a new `tstp` op that compares straight into the branch predicate;
   register reads propagated into their uses; native `ffract`, `imul`→`iadd` folding, uniform
   buffer preloading into shared registers, push constants through a pseudo move.
4. **Driver-side caches and fixes:** render-target dataset reuse across command buffers,
   control-stream block recycling, pixel-rate vs sample-rate resolved at compile time, no forced ISP
   feedback for shaders that cannot discard, buffer copy alignment and row-size fixes.

**How it was checked.** Every commit was built and A/B measured on the board (alternating blocks,
`bench/` harnesses, glmark2 off-screen), run through the Vulkan CTS subsets that cover what it
touches (the lists are named in each commit message; the pco, render-pass, TFBC, dynamic rendering,
robustness, UBO and control-flow lists were run on every compiler change), and through a weston +
Xwayland `glmark2-es2 --validate` (27/27 scenes). This is not a full CTS mustpass run; the known
pre-existing failures (47 in the pco list, 7 in dynamic rendering, 2 robustness crashes) are
unchanged throughout. Three bugs found by review and fixed before this PR: an instruction sunk past
a register write by the dual-issue scheduler, a backwards live range for a register first written
inside a loop, and condition producers marked `(ignorepe)` inside loops that turn out divergent.

## Added 2026-10-14: seven more commits, 99-105, and a sync bug fix

The same two carriers again: `open-driver-series/0099…0105` continue the numbered series and
`mesa-main-open-driver-all.patch` is regenerated as the whole delta against upstream `9a2b374`
(`git am` of all 105 patches onto `9a2b374` gives exactly the tree of the Mesa branch).

**A bug in patch 0018, found and fixed (patch 0105).** Patch 0018 registered the emulated timeline
sync type after the DRM syncobj type. With kernel timeline syncobj support both types advertise
`VK_SYNC_FEATURE_TIMELINE`, and the Vulkan runtime supports one: debug builds assert in
`get_timeline_mode()`, release builds mixed the two types. The CTS timeline semaphore cases showed it:
of 5637 `dEQP-VK.synchronization*` cases (basic, timeline, a sample of signal_order, op,
cross_instance, synchronization2), **596 crashed (109) or lost the device (486)** on a build without
the fix, every one a `timeline_semaphore` case (`device_host`, `signal_order`, `one_to_n`,
`wait_before_signal`). Patch 0105 registers the emulated type only when the syncobj type has no
timeline support, as anv and radv do, and drops the per-queue `job_sync[]` timeline syncobjs that
were created from that type and never used (on a debug build they hit `vk_sync_type_validate:
Assertion 'type->init'` once the emulated type is no longer registered). With it, 595 of the 596 pass.
The assert build passes all 1235 syncset cases that pass on the release build, the 20 `foldtest` and
130 `nantest` checks, and `glmark2-es2 --validate` (27 ok, 0 failed, 6 reported unknown by the
validator).

Two syncset cases still `Fail` in the first full pass. One is a binary-semaphore `cross_instance`
case (`write_ssbo_fragment_read_vertex_input.buffer_262144`) that fails the same way on the build
without the fix, so it is not caused by this. The other is a timeline case
(`device_host.write_ssbo_compute_indirect_read_vertex_input.buffer_262144`): it failed once while
four test processes shared the GPU, and passed 60 of 60 reruns alone on the release build and 60 of 60
on the assert build. That one is unexplained and may be a load dependent race.

**The six performance commits, with the numbers measured for each** (glmark2-es2 off-screen through
zink, GPU 1104 MHz, pinned to the A76 cores, A/B in alternating blocks):

| patch | what it does | measured |
|---|---|---|
| 0099 | a pending `glClear` that zink submits as its own render pass is held until a later pass uses the image, which takes it as its own load op | desktop shadow 258 → 286, desktop blur 209 → 224, shadow 242 → 312 FPS; suite 641 → 643 |
| 0100 | second fragment shader per pipeline without the colour write enable read-modify-write, picked at draw time when no output is masked | loop 764 → 877, desktop shadow 277 → 327, effect2d 445 → 505, jellyfish 248 → 270, shadow 283 → 314 |
| 0101 | null checks of texture descriptors made when the draw is recorded, not in the shader, when a shader has two or more texture terms | bump +11 %, shadow +6 %, jellyfish +4 % |
| 0102 | `pco_shrink_vecs` also drops dead trailing channels of vecs that contain multi channel sources | effect2d 31 → 16 temporaries, +15 %; desktop shadow +16 %; bump +3 % |
| 0103 | no NaN-preserving expansion for `fmin`/`fmax` with a non-NaN constant operand | +1 to +3 % on shading, bump, ideas, texture, shadow |
| 0104 | a uniform if's other branch no longer forces a DITR pre-fence | jellyfish and bump lose 3 groups on the executed path |

Temporary count is the lever that paid here: the shaders with the most temporaries (effect2d,
desktop shadow) gained the most from 0102, while a synthetic probe with growing temporary counts shows
only a mild slope elsewhere.

**Where the whole glmark2-es2 suite stands** (33 scenes, two full runs of the final build, same
conditions): score **690 and 687** (vendor 930; the series at 0098 measured 641 in the session that
measured 0099, and different sessions drift by about 3 %). Per-scene geometric mean of open over
vendor: **0.719 and 0.715**. Per scene, open as a share of vendor: texture 84–95 %, gouraud 87 %,
buffer 63–95 %, conditionals 69–90 %, phong 72 %, bump 66–80 %, jellyfish 75 %, function 64–73 %,
effect2d 62–68 %, loops 60–74 %, desktop shadow 40–46 %, desktop blur 39 %. The remaining gap is
the same few scenes as before: desktop blur and shadow, the uniform-count loops, effect2d.

**How it was checked.** Each performance commit was built, A/B measured and run through the CTS
lists that cover what it touches (named in its commit message). The sync fix was checked as
described above, plus a speed A/B of the build without and with it, which is inside the run to run
noise off-screen and windowed through weston and Xwayland (every scene within 3 %). The full
15-list CTS gate was not re-run on the very last tree, because patch 0105 does not touch the compiler
or the render paths; the lists were run on the build that has 0099–0104. Still open: about 30 debug
trace hooks (`getenv` checks such as `XGEOM_TRACE`, `ZINK_TIMING`, `PVR_SUBMIT_MIX`, `PVR_JOB_TRACE`)
are in zink, wsi and pvr paths of the branch diff; they are off by default and have to go before any
of this can be sent upstream.


## Added 2026-10-27: the native `pvr` driver work, commits 106-269, and the TA/kick investigation

`open-driver-series/0106…0269` continue the numbered series (164 commits, from
`b0c0edc5 gallium/pvr: skeleton for a native PowerVR Rogue driver` to the probe hooks of 2026-10-27);
`mesa-main-open-driver-all.patch` is regenerated as the whole delta against upstream `9a2b374` -
**269 patches, 133 files, +21015 / -680** (`git am` of all 269 onto `9a2b374` gives the branch tree).

**What the 164 commits contain.** The Igalia Mesa `pvr` Gallium driver, developed on this board:
device enumeration and buffer/VM objects, shader stages through `pco`, the control-stream builder, the
PDS/PPP/VDM state packets, texture and sampler descriptors, the firmware static context state, kernel job
submission, then the transition of the driver into `pvr_native` with probe tooling (`PVR_PROBE_*` hooks:
geometry-only, fragment-only, flags, context priority, sync-op shapes, compute benchmark), a 4x2
pixel-block compute triangle fallback (~7x faster fallback draws), and reference hooks in the Vulkan
driver (`PVR_VK_GEOM_ONLY`, stream dumps). The whole investigation is carried in `open-driver-perf/` at
the repository root: the DM2/TA stall dossier with the kernel debugfs and firmware instrumentation
evidence, the corrected firmware provenance (the official Imagination `rogue_36.56.104.183_v1.fw` build,
sha256-verified against IMG's own linux-firmware fork), the apercel/transform-feedback experiment and
the benchmark logs.

**Where the native path stands.** The native Gallium driver initialises, compiles shaders, records and
submits real command streams, and its `glClear` and compute-fallback paths render on the GPU; its direct
TA kick is processed by the firmware but the hardware never raises the DM2 completion event, so the
geometry stage times out (documented, with every client-visible input compared byte-for-byte against the
working Vulkan client). GL on this board is therefore served through zink -> the open Vulkan driver
(glmark2-es2 543-594 at 800x600, ahead of the vendor GL score 522), while the native GL frontend remains
the open item.


## Added 2026-10-27: VK_EXT_transform_feedback port, patches 0270-0294

The official **Mesa MR !44385 "pvr: implement VK_EXT_transform_feedback"** (Ashish Chauhan, IMG; merged into
Mesa main 2026-10-13, 20 commits) cherry-picked onto the perf branch, plus the two upstream u_trace
prerequisite commits the base predates, plus three integration fixes — 25 more `format-patch` files
(`0270…0293`), and `mesa-main-open-driver-all.patch` is regenerated as the whole delta against upstream
`9a2b374`: **294 patches, 173 files, +25495 / -928** (`git am` of all 294 onto `9a2b374` reproduces the ported
branch tree byte-identically).

Integration notes:
- `pvr_pipeline_destroy_shader_data()` gained a `vs` argument; the perf branch's alt-fragment and
  versioned-shader retry call sites pass `false` / `stage == MESA_SHADER_VERTEX`.
- The MR's dummy render-target dataset is created for fragment jobs outside a render pass; with the perf
  branch's RT-dataset cache it has to be initialized **after** `pvr_rt_dataset_cache_init()` and finished
  **before** `pvr_rt_dataset_cache_finish()` - patch 0294. Without it, device creation crashes in
  `pvr_rt_dataset_cache_take` (the first patch export missed this fix; it is included now and the series was
  re-validated).

**Validated on the board** (open `powervr` kernel driver, official IMG firmware):
- Vulkan `vkrender 512 3`: PASS (262144/262144 pixels);
- zink through headless weston: **`GL_VERSION: OpenGL ES 3.1 Mesa 26.3.0-devel`** (the perf branch without
  this port is GL 2.1 / ES 2.0);
- glmark2 800x600: **578-608** with the port, against 543-594 for the same branch before it — GLES 3.1
  arrived with no performance regression. Re-validated after including patch 0294: Vulkan PASS as both user and
  root, zink GLES 3.1, glmark2 578. The driver still needs `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1` (BXM-4-64 is
  not declared conformant), as before.

## Added 2026-10-27 (evening): the native DM2/TA blocker fixed, patches 0295-0296

The native Gallium client's geometry kick (TA processed by the firmware, DM2 never finishing) was not a
firmware, MTS or kernel limit. An ioctl-level capture and replay of the working Vulkan client and of the
native probe (`open-driver-perf/tools/pvrcap/`) reproduced both outcomes, and grafting one component at a
time showed the native context, HWRT data set and free lists are fine: the native process runs the Vulkan
draw data to `TA finished`. The draw data the native driver builds was wrong:

- **0295** `pvr_vm_map()` did not reserve code-heap pages. The shader stage maps the vertex PDS program, its
  data and the VS/FS USC code at the first usable page of the PDS/USC heaps; the render context later
  allocated the same pages, and VM_MAP silently replaced the earlier mappings (drm_gpuvm unmaps what a new
  mapping overlaps), so the VDM ran a zero-filled buffer as the vertex program. Also
  `pvr_shader_vertex_data()` wrote the PDS const map at byte offsets (the map gives dword offsets) and skipped
  the literal entries, so the attribute DMA had base 0, stride 0, control 0. Each bug alone still hangs.
- **0296** once the TA ran, the 3D stage faulted: the PBE line stride was given in bytes instead of pixels
  (writes past the render target), and the render's completion syncobj was never signalled.

Result on the board: native geometry-only probe `Kick TA -> TA finished` 3/3, no HWR; the full native render
runs `TA finished -> 3D finished` with no fault and the GPU writes the whole target. The written image is all
black (no visible triangle yet) - that is the next item, so native GL is not usable yet. Note for anyone
bisecting: a terminate-only VDM stream hangs the TA even for the working Vulkan driver, so it is not a valid
"minimal" test.
