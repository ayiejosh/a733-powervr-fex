# Mesa pvr: shaderFloat16 and shaderInt8

First increment of the 8/16-bit feature group. The group is nine features; this lands the two that do
not need memory-access lowering, because they turned out to need no implementation at all - only
advertising, and a test to prove it.

## What was there

`pco_trans_nir.c` carries two `/* TODO: f16 support. */` comments (in the vertex input and output
paths) and `pco_nir_alu.c` has no 8/16-bit ALU, so the assumption was that narrow types were a
compiler project. They are not: pco already translates the conversions (`f2f16`, `f2f16_rtne`,
`f2f16_rtz`) and `pco_nir_options()` sets `lower_fquantize2f16`, so a shader that uses `float16_t` or
`int8_t` arithmetic compiles and runs correctly with the features switched on.

## The change

`.shaderFloat16 = true` and `.shaderInt8 = true` in the flat feature table
(`pvr_physical_device.c`), with a comment recording that this was measured rather than assumed.

## Verification

`bench/pvr-vulkan/vk16.c` + `f16_alu.comp`: a compute shader doing narrow-type arithmetic on values
chosen to be exact, so nothing here is a rounding argument. Run against both drivers, identical
results:

```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json ./vk16     vendor: PASS (9 ok, 0 failed)
$ sudo ./open-run.sh .../vk16                                      open:   PASS (9 ok, 0 failed)
  f16 1.5*2.25*8 = 27 (want 27)      i8 -100/3+128 = 95 (want 95)
  u8 200+100 = 44 (want 44, wraps)   f16 cmp = 1 (want 1)   no out-of-slot write
$ regress.sh   23 passed, 0 failed, 0 known-open     (vk16 is now a suite case)
```

## What this does not prove, and was removed rather than kept

A per-operation f16 rounding probe was written and then **deleted**: summing `1.0 + 0.0005` twice in
f16 should distinguish per-operation rounding (1026 x1024) from rounding once at the end (1025), and
both this driver and the vendor return the same constant-folded value. So it proves nothing about
either driver, and its expectation was mine rather than the specification's. **Per-operation f16
rounding is therefore not covered by the test suite** - `vk16` prints that caveat in its own output
rather than leaving the impression that it is.

## Still open in the group

The seven storage and push-constant features (`storageBuffer8/16BitAccess`,
`uniformAndStorageBuffer8/16BitAccess`, `storagePushConstant8/16`, `storageInputOutput16`) need the
memory-access half: `nir_lower_mem_access_bit_sizes` with a pvr callback that rounds sub-32-bit
accesses up to a channel, and the store side of that (read-modify-write) checked by hand. That is the
next increment, and it is the bigger one.
