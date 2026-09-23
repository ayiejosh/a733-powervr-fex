# pvr-vulkan — does this board's PowerVR actually execute Vulkan work?

The A733 has a PowerVR B-Series **BXM-4-64 MC1** (BVNC 36.56.104.183) with **no
usable desktop GL**, which is why desktop GL here goes through zink. Vulkan is a
different question, and it turned out to have three separate answers depending on
which driver and which ICD you mean. These tools measure them.

## Files

| file | what it answers |
|---|---|
| `vktest.c` | can a Vulkan driver take a shader, dispatch it, and return the right bytes? Prints device/queue/memory facts, then verifies every element of a 1M-element xorshift and reports throughput |
| `compute.comp` | the compute shader (xorshift per element, so the host can predict every output exactly) |
| `build.sh` | glslangValidator → SPIR-V → embedded as `uint32_t` words → `vktest` |
| `drmdevs.c` | inventory of DRM devices as **Mesa sees them**: node paths, bus type, DT `compatible` list, and the `drmGetVersion` name/version that pvr_winsys_create switches on |
| `compare-icds.sh` | runs `vktest` through each ICD installed/built here and prints one table |

## Build and run

```sh
./build.sh                     # needs glslangValidator, gcc, python3
./drmdevs                      # what libdrm reports for card0/card1/renderD128
./compare-icds.sh 20 1048576   # 20 dispatches x 1M elements through each ICD
```

`VKTEST_API` (default `1.0`) sets the requested `apiVersion`. This matters: the
loader fails `vkCreateInstance` with `VK_ERROR_INCOMPATIBLE_DRIVER` when the app
asks for more than the ICD's JSON declares, and Mesa 25.0.7's pvr ICD declares
only 1.0.

## The three answers

**1. Vendor ICD — works.** `/usr/lib/libVK_IMG.so` (Imagination DDK
`24.2@6603887`, Vulkan 1.3.277, registered by
`/usr/share/vulkan/icd.d/img_icd.json`) talks to the vendor `pvrsrvkm` module.
Measured, 1M elements per dispatch:

```
submit+wait: 24.971 ms total, 2.497 ms/dispatch
throughput:  3.36 GB/s (read+write)
RESULT: PASS - 1048576/1048576 elements correct
```

**2. Mesa pvr against the vendor module — closed by design.** Mesa's
`pvrsrvkm` ("srv") winsys compiles (`-Dimagination-srv=true`) and the kernel node
is even named `pvr`, but `pvr_srv_winsys_create()` calls
`pvr_is_driver_compatible()`, which accepts **only downstream driver version
1.17** (`PVR_SRV_VERSION_MAJ/MIN`). This board's vendor module reports
`24.2.6603887`, so it returns `VK_ERROR_INCOMPATIBLE_DRIVER` before any ioctl.

**3. Mesa pvr against the mainline `powervr` driver — works.** This is the path
that needed real work, and it now executes GPU work end to end:

```
WARNING: powervr is not a conformant Vulkan implementation, testing use only.
device[0] name="PowerVR B-Series BXM-4-64 MC1" api=1.2.328 vendor=0x1010 device=0x36104183
queue family 0: flags=0x7 count=2
memory: type 0, heap 0 (4436 MB)
dispatching 10 x 1048576 elements (4 MiB per dispatch)...
submit+wait: 28.797 ms total, 2.880 ms/dispatch
throughput:  2.91 GB/s (read+write)
RESULT: PASS - 1048576/1048576 elements correct
```

Three gates had to be cleared, in this order, and each one is a distinct fact:

1. **Enumeration is a DT `compatible` whitelist** (`pvr_drm_configs[]`): it wants
   `img,gpu` for the render node and `allwinner,sunxi-drm` for the display node,
   and without a match the driver enumerates **zero** devices. See
   `mesa/0001-pvr-add-A733-img-gpu-platform.patch`.
2. **Device info for the BVNC**: 25.0.7–25.2 ship only `axe-1-16m`, `bxs-4-64`,
   `gx6250`, so `pvr_device_info_init()` returns `-ENODEV` → the device is
   rejected. Mesa 25.3.0 ships `bxm-4-64.h` with
   `PVR_DEVICE_IDENT_36_V_104_183`.
3. **A working shader compiler**: 25.0.7's `pco_nir.c` is a stub (four
   `finishme`s) and it falls back to hard-coded programs it does not have for
   this GPU, so `vkCreateComputePipelines` fails even with the device info
   backported. 25.3.0 compiles at runtime. Backporting device info into 25.0.7 is
   therefore not a shortcut; `mesa/0002-...patch` exists to document why.

25.3.0 in turn requires CLC → LLVM + LLVMSPIRVLib 19.1.x + libclc + SPIRV-Tools;
`mesa/README.md` has the recipe and the two things this board was missing.

## Notes

- The kernel side already reports the right BVNC: `pvr_dev_query_gpu_info_get()`
  fills `gpu_id` from `pvr_gpu_id_to_packed_bvnc(&pvr_dev->gpu_id)`, and that
  struct is filled by `pvr_load_gpu_id()` reading the hardware control registers.
  Mesa's vendored `pvr_drm.h` is byte-identical to this kernel's UAPI header.
- Taking the GPU away from the vendor module needs the desktop stopped
  (`systemctl stop display-manager`), which drops `pvrsrvkm` refs 188 → 0 in
  under a second. The session running this work lives in
  `user@1000.service/app.slice/dsh-web.service`, not the graphical session, so it
  survives. See `kernel/open-driver-spike/stage4-mainline-vulkan.sh`.
