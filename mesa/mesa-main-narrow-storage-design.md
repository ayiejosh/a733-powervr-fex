# 8/16-bit storage access for pco

**Implemented.** Eight of the nine features in this group are advertised and verified on hardware;
the ninth, `storageInputOutput16`, is 16-bit vertex I/O and is a different problem (below).

Status:

| feature | state |
|---|---|
| `shaderFloat16`, `shaderInt8` | advertised, `vk16` PASS 9/9 on both drivers |
| `storageBuffer16BitAccess`, `uniformAndStorageBuffer16BitAccess` | **advertised**, `vkbits` PASS |
| `storageBuffer8BitAccess`, `uniformAndStorageBuffer8BitAccess` | **advertised**, `vkbits` PASS |
| `storagePushConstant8`, `storagePushConstant16` | **advertised**, `vkbits` PASS 15/15 on the open driver and the vendor |
| `storageInputOutput16` | **not advertised** - 16-bit vertex I/O, where the `/* TODO: f16 support. */` comments in `pco_trans_nir.c` actually are |

## The implementation

pco's memory path is 32-bit, so a narrow access becomes a 32-bit access on the containing word.
Mesa already has the pass: `nir_lower_mem_access_bit_sizes()`, driven by a driver callback that says
what shape an access the driver cannot do should become.

```c
static nir_mem_access_size_align
pco_mem_access_size_align_cb(...)
{
   if (bit_size >= 32)
      return (nir_mem_access_size_align){ .num_components = bytes / (bit_size / 8),
                                          .bit_size = bit_size,
                                          .align = MIN2(align_mul, 4),
                                          .shift = nir_mem_access_shift_method_scalar };

   return (nir_mem_access_size_align){ .num_components = 1, .bit_size = 32, .align = 4,
                                       .shift = nir_mem_access_shift_method_scalar };
}
```

It is wired in **twice**, and that is the whole subtlety:

| accesses | pass | placement |
|---|---|---|
| SSBO, UBO | `modes = nir_var_mem_ssbo \| nir_var_mem_ubo` | after `nir_lower_explicit_io(ssbo\|ubo)`, which is what makes them intrinsics |
| push constants | `modes = nir_var_mem_push_const` | after `nir_lower_explicit_io(push_const\|shared)` |

Both use `may_lower_unaligned_stores_to_atomics = true`.

## Three corrections to this note's first draft

All three were found by measuring rather than by reading, and the first draft was wrong about each.

**1. A widened store needs no hand-written read-modify-write.** `lower_mem_store()` tracks a byte mask
and, for a chunk it cannot do at the requested size and alignment, writes it as *a pair of 32-bit
atomics* - but only when `may_lower_unaligned_stores_to_atomics` is set, and it asserts otherwise.
Setting that flag is what makes the store correct, and pco already translates `store_ssbo`'s atomics.
The draft treated this as an open question and assumed a custom implementation was needed.

**2. Push constants cannot be folded into the buffer pass.** The draft's snippet listed
`nir_var_mem_push_const` in one call placed after the UBO/SSBO lowering. That silently does nothing: at
that point a push constant is still a `nir_var_mem_push_const` *variable* and its load is a
`load_deref`, which `nir_lower_mem_access_bit_sizes()` does not match - `intrin_to_variable_mode()`
maps only `nir_intrinsic_load_push_constant`, and that intrinsic does not exist until
`nir_lower_explicit_io(push_const, ...)` runs a few passes later. Adding the mode to the early call
produces no error, no warning and no change; the narrow loads reach the translator unchanged. This is
what the "measured wrong" note below actually was.

**3. The failure was not in the pass.** See below.

## What the wrong value actually was

The draft recorded "a 16-bit push-constant load returns `0x60204` where `0x105` is expected" and left it
as an unexplained driver bug. It decodes exactly. With the lowering never running, both narrow loads
reached `trans_load_common_store()` as-is, which moves a whole 32-bit channel out of the push-constant
word at the dword index pco derived. Neither the mask to the field's width nor the shift down out of the
containing word happened, so a `uint16_t` at byte 0 and a `uint8_t` at byte 2 - pushed as `0x0102` and
`0x03` in a block whose next byte was `0x03` and whose byte 3 was zero - each returned the entire word
`0x00030102`. The test summed them: `0x30102 + 0x30102 = 0x60204`.

The fix is the second pass. The NIR now reads:

```
32 %52 = @load_push_constant (%51 (0x0)) (base=0, range=10, align_mul=256, align_offset=0)
32 %54 = @load_push_constant (%53 (0x1)) (base=0, range=10, align_mul=256, align_offset=4)
32 %56 = @load_push_constant (%55 (0x2)) (base=0, range=10, align_mul=256, align_offset=8)
32 %59 = ubitfield_extract %56, 0x0, 0x8
32 %62 = ubitfield_extract %56, 0x8, 0x8
32 %64 = ishl %62, 0x8
32 %65 = ior %59, %64
```

The two 16-bit members at bytes 4 and 6 became one 32-bit load of the word that contains them, and the
two 8-bit members at bytes 8 and 9 became one load plus two field extracts. The offsets are still in
bytes here; `pco_nir_lower_io()` divides them by four after this point, which is why the widened load has
to be dword-aligned - the pass rounds the offset down to `requested.align`, so it is.

## A residual trap worth knowing about

`nir_op_i2i16`, `i2i32`, `u2u16` and `u2u32` are all translated as a bare `pco_mov`, with the comment
"just consume/treat as 32-bit for now" - the float conversions (`f2f16`, `f2f32`) really do convert, but
the integer ones do not. A narrow integer NIR value therefore lives in a 32-bit register whose upper bits
are *whatever the producer left there*, and widening it to 32 bits does not clear them. That is harmless
while every narrow value is produced by a load that masks, which is what this lowering guarantees - and
it is exactly what the `0x60204` above was, one layer up. Anything that starts producing 16-bit integer
defs without masking will reintroduce it.

## Verification

`bench/pvr-vulkan/vkbits` (shader `bits_storage.comp`). The push-constant block deliberately covers both
alignments, so a widening that forgets the shift shows up as a wrong value rather than a crash:

```glsl
layout(push_constant) uniform PC {
    uint     pc32;   /* byte 0 */
    uint16_t pch_a;  /* byte 4 - low half of the word */
    uint16_t pch_b;  /* byte 6 - high half, needs the shift */
    uint8_t  pcb_a;  /* byte 8 */
    uint8_t  pcb_b;  /* byte 9 - second byte of the word, needs the shift */
} pc;
```

On the open driver (`open-run.sh`), 15 ok / 0 failed:

```
push constants: 32-bit = 0xcafef00d (want 0xcafef00d), 16-bit pair = 0x03040102
                (want 0x03040102), 8-bit pair = 0x00000605 (want 0x00000605)
pre = 0xdeadbeef, post = 0xdeadbeef     <- the store sentinels
```

## `storageInputOutput16` is a separate problem

It is not a memory access at all: it is 16-bit *types* in shader inputs and outputs, and the two
`/* TODO: f16 support. */` comments in `pco_trans_nir.c` (`trans_load_input_vs`, `trans_store_output_vs`)
are where it lives. Both assert that the type is 32-bit - and both asserts are compiled out, because
this build is `buildtype=release`. A 16-bit varying therefore does not fail loudly today; it translates
as a whole 32-bit word, the same failure mode as the push constants above.

Two ways to do it:

1. **In pco**, as packed halves of a 32-bit register. `glsl_count_dword_slots(f16vec2)` is already 1, so
   the varying and attribute allocation in `pvr_arch_pipeline.c` already agrees between the stages. What
   is missing is the pack/unpack in the four translation functions, plus the fact that
   `pco_ref_nir_def()` gives a 16-bit def a 16-bit ref, which the current `pco_mov` does not reconcile.
2. **In NIR**, by promoting 16-bit shader I/O to 32-bit and converting around the loads and stores. The
   only interfaces that leave the driver are the vertex input and the framebuffer, and both are
   format-driven conversions the hardware already does, so this is sound - and it is far less invasive
   than teaching four translation functions a packing convention.
