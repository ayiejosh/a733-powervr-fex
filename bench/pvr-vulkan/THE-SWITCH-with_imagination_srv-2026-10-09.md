# THE SWITCH IS `with_imagination_srv`

## The exact gate

`src/imagination/vulkan/meson.build`:

```meson
  if with_imagination_srv
    pvr_files += files(
      'winsys/pvrsrvkm/pvr_srv.c', 'winsys/pvrsrvkm/pvr_srv_bo.c',
      'winsys/pvrsrvkm/pvr_srv_bridge.c',
      'winsys/pvrsrvkm/pvr_srv_job_common.c', '..._compute.c', '..._null.c',
      '..._render.c', '..._transfer.c',
      'winsys/pvrsrvkm/pvr_srv_sync.c',        ← the driver-native sync
      'winsys/pvrsrvkm/pvr_srv_sync_prim.c',
    )
    pvr_flags += '-DPVR_SUPPORT_SERVICES_DRIVER'
  endif
```

**`with_imagination_srv` is false in this build** — which is why the vendor-kernel winsys isn't compiled, why
`PVR_SUPPORT_SERVICES_DRIVER` is undefined so `pvr_instance.c` rejects the vendor DRM name, and why the open
Mesa ICD reports **0 physical devices** against `pvrsrvkm`.

## What enabling it gives

- **`pvr_srv_sync_type`** — the **driver-native sync**, the 60% lever's mechanism, **already written**
- **The srv job paths** (render, compute, transfer, null)
- **The vendor bridge** (144 `pvr_srv_` calls) — Mesa userspace driving the vendor kernel directly

## Why it's the most promising item on the board

**It is a build configuration, not a code change.** No new mechanism, none of the risk that broke weston three
times — **the code exists and is upstream-shaped.** And **it needs no driver switch to test**: the desktop
already runs on `pvrsrvkm`.

## The test

```sh
meson configure build -Dwith_imagination_srv=true
ninja -C build
VK_ICD_FILENAMES=/home/radxa/pvr_gen_icd.json PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 ./enumvk
#   expect: physical devices 1, extensions/features from the SRV path
# then the gate: probes -> weston -> client
```

**If it comes up, the open userspace gets the vendor's kernel-side sync at 0 ioctls/op — the 60% of the real
workload's recoverable cost — without writing the timeline conversion at all.**

**Direct answer to "what is missing": a build option — not a feature, extension or flag.**
