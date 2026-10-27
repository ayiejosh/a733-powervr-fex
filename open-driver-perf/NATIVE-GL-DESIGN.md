# Native GL for the open PowerVR driver: design

Goal: **GL on the open kernel driver (no zink) that functions, then closes on the vendor's native GL.**

## What the measurements force

| stack | glmark2 (texture+bump+effect2d) |
|---|---|
| vendor native GL | 720 |
| zink -> vendor Vulkan | 293 |
| zink -> open Vulkan | 289 |

The open **driver** is at vendor parity. The 2.5x is the GL frontend. Every cheap fix has been tested and
excluded (descriptor modes, window system, Mesa version, texture paths, same-path comparison). So: only a
native GL path closes it, and it starts 430 points behind.

## Staged acceptance

- **Stage 1** - a pixel, then a triangle, verified by readback. Correctness, not speed.
- **Stage 2** - glmark2 off-screen runs to completion through the native path.
- **Stage 3** - beat zink (289). This is the first *performance* claim worth making.
- **Stage 4** - approach vendor (600+). Long-term; a from-scratch driver rarely matches a closed one.

Requiring 720 before Stage 1 is how this project dies. The stages are the contract.

## The approach: reuse, do not re-derive

The session's core inefficiency is that `pvr_native` re-implements what `src/imagination/vulkan`
already does correctly at vendor parity. Every stream, block, launcher and HWRT structure was rebuilt
from captures while a working implementation sat in the same tree.

So: **port the Vulkan driver's render path into the Gallium driver** (or a shared library both use),
and write only what is genuinely GL-specific.

Port (mechanical, proven):
- `pvr_arch_job_render.c` stream and HWRT setup
- `pds/pvr_pipeline_pds.c` vertex attribute program (generator already callable)
- descriptor/PCG emission
- job submission and synchronisation

Write (GL-specific, genuinely new):
- GL state -> PowerVR state translation (the Gallium state tracker already produces these)
- `draw_vbo` -> control stream
- DRI/pipe frontend exposing the driver to Mesa's GL

## Where the vertex address actually goes (established 0.24, pvr_arch_cmd_buffer.c:6163)

The VDM state's `vs_data_addr` is NOT the vertex buffer. The chain is:

1. The vertex attribute program's const map carries entries of type
   `PVR_PDS_CONST_MAP_ENTRY_TYPE_VERTEX_ATTRIBUTE_ADDRESS`, each naming a binding index, an offset and a
   `const_offset` within the PDS data.
2. At draw time the driver patches the PDS data: at `const_offset` it writes
   `vertex_buffer_dev_addr + binding_offset + attribute_offset`.
3. The VDM state's `vs_data_addr` points at that patched PDS data block.

So a draw needs, beyond the program and the buffer: the patched const-map data, and the VDM state aimed at
it. Pointing the VDM state at the vertex buffer directly - which is what was tried first - cannot work,
and the unchanged render target is the expected outcome of that mistake.

## Milestones

1. **Vertex fetch** - one `pvr_pds_vertex_dma` (position, 3 floats, stride 12), call
   `pvr_pds_generate_vertex_primary_program()`, upload, emit descriptors. *(Closest to done: structs and
   generator identified, probe test started.)*
2. **First draw** - control stream with vertex fetch bound; verify the target changes by readback.
3. **Triangle** - correct pixel positions, verified against a CPU rasteriser reference.
4. **Pipe frontend** - context/resource/surface, enough for `glxinfo` through the native driver.
5. **glmark2 single scene** - end-to-end through the native path.
6. **Stage 3 measurement** - the same three-scene set used for the vendor/zink numbers.

## Testing

Every milestone is verified the same way this session proved things: against an external reference,
never against my own expectation.
- streams: capture from the working Vulkan driver and compare byte for byte
- pixels: readback and compare against a CPU-computed reference
- performance: the same glmark2 invocation used for the 720/293/289 numbers

## Risks

- **GPU hangs** surface as board resets, not clean errnos. Test with a small surface first and one draw
  at a time; keep the reboot-tracker snapshot running.
- **The state tracker mountain**: after a triangle, GL state (blend, depth, textures, uniforms) is where
  a driver becomes real, and it is larger than everything before it.
- **Scope**: this is months. The stages exist so each is independently valuable.

## Standing safety rules

- never run a GPU compositor (weston/KDE-GL) with `pvrsrvkm` bound - it NULL-derefs the kernel
- never hand-roll a driver switch; use `bench/pvr-vulkan/ab.sh` or `open-wayland-score.sh` (they carry the
  alive-compositor abort and the refcount-0 gate, which took 36 s one time)
- switch back to `pvrsrvkm` BEFORE restarting display-manager
