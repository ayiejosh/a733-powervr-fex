# mesa-main: buffer device addresses, and the two compiler bugs they uncovered

Applies to `/home/radxa/mesa/mesa-main` (the tree that builds the default ICD,
`build-x11/src/imagination/vulkan/powervr_mesa_devenv_icd.aarch64.json`).
Kept as notes rather than patches because the tree already carries the earlier
changes as working-tree modifications.

## 1. Advertise buffer device addresses - opt-in

Everything the feature needs was already implemented: `pvr_GetBufferDeviceAddress()`
returned the bound device VA, `pvr_BindBufferMemory2()` assigned it, descriptors already
carried it (`pvr_arch_descriptor_set.c:37`), and the compiler lowered
`PhysicalStorageBuffer` to the global LD/ST that shared memory uses. The gate was the
advertisement: `vk_spirv_to_nir()` installs SPIR-V capability masks derived from
`supported_features`, so a `PhysicalStorageBuffer` shader module was rejected before the
compiler saw it.

`src/imagination/vulkan/pvr_physical_device.c` sets `.KHR_buffer_device_address`,
`.EXT_buffer_device_address` and the flat `.bufferDeviceAddress` from

```c
   const bool bda = os_get_option("PVR_ENABLE_BUFFER_DEVICE_ADDRESS") != NULL;
```

The extension and the feature are gated together - a feature advertised without its
extension is a broken device.

**Why opt-in.** With the extension advertised, zink changes behaviour and GL renders
wrongly (glheadless 512x20: 9504-62320 of 262144 pixels incorrect; the same build with the
feature off passes 262144/262144 twice). zink keys off
`screen->info.have_KHR_buffer_device_address` in four places: `zink_resource.c:325` (adds
`VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` to every buffer), `zink_bo.c:235`
(`VkMemoryAllocateFlagsInfo` on every allocation), `zink_context.c:5942`
(`set_global_binding`), `zink_resource.c:3596` (`resource_get_address`). The pvr driver
looks innocent - it never reads the usage bit, and `pvr_AllocateMemory()` explicitly
ignores `VK_MEMORY_ALLOCATE_FLAGS_INFO` - and the zink in this stack is built from a
different tree (`mesa-25.3.0/build-gl`), so that is where to isolate it.

`src/imagination/vulkan/pvr_device.c` publishes `vk_buffer::device_address` at bind,
returns it from `pvr_GetBufferDeviceAddress()`, and defines the KHR/EXT entry point
aliases. Without the aliases the generated table's weak symbols resolve to the entry point
stub, so `vkCreateDevice` with the extension enabled fails with -7
(`VK_ERROR_EXTENSION_NOT_PRESENT`) - verified on the pre-change ICD.

Capture replay stays false deliberately: it needs application-directed placement in the
winsys, which the heap allocator cannot do.

## 2. `pco_trans_nir.c` / `pco_nir.c`: 64-bit values are two 32-bit channels

Two bugs, both on the path a `uint64_t` takes when a shader reads it.

**(a) The crash.** `nir_lower_explicit_io()` emits `pack/unpack_64_2x32_split` when it turns
a 64-bit address into the 2x32-bit global format. Only `nir_opt_algebraic()` lowers those
(`nir_opt_algebraic.py:2260`); `nir_lower_pack()` handles only the non-split forms. They
reached `trans_alu()`'s default branch, which is `UNREACHABLE` - undefined behaviour in a
release build - so `vkCreateComputePipelines()` segfaulted instead of returning an error.

**(b) The wrong value.** `pco_ref_nir_def()` mapped a 64-bit scalar to a one-channel
reference, so `unpack_64_2x32_split_x` and `_y` both resolved to the same dword, and the
high half of a `uint64_t` push constant read back as a copy of the low half.

The fix, which has to be all four parts at once:

```c
/* pco_trans_nir.c - the one place every NIR value becomes a reference */
static inline pco_ref pco_ref_nir_def(const nir_def *def)
{
   unsigned bits = def->bit_size;
   unsigned chans = def->num_components;
   if (bits == 64) { bits = 32; chans *= 2; }   /* 64-bit == two 32-bit channels */
   return pco_ref_ssa(def->index, bits, chans);
}
```

```c
/* pco_trans_nir.c - take the halves apart with pco_comp, NOT mov + element
 * modifier: a mov with elem set does not select a channel, which is why an
 * earlier attempt still returned the low half twice. */
case nir_op_unpack_64_2x32_split_x:
   instr = pco_comp(&tctx->b, dest, src[0], pco_ref_val16(0));
   break;
case nir_op_unpack_64_2x32_split_y:
   instr = pco_comp(&tctx->b, dest, src[0], pco_ref_val16(1));
   break;
case nir_op_pack_64_2x32_split:
   instr = pco_trans_nir_vec(tctx, dest, 2, src);
   break;
```

```c
/* pco_nir.c - stop rewriting the intrinsics into 64-bit shifts, which is what
 * made nir_opt_algebraic() fight nir_lower_int64() and never converge (pco_nir_opt
 * gives up after 1000 iterations, printing "Infinite opt loop!"). */
   .lower_pack_64_2x32_split = false,
   .lower_unpack_64_2x32_split = false,
```

```c
/* pco_nir.c - the range is counted in dwords and a 64-bit value occupies two */
   if (intr->def.bit_size == 64)
      num_components *= 2;
```

Verified: `pctest` 6/6 (`uvec4` and `uint64_t` blocks, identical pushed bytes, offsets 0 and
8) and `bda` 9/9, with `WARNING! Infinite opt loop!` gone entirely. The push constant fix
is independent of the feature flag and is active in both configurations.

Known gap this does **not** cover: capture replay, and whatever makes zink render wrongly
when the extension is advertised (section 1).
