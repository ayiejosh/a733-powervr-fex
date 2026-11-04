# VK_EXT_transform_feedback port onto the perf branch (validated 2026-10-27)

Official source: Mesa MR **!44385 "pvr: implement VK_EXT_transform_feedback"** (Ashish Chauhan, IMG),
merged into Mesa main 2026-10-13 (20 commits), **plus** the two upstream u_trace prerequisite commits the
base predates (`pvr: implement u_trace support`, `pvr: Instrument draw and transfer u_trace markers`).
Cherry-picked onto the project perf branch `claude/perf-2026-10-10` (HEAD `25eef88d`) - **24 patches** here
reproduce the port (`git am` them in order).

Conflicts and resolutions: `pvr_iface.h` (kept the MR's XFB definitions); `pvr_cmd_buffer.h` (both: our
pending-clear struct + XFB binding/state structs); `pvr_arch_cmd_buffer.c` (MR includes);
`pvr_arch_device.c` (MR's utrace init + teardown, and the dummy-RT dataset init/teardown **moved after/before
our RT-dataset cache** - upstream has no cache, and the first ordering crashed in
`pvr_rt_dataset_cache_take`); `pvr_physical_device.c` (kept our `bufferDeviceAddress=true` + added
`transformFeedback=true`); `pvr_arch_pipeline.c` (pass the new `vs` flag at our alt-fragment and
versioned-shader retry call sites); `pvr_device.h`/`pvr_arch_device.c` (keep our cache fields + u_trace state).

Validated on the board (open `powervr` kernel driver, real IMG firmware; re-validated after including the
ordering fix, with the rebuilt 294-patch series reproducing the same tree):
- Vulkan `vkrender 512 3`: **RESULT: PASS** (262144/262144), exit 0;
- zink through headless weston: `GL_VERSION: OpenGL ES 3.1 Mesa 26.3.0-devel`, score **608** (perf branch
  before the port: 543-594) - GLES 3.1 arrives with **no performance regression**;
- needs `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1` (BXM-4-64 is not declared conformant).
Logs: `captures/bench-2026-10-27/glmark2-tf-port-perf-branch-es31.txt`, `vkrender-tf-port-perf-branch.txt`.
