# apercel experiment (2026-10-27): newest open driver + patched Mesa

[apercel](https://github.com/TumRedSun/apercel) is a community out-of-tree build of the upstream
drm/imagination driver plus a Mesa patch for this exact GPU (BXM-4-64, BVNC 36.56.104.183 on Allwinner A733).
The fork used here is `CheezeCS/apercel` branch `upstream-fixes` (upstream `e098b9f` + 30 backported upstream
drm/imagination fixes: scheduler timeout above firmware timeout, paired-job submission fixes, FW
common-context-ID population, reset-deadlock, etc.).

## Kernel driver

Built out-of-tree against this board's `6.6.98-5-aw2511` headers:

```
cd apercel
make -C /lib/modules/$(uname -r)/build M=$PWD/driver modules DRM_SYMVERS=/tmp/Module.symvers.empty
```

(`DRM_SYMVERS` must NOT point at the kernel's full Module.symvers: modpost then reports thousands of
"exported twice" errors from BSP modules. An empty extra-symvers file works because the kernel's own
Module.symvers already carries the drm_sched_*/drm_gem_shmem_*/drm_exec_* exports.)

Driver binds our DT node (`img,gpu`), reports `FW version v1.1 (build 6976702 OS)`, and brings the GPU to
1008 MHz. Firmware path: `/lib/firmware/apercel/rogue_36.56.104.183_v1.fw` - the genuine IMG file (sha256
`1db1c399...`, from IMG's linux-firmware fork branch `powervr`, commit 8a58f818).

- Result with the native probe: `--- ALL PASS` overall, but the firmware trace still shows
  `Kick TA ... First kick:1, Last kick:1, CSW resume:0` then `GPU has locked up` ~125 ms later and no
  `TA finished` => the newest driver with all upstream fixes does not change the native TA stall.

## Mesa

The `mesa-26.3` branch carries `mesa/apercel-mesa-26.3.0-rc2.patch` (3901 lines, sha256
`751f3c5c4ce6fa728ddeddd1aa302dbd8f4c883527eb710e46b8ea632156d09c`), which applies cleanly to vanilla
`mesa-26.3.0-rc2` (tag b34c71d9ff). It adds `VK_EXT_transform_feedback` for PowerVR and with it
OpenGL 3.2 core / GLES 3.1 through zink, and renames the DRM driver match to `apercel`.

Build (done in `/tmp/mesa-ap263`, worktree of the tag):

```
meson setup build -Dbuildtype=release -Dvulkan-drivers=imagination -Dgallium-drivers=zink \
      -Dopengl=true -Dgles2=enabled -Dplatforms=x11,wayland -Dglx=dri -Dshader-cache=enabled
ninja -C build
```

Use `build/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json` (the non-devenv ICD points at the
install path /usr/local).

Results on the board with apercel + this Mesa:
- Vulkan `vkrender 512 2`: **PASS** (262144/262144 pixels, 2.6 ms/frame).
- zink GL via headless weston: `GL_VERSION: OpenGL ES 3.1 Mesa 26.3.0-rc2` (vs ES 2.0 on the project's
  current branch), i.e. the transform-feedback stack works.
- glmark2 800x600 score was only **166**, but this is vanilla 26.3.0-rc2 without the project's perf series,
  so it is not comparable with our branch's 543-594 - porting the patch onto the perf branch is required for a
  fair benchmark (open follow-up).

## Reproduce / switch back

`tools/verify/apercel-stack.sh up|down` switches between vendor/apercel stacks with the firmware in place and
restores the desktop afterwards.

## Follow-up: patched vanilla Mesa on the project's own powervr driver

Retargeted the patched tree to our kernel driver (`PVR_DRM_DRIVER_NAME "powervr"`, one-line change + rebuild)
and re-ran the same zink benchmark:
- `GL_VERSION: OpenGL ES 3.1 Mesa 26.3.0-rc2`, Vulkan render PASS,
- glmark2 800x600: **175** (vs 166 on apercel, vs **543-594 on the project's perf branch**).
So the transform-feedback patch gives the GL 3.2/ES 3.1 capability, but the vanilla 26.3-rc2 tree is ~3.1x
slower than our perf-series branch on this board. Getting both requires porting the apercel TF patch onto the
perf branch (conflicts: pvr UAPI header, pco internals, pvr_limits, winsys - next large task, not attempted
yet). Raw log: captures/bench-2026-10-27/glmark2-vanilla263rc2-tf-powervr.txt.

## Porting the TF patch onto the perf branch: conflict scale (2026-10-27)

Three-way apply of `apercel-mesa-26.3.0-rc2.patch` onto the project perf branch (`claude/perf-2026-10-10`,
HEAD 25eef88) with `git apply -3`: **17 conflicted files** - `include/drm-uapi/pvr_drm.h`,
`pco_const_imms.c`, `pco_nir.c`, `pco_nir_pvfio.c`, `pvr_arch_cmd_buffer.c`, `pvr_arch_device.c`,
`pvr_arch_mrt.c`, `pvr_arch_pipeline.c`, `pvr_arch_queue.c`, `pvr_bo.c`, `pvr_device.c/.h`,
`pvr_physical_device.c`, `pvr_queue.h`, `vulkan/winsys/powervr/pvr_drm.c`,
`vulkan/winsys/powervr/pvr_drm_job_render.c` (plus clean hits for limits/job files).
The conflicts are in exactly the files the perf series rewrote (command-buffer recording, MRT/blend, queue
submission, BO alloc, device features), so this is a multi-hour merge with build+validate cycles per step -
recorded as future work, not attempted. Worktree cleaned; the patched vanilla build and its measurements
remain documented above.
