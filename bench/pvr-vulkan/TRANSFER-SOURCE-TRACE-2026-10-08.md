# Tracing the 3 transfers/frame: two eliminations, source still unidentified

Following "an empty pass with no work to order should not need 3 creates + 3 transfers + 3 destroys +
2 waits", I traced where the transfers come from.

## Eliminated

* **Not the kernel submit's requirement.** The DRM winsys passes syncs to the kernel submit directly -
  `pvr_drm_sync_op_init(&geom_sync_ops[...], signal_sync_geom->sync, DRM_PVR_SYNC_OP_FLAG_SIGNAL, ...)`
  in `pvr_drm_job_render.c` - so the driver is not forced to pre-copy fences; the kernel takes handles.
* **Not `vk_drm_syncobj_copy_payloads`.** The only caller of `copy_payloads` is
  `vk_device_copy_semaphore_payloads` (`vk_device.c:795`), and **the PVR driver never sets
  `copy_sync_payloads`** (nothing under `src/imagination/` references it). It also returns early when
  the hook is NULL.
* **Not timeline point materialisation.** The queue creates persistent timeline syncs
  (`pvr_arch_queue.c:150-162`) but its own comment says they are **unused**: "Unused for now: the job
  paths still create and destroy a syncobj per job, so this commit changes no behaviour."

## Still unidentified

**Where the 3 `SYNCOBJ_TRANSFER` per frame come from.** Next step is to instrument rather than read:
print at each `transfer` call site in `vk_drm_syncobj.c`, or breakpoint the ioctl and take a backtrace
on the first three hits.

## Solid regardless

* **Empty pass 0.968 ms/frame, 0.456 ms of it kernel time blocked on DRM syncobj**, vs the vendor's
  0.003 ms - a 74x relative gap on a pass that draws nothing.
* **The kernel takes sync handles directly**, so reducing the number of DRM syncobj operations is
  possible in principle without changing what the kernel receives.
* **Timeline-backing is not the fix** (measured: worse ioctl count).
* **The vendor pays none of this** because it uses a driver-native sync type, which the mainline
  `powervr` module does not provide. Adding one there would remove the whole per-pass cost - a
  kernel-module change, in scope for the objective but a project rather than a patch.
