# Auditing advertised limits for enforcement: three defects of one class

## The class

**An advertised limit whose only enforcement is an `assert()`.** `assert` is compiled out in a
release build, so the advertised value is correct but nothing checks it, and a non-conformant caller
corrupts memory or crashes the driver process instead of receiving an error.

Found by taking each advertised limit, grepping for the check that enforces it, and treating
"assert only" as a defect.

## The three instances

| # | limit | array | how the index is derived | symptom |
|---|---|---|---|---|
| 1 | `maxVertexInputAttributes` = 16 | `pco_vs_data::attribs[16]`, `pvi_state::attribs[16]`, `attrib_formats[32]` | `location - VERT_ATTRIB_GENERIC0` | **SIGSEGV** in `pco_nir_pvi`; **heap corruption** (`realloc(): invalid next size`) |
| 2 | `maxVertexInputBindings` = 16 | `pvr_cmd_buffer_state::vertex_bindings[16]` | `firstBinding + i` | **silent** corruption - the overflow lands inside the same struct, so no fault |
| 3 | `maxBoundDescriptorSets` = 4 | `pvr_descriptor_state::sets[4]`, `layout->set_layouts[]` | `firstSet + u` (application-controlled) | out of bounds by inspection |

`VERT_ATTRIB_GENERIC0` is **15** and `VERT_ATTRIB_GENERIC15` is **30**, so for case 1 a 17th attribute
lands at location 31 -> index 16 into a 16-entry array, and an 18th at location 32 -> index 32 into a
32-entry array.

## Fixes applied

1. `pvr_graphics_pipeline_init`: reject `location >= MAX_VERTEX_GENERIC_ATTRIBS` with
   `VK_ERROR_UNKNOWN`. Plus two narrower bounds checks kept as defence in depth
   (`pvr_init_vs_attribs`, `lower_pvi`).
2. `CmdBindVertexBuffers`: clamp the loop and the stride call to the array size.
3. `CmdBindDescriptorSets2KHR`: skip `desc_set >= PVR_MAX_DESCRIPTOR_SETS`.

## Verified

`vattrib` (N attributes) and `vbind` (N bindings) harnesses; N=16 works, over-limit no longer
crashes. All 13 probes plus `glmark2 --validate` (27/0) pass after each fix.

**Case 2 has no observable symptom** - the vbind harness reports success both before and after, so
that fix rests on inspection of the index arithmetic, not on a failing test. Stated explicitly
because it is the weaker of the three.

## Other assert-only sites not yet examined

```
pvr_arch_cmd_buffer.c:4444   assert(pbe_emits <= PVR_MAX_COLOR_ATTACHMENTS);
pvr_arch_spm.c:519,561       assert(total_render_target_used ... < PVR_MAX_COLOR_ATTACHMENTS);
```

These read compiler-generated data rather than an application index, so they are lower risk, but the
same reasoning applies: an assert is not a guard in a release build.

## Fourth instance

| # | limit | array | index from | symptom |
|---|---|---|---|---|
| 4 | `maxColorAttachments` = 8 | VLAs sized by the count; `mrt_setup->mrt_resources[]` | `colorAttachmentCount` (application) | out of bounds by inspection |

`pvr_dynamic_rendering_output_attachments_setup()` sizes
`VkFormat attachment_formats[colorAttachmentCount]` and
`uint32_t mrt_attachment_map[colorAttachmentCount]` from the application's value, then scatters
render targets over `mrt_setup->mrt_resources[]` for every attachment. No guard existed. Now
rejected with `VK_ERROR_UNKNOWN`.

## Audited and found NOT defective

| site | why it is safe |
|---|---|
| `pvr_arch_cmd_buffer.c:4462` `assert(pbe_emits <= PVR_MAX_COLOR_ATTACHMENTS)` | `pbe_emits` increments at most once per attachment, and the second loop already has an explicit `if (pbe_emits < PVR_MAX_COLOR_ATTACHMENTS)` check |
| `pvr_arch_spm.c:519/561` `assert(total_render_target_used ... < PVR_MAX_COLOR_ATTACHMENTS)` | the `pbe_state_words[]`/`tile_buffer_addrs[]` arrays are sized by the same constant and the counters are bounded by it in the same expressions |
| `pvr_arch_hw_pass.c:1191` `assert(eot_surface_count <= 16U)` | `eot_surfaces` is allocated `sizeof * eot_surface_count`, so the count sizes its own array; 16 is a hardware limit, not an array bound |

## The pattern, stated once

**An advertised limit is only safe if something in the release build enforces it.** `assert()` is
not enforcement. Four instances found by grepping each advertised limit for its check; all four had
either no check or an assert-only check, and all four are now guarded in the place the value enters
the driver (`pvr_graphics_pipeline_init` for attributes, `CmdBindVertexBuffers`,
`CmdBindDescriptorSets2KHR`, `pvr_dynamic_rendering_output_attachments_setup`).
