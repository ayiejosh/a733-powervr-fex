# mesa-main: buffer device addresses, and the pack/unpack bug they uncovered

Applies to `/home/radxa/mesa/mesa-main` (the tree that builds the default ICD,
`build-x11/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json`).
Unlike `0001`-`0004`, these are kept as notes rather than patches because the tree
already carries the earlier changes as working-tree modifications; the hunks below
are small and quoted in full.

## 1. Advertise `VK_KHR_buffer_device_address` / `VK_EXT_buffer_device_address`

Everything the feature needs was already implemented: `pvr_GetBufferDeviceAddress()`
returned `buffer->dev_addr.addr`, `pvr_BindBufferMemory2()` assigned it, descriptors
already carried it (`pvr_arch_descriptor_set.c:37`), and the compiler lowered
`PhysicalStorageBuffer` to the global LD/ST that shared memory uses. The gate was the
advertisement itself: `vk_spirv_to_nir()` installs SPIR-V capability masks derived from
`supported_features`, so a `PhysicalStorageBuffer` shader module was rejected before the
compiler saw it.

`src/imagination/vulkan/pvr_physical_device.c`:

```c
       .KHR_bind_memory2 = true,
-      .KHR_buffer_device_address = false,
+      .KHR_buffer_device_address = true,
...
       .EXT_border_color_swizzle = true,
+      .EXT_buffer_device_address = true,
       .EXT_color_write_enable = true,
...
       /* Vulkan 1.2 / VK_KHR_buffer_device_address */
-      .bufferDeviceAddress = false,
+      .bufferDeviceAddress = true,
       .bufferDeviceAddressCaptureReplay = false,
       .bufferDeviceAddressMultiDevice = false,
```

One flat flag fills all three feature structs - `VkPhysicalDeviceVulkan12Features`,
`VkPhysicalDeviceBufferDeviceAddressFeatures` and `...FeaturesEXT` - via
`vk_physical_device_features_gen.py:104`. Capture replay stays false on purpose: it
needs application-directed placement in the winsys, which the heap allocator cannot do.

`src/imagination/vulkan/pvr_device.c`:

```c
       VkResult result = pvr_bind_memory(device, mem, pBindInfos[i].memoryOffset,
                                         buffer->vk.size, buffer->alignment,
                                         &buffer->vma, &buffer->dev_addr);
       ...
+      /* Publish the device address for vkGetBufferDeviceAddress() and for the
+       * common runtime helpers, which read vk_buffer::device_address. */
+      buffer->vk.device_address = buffer->dev_addr.addr;
```

and the entry points the generated table expects but the driver never defined. Without
them the weak symbol resolves to the entry point stub, `vkGetDeviceProcAddr` returns the
stub, and `vkCreateDevice` with the extension enabled fails with
`VK_ERROR_EXTENSION_NOT_PRESENT`:

```c
+VkDeviceAddress pvr_GetBufferDeviceAddressKHR(VkDevice device, const VkBufferDeviceAddressInfo *pInfo)
+{ return pvr_GetBufferDeviceAddress(device, pInfo); }
+VkDeviceAddress pvr_GetBufferDeviceAddressEXT(VkDevice device, const VkBufferDeviceAddressInfo *pInfo)
+{ return pvr_GetBufferDeviceAddress(device, pInfo); }
+uint64_t pvr_GetBufferOpaqueCaptureAddressKHR(VkDevice device, const VkBufferDeviceAddressInfo *pInfo)
+{ return pvr_GetBufferOpaqueCaptureAddress(device, pInfo); }
+uint64_t pvr_GetDeviceMemoryOpaqueCaptureAddressKHR(VkDevice device, const VkDeviceMemoryOpaqueCaptureAddressInfo *pInfo)
+{ return pvr_GetDeviceMemoryOpaqueCaptureAddress(device, pInfo); }
```

## 2. `pco_nir.c`: 64-bit pack/unpack must not reach the translator

`nir_lower_explicit_io()` emits `pack/unpack_64_2x32_split` when it turns a 64-bit
address into the 2x32-bit global format. Only `nir_opt_algebraic()` lowers those
(`nir_opt_algebraic.py:2260`); `nir_lower_pack()` handles only the non-split
`pack_64_2x32`/`unpack_64_2x32`. The last `pco_nir_opt()` call in `pco_postprocess_nir()`
passes `algebraic = false`, so nothing removes them, and `trans_alu()` has no case for
them: it reaches `UNREACHABLE("")`, which is undefined behaviour in a release build, so
`vkCreateComputePipelines()` segfaults instead of returning an error.

Added a `shader_has_pack_split()` scan and, after the final `pco_nir_opt(..., false)`:

```c
   if (shader_has_pack_split(nir)) {
      for (unsigned i = 0; i < 8; i++) {
         bool late_progress = false;
         NIR_PASS(late_progress, nir, nir_lower_int64);
         NIR_PASS(late_progress, nir, nir_opt_algebraic);
         NIR_PASS(late_progress, nir, nir_opt_constant_folding);
         if (!late_progress)
            break;
      }
   }
```

This is inert for every shader that compiled before the change: such a shader contained
no split intrinsic, so the block never runs.

**Still open:** `nir_lower_int64()` and `nir_opt_algebraic()` form a cycle - the first
builds 64-bit values out of these split intrinsics, the second rewrites them back into
64-bit `ishl`/`ushr`/`u2u64`. That is why `pco_nir_opt()` prints `WARNING! Infinite opt
loop!` and abandons its loop after 1000 iterations, and why 64-bit push constants read
back with the high half duplicated (`trans_shift()` asserts `bits == 32`, an assert that
is compiled out in a release build). Reproducer: `bench/pvr-vulkan/pctest.c`.
