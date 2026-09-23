# 8/16-bit storage access for pco

**Implemented for buffers and UBOs; push constants are still open.** The four buffer features are
advertised and verified. What is left is `storagePushConstant8`/`16` (measured wrong, below) and
`storageInputOutput16` (16-bit vertex I/O, a separate problem). The design reasoning below is kept
because it is what the implementation follows.

Status:

| feature | state |
|---|---|
| `shaderFloat16`, `shaderInt8` | advertised, `vk16` PASS 9/9 on both drivers |
| `storageBuffer16BitAccess`, `uniformAndStorageBuffer16BitAccess` | **advertised**, `vkbits` PASS |
| `storageBuffer8BitAccess`, `uniformAndStorageBuffer8BitAccess` | **advertised**, `vkbits` PASS |
| `storagePushConstant8`, `storagePushConstant16` | **not advertised** - a 16-bit push-constant load returns `0x60204` where `0x105` is expected, so the pass leaves `load_push_constant` alone |
| `storageInputOutput16` | not advertised - 16-bit vertex I/O, where the `/* TODO: f16 support. */` comments in `pco_trans_nir.c` actually are |

The implementation is the pass below with
`modes = nir_var_mem_ssbo | nir_var_mem_ubo` and `may_lower_unaligned_stores_to_atomics = true`,
wired into `pco_lower_nir()` immediately after the UBO/SSBO and global explicit-IO lowering, with a
callback that widens anything narrower than 32 bits to one 32-bit channel.

**What made it safe, and the correction to this note's first draft.** The draft assumed a widened
store would need hand-written read-modify-write. It does not: `lower_mem_store()` in
`nir_lower_mem_access_bit_sizes.c` tracks a byte mask and, when a chunk cannot be done at the
requested size and alignment, writes it as *a pair of 32-bit atomics* - but only if
`may_lower_unaligned_stores_to_atomics` is set, and it asserts otherwise. Setting that flag is what
turns the silent-corruption hazard into correct code, and the sentinel test is what proves it:
`pre` and `post` around the written values are unchanged after the dispatch.


## What has to happen

pco's memory path is 32-bit. A shader that reads `uint16_t` out of an SSBO produces a NIR load whose
def is 16 bits wide, and the translator has nothing for that. Mesa already has the pass for this case:
`nir_lower_mem_access_bit_sizes()` (`src/compiler/nir/nir_lower_mem_access_bit_sizes.c`), which asks a
driver callback what shape an access it cannot do should become, and rewrites the access - including
the store side, via read-modify-write when `may_lower_unaligned_stores_to_atomics` is set.

**Placement.** After pco has lowered the variable-based accesses to explicit intrinsics, and before
the translation: `pco_nir.c` line ~947-951 lowers UBO/SSBO to
`nir_address_format_vec2_index_32bit_offset` and globals to `nir_address_format_2x32bit_global`, so
the accesses are `load_ssbo`/`store_ssbo`/`load_ubo`/`load_push_constant` by the end of that block.
The pass goes immediately after it.

```c
nir_lower_mem_access_bit_sizes_options opts = {
   .modes = nir_var_mem_ssbo | nir_var_mem_ubo | nir_var_mem_push_const,
   .may_lower_unaligned_stores_to_atomics = true,
   .callback = pco_mem_access_size_align_cb,
   .cb_data = NULL,
};
NIR_PASS(_, nir, nir_lower_mem_access_bit_sizes, &opts);
```

**Callback.** The model to copy is AMD's default branch
(`src/amd/common/nir/ac_nir_lower_mem_access_bit_sizes.c`, the tail of `lower_mem_access_cb`): round
the access up to 32 bits, `num_components = DIV_ROUND_UP(bytes, 4)` rounded to a legal component
count, `align = MIN2(4, combined_align)`, `shift = nir_mem_access_shift_method_none`. On little-endian
`shift = none` is right for a *load*: widening in place puts the wanted 8/16-bit value in the low bits
of the 32-bit result, whatever the byte offset. AMD needs a special shift method because it wants to
combine adjacent values; pvr does not.

## The two hazards, which is why this is not a ten-line patch

1. **A widened load can read past the end of the buffer.** Rounding a 2-byte access at the end of an
   SSBO up to 4 bytes reads two bytes that may not exist. AMD handles this with `max_pad` and by
   refusing to widen some shapes; SSBOs are bounds-checked but the check is on the access the shader
   wrote, not on the one the lowering invents. This needs either a bounded-load form or a policy that
   only widens when the extra bytes are provably inside the bound - and a test that puts a 16-bit
   value in the last two bytes of a buffer.
2. **A widened store clobbers its neighbours.** This is the one that fails *silently*: writing 4 bytes
   where the shader asked for 2 corrupts the two bytes next door, and nothing reports an error. The
   pass's `may_lower_unaligned_stores_to_atomics` path does the read-modify-write via atomics; whether
   pco translates `nir_intrinsic_ssbo_atomic_*` for this purpose is an open question (it translates
   `shared_atomic`; the SSBO side needs checking). If it does not, the store half needs its own
   implementation.

## The test to write first

`bench/pvr-vulkan/vk16.c` already has the harness for the narrow-type features; the storage phase
belongs beside it, and the shape that catches hazard 2 is a block with sentinels:

```glsl
layout(std430, binding = 0) buffer Buf {
    uint     pre;     /* offset 0  - sentinel, must survive untouched */
    uint16_t h0;      /* offset 4  - 4-aligned: the easy case */
    uint16_t h1;      /* offset 6  - only 2-aligned: the interesting case */
    uint8_t  b0, b1, b2, b3;   /* offsets 8..11 */
    uint     out;     /* offset 12 - the push-constant result */
    uint     post;    /* offset 16 - sentinel */
} b;
```

Writing `h0`/`h1` and `b0..b3` and then checking both sentinels and the packed words
(`0xABCD1234` at offset 4, `0x44332211` at offset 8) catches a store that widens without
read-modify-write, and a load that mis-extracts. The vendor driver is the oracle: it advertises all
seven features on this silicon and already agrees with us on the narrow-ALU shader.

## What to advertise, and when

Only after the test passes on hardware: `storageBuffer8BitAccess`, `storageBuffer16BitAccess`,
`uniformAndStorageBuffer8BitAccess`, `uniformAndStorageBuffer16BitAccess`, `storagePushConstant8`,
`storagePushConstant16`. `storageInputOutput16` is a *different* problem - 16-bit vertex I/O - and is
where the two `/* TODO: f16 support. */` comments in `pco_trans_nir.c` actually live, so it stays
false until that is done separately.
