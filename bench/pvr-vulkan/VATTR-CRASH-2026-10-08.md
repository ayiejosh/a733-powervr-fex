# A >16-vertex-attribute pipeline SIGSEGVs the driver

## How it was found

Probing `maxVertexInputAttributes` (advertised **16**, exactly the Vulkan minimum, with no backing
constant) with a new harness `vattrib`: a vertex shader declaring N `vec4` attributes, one vertex
binding, attribute i holding `i/1024`, and a readback of the sum.

```
N=16  -> correct (got 30, want 30)
N=17  -> SIGSEGV (exit 139)
N=24  -> SIGSEGV
N=32  -> SIGSEGV
```

## The crash

```
#0 add_defs_uses
#1 nir_instr_insert
#2 nir_builder_instr_insert
#3 pco_nir_pvi
#4 pco_lower_nir
#5 pvr_graphics_pipeline_init
#6 pvr_rogue_CreateGraphicsPipelines
```

## Cause 1 (FIXED): out-of-bounds write in pvr_init_vs_attribs

```c
gl_vert_attrib location = attrib->location + VERT_ATTRIB_GENERIC0;   /* 16 for the first */
data->vs.attrib_formats[location] = vk_format_to_pipe_format(attrib->format);
```

`attrib_formats` is `enum pipe_format attrib_formats[VERT_ATTRIB_MAX]` = **32 entries, indices
0..31**. `VERT_ATTRIB_GENERIC0` is 16 and `MAX_VERTEX_GENERIC_ATTRIBS` is 16, so the valid range is
16..31. **A 17th attribute has location 16, giving index 32 - one past the end**, corrupting the
surrounding `pco_data`. A bounds check now skips out-of-range locations.

## Cause 2 (OPEN): the segfault persists

After the fix the crash is unchanged, at the same `pco_nir_pvi` site, so a second defect exists in
that function. `pco_nir_pvi` loops `u < ARRAY_SIZE(state.attribs)` (16) over
`location = u + VERT_ATTRIB_GENERIC0` (16..31) and builds a `nir_load_input` plus
`unpack_from_format(&b, packed_comps, base_type, format, 4)` per attribute. Candidates:
`base_type` can be 0 for an attribute whose shader variable is absent, and an attribute at
location 32 (the 17th) is outside the loop's range entirely, leaving an input variable unhandled.

**This is a real robustness bug**: an application passing one attribute over the advertised limit
crashes the driver process instead of receiving a validation error. The advertised limit itself is
honest (16 works, 17 does not), so this is not a wrong limit - it is a missing guard.

## Harness

`vattrib.c` / `vattrib.vert` / `vattrib.frag` are in this directory; build the shaders with
`glslangValidator -V -DN=<n>` and run `./vattrib vattrib<n>.vert.spv vattrib.frag.spv <n>`.
