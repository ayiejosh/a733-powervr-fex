# Mesa pvr: advertise and honour the three missing Vulkan 1.3 features

Target: `/home/radxa/mesa/mesa-main` (`pvr`, PowerVR B-Series BXM-4-64 MC1, kernel `powervr` 1.0.0).
Companion to [`mesa-main-buffer-device-address.md`](mesa-main-buffer-device-address.md); the whole local
Mesa diff is kept as [`mesa-main-local-all.patch`](mesa-main-local-all.patch).

## What the device reported before

`vkaudit` on the open driver: `apiVersion 1.2.363`, with three `VkPhysicalDeviceVulkan13Features`
flags false - `pipelineCreationCacheControl`, `robustImageAccess` and
`shaderZeroInitializeWorkgroupMemory`. Everything else Vulkan 1.3 requires was already true, so those
three flags were the entire gap between 1.2 and 1.3.

## The changes

1. **`shaderZeroInitializeWorkgroupMemory` - advertise only.** The implementation was already in the
   driver and simply not advertised. The SPIR-V front end sets
   `nir->info.zero_initialize_shared_memory` for a Workgroup variable carrying a null initializer,
   `pvr_alloc_cs_shmem()` copies it to `data->cs.zero_shmem`, and the driver either memsets the global
   shmem buffer (`pvr_arch_cmd_buffer.c`, spilled case) or runs a USC zero-init shader over the
   coefficient-backed region (`pvr_usc_zero_init_wg_mem`, `pvr_arch_pipeline.c`).
2. **`pipelineCreationCacheControl` - implemented.** `pvr_graphics_pipeline_create()` and
   `pvr_compute_pipeline_create()` now return `VK_PIPELINE_COMPILE_REQUIRED` when
   `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT` is set. pvr's pipeline cache is a stub -
   nothing is ever looked up or inserted - so every creation would compile and that is the honest
   answer; the application then retries without the flag, which is the loop the extension exists for.
   `VkPipelineCreationFeedback` structures are also written (flags left clear, i.e. "no feedback"),
   because the application allocated them uninitialised and asked the driver to fill them.
3. **`robustImageAccess` - advertised, verified against the vendor.** Out-of-bounds image reads come
   back as zero on this hardware: the vendor driver, which advertises the feature, returns `0,0,0,0`
   for an out-of-bounds `imageLoad` while returning the cleared colour in bounds. Same silicon, so the
   open driver has the same behaviour - now measured on both (see `bench/pvr-vulkan/vk13.c`).
4. **`get_api_version()` -> 1.3**, with the reasoning for each required feature recorded in the
   function comment. The only remaining false flag in `VkPhysicalDeviceVulkan13Features` is
   `textureCompressionASTC_HDR`, which 1.3 does not require.

## What was tried and removed

The first implementation ran the generic `nir_zero_initialize_shared_memory()` pass from
`pco_lower_nir()`. Two things were wrong with it:

* it ran **after** `pco_nir_lower_barriers()`, so the barrier it emits was never lowered and reached
  the translator as an unsupported intrinsic - which in a release build is undefined behaviour, and
  it segfaulted (`Unsupported intrinsic: "@barrier (execution_scope=WORKGROUP, ...)"`);
* it was **unnecessary**: the driver already zeroes shared memory through `data->cs.zero_shmem`.

It was removed in favour of advertising the existing implementation.

The same investigation corrected the *test*, not just the driver: the first version of the test
expected workgroup memory without an initializer to read back as zero. The feature is about
"initializing a variable in Workgroup storage class" - honouring an explicit initializer - not about
zeroing workgroup memory in general. The vendor driver failed that test too, which is what prompted
reading the specification again. `zero_shared.comp` now declares `shared uint s[64] = {}` under
`GL_EXT_null_initializer`, which is the SPIR-V `OpConstantNull` initializer the extension is about.

## Verification

```
$ sudo ./open-run.sh .../vk13                         open driver: PASS (11 ok, 0 failed)
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json ./vk13
                                                      vendor:      PASS (11 ok, 0 failed)
$ sudo ./open-run.sh .../regress.sh                   regress: 21 passed, 0 failed, 0 known-open
$ ./vkaudit | grep -E 'apiVersion|vk13\.(robust|pipelineCreation|shaderZero)'   # in the same run
  apiVersion    1.3.363
  vk13.robustImageAccess=1
  vk13.pipelineCreationCacheControl=1
  vk13.shaderZeroInitializeWorkgroupMemory=1
```
