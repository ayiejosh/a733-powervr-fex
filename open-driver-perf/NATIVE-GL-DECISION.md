# Native GL on the open PowerVR driver: what the measurements say, and the decision

Written 2026-10-24 after a long session on `src/gallium/drivers/pvr_native`. It records what is
established by measurement, what has been ruled out, and what the remaining options actually cost.

## The numbers that matter

All on the same board (Radxa Cubie A7A), same glmark2 scenes (`-b texture -b bump -b effect2d`):

| stack | score | notes |
|---|---|---|
| vendor native GL (`PowerVR B-Series BXM-4-64`, real GL) | **720** | on the X server |
| zink -> vendor Vulkan | **293** | same GPU driver, different GL frontend |
| zink -> open Vulkan (`IMAGINATION_OPEN_SOURCE_MESA`) | **289-309** | weston + Xwayland |

And on the non-texture set (`-b build -b shading -b function`): vendor **725**, zink/open **261**.

**Read this carefully: the driver is not the gap.** Against *vendor Vulkan under zink* the open driver
measures at parity or better across two independent A/B runs (cstp 0.86x = faster, cstpf 0.99x, cstpi
0.98x, cstpin 1.01x, vkheavy 0.95x, vkrender 256/1024/2048 = 0.96/0.83/0.92x, 4096 1.03x). The 2.5x
sits entirely in the **GL frontend**: zink translating GL to Vulkan on a CPU that cannot afford it.

## Ruled out by experiment (do not repeat these)

1. **zink descriptor modes** - `ZINK_DESCRIPTORS=default/lazy/immediate` = 290/278/270. Default is
   already best; tuning costs performance.
2. **Window system** - zink through weston+Xwayland (289) vs zink through the real X server (293): ~1%.
   Not the gap.
3. **Texture-specific work** - textured set 0.40 and non-texture set 0.36 of vendor native. The
   non-texture set loses *slightly more*, so there is no texture deficit to chase.
4. **Same-path driver comparison** - impossible: zink refuses to start on the vendor driver
   ("requires the nullDescriptor feature of KHR/EXT robustness2"); the earlier 293 figure only worked
   via `VK_LAYER_PVR_strip`, a feature-faking layer.
5. **Driver-side micro-optimisation** - the stated target ("per-frame CPU overhead") was measured and
   disproven: open record/submit 0.048/0.083 ms vs vendor 0.124/0.139, i.e. ~2.5x cheaper.

## Where the native driver actually stands

`src/gallium/drivers/pvr_native` - 9 sources, ~6800 lines. Verified working:

- **Kernel submission**: geometry + fragment jobs in one submit, ACCEPTED by the real kernel. This
  took finding that `sync_ops.stride` is checked *before* `sync_ops.count` (a zeroed array is
  rejected even with count 0), and that `cmd_stream` is a USERSPACE pointer, not a GPU address.
- **HWRT dataset + two free lists**: created and accepted (page counts must come from the device's
  runtime info, be multiples of 4, and initial == max when grow == 0; the BO needs PM_FW_PROTECT).
- **Render target + PBE**: allocator now honours alignment (it silently dropped it before), so the
  target lands 16 GiB-aligned and is encodable.
- **Static streams**: fragment verified byte-for-byte against a capture; geometry verified against a
  capture at every dword its inputs determine.
- **Control stream**: 17 block emitters (TA state, PPP, PDS, VDM, PBE, samplers, textures, index list,
  terminate).
- **Shaders**: NIR -> pco pipeline -> 224 bytes of USC -> uploaded to a mapped device address.

**Latest state (commit 1e03d19)**: the PDS launcher is built (36 B, zero temps) and the control stream
now describes a draw - PDS state update + VDM state + index list + terminate - and the kernel ACCEPTS
it. Dumped from a real submission:

```
20000000 fffd0006 fffd0000            PDS state update, launcher at 0x9ffffd0000
40000000                              VDM state (1 dw)
7c0a0000 fffd1000 00000003 00000001   index list: address 0x9ffffd1000, count 3, instances 1
c0000000                              STREAM_TERMINATE
```

**Update (commit a17dd4e): both shader programs are now bound at REAL offsets and the submission is still
accepted.** The two former blockers are closed:

 1. the vertex shader is uploaded into the USC heap usable area and the launcher names that offset
    (0x1000); the fragment shader's slot is 0x2000 and its launcher names it. Both offsets are computed
    the same way (heap base + carveout, page-aligned, relative to the heap base), so launcher and upload
    agree by construction.
 2. the fragment program is bound as a PDS state update with dm_target = DDM in the geometry job's
    control stream - proven by the captured fragment static stream being zeros in every pds_bgnd word,
    i.e. the fragment program is NOT bound there.

**Concrete entry point for that category** (found, so the next session does not have to search):
`src/imagination/vulkan/pvr_arch_pipeline.c` holds
`pvr_pds_vertex_attrib_program_create_and_upload()` (lines 248-380, ~130 lines) and
`pvr_pds_vertex_attrib_programs_create_and_upload()`, which build the PDS program that fetches vertex
attributes and the PCG descriptors that describe them. The gallium driver has 17 control-stream emitters
and NONE of them is a vertex/attribute descriptor - so this is greenfield, not a wiring fix.

**What remains is one category: vertex data and attribute descriptors.** The VS outputs a constant
position, the index list points at three indices with no vertex buffer behind them, and no descriptor
state tells the TA how to fetch vertices - so the TA has a valid draw of vertices it cannot read, which
is exactly why the render target stays untouched. This is the same category the working driver spends
thousands of lines on.

**Historical note on the two blockers that USED to be listed here** (kept because the reasoning still
applies to anyone re-deriving it):

1. the launcher points at **USC offset 0**, where no compiled vertex shader sits - the binary must be
   uploaded into the USC code heap at a real offset, and the launcher must name that offset;
2. the **fragment job names no fragment program** - the FS must be bound in the fragment job's stream
   the same way the VS is bound in the geometry job's control stream.

Everything upstream of those two is done and verified: submission, HWRT, free lists, render target,
PBE encoding, static streams (both verified against captures), control-stream blocks, shader
compilation for both stages, USC upload, and the PDS launcher.

## The options, with real costs

| option | work | result |
|---|---|---|
| **A. Finish `pvr_native`** | months; draw path + descriptors + DRI frontend | true native GL, could beat zink and maybe vendor |
| **B. GL frontend on the Vulkan driver's internals** | months, but reuses ~50k lines of working code (command buffers, pco, descriptors) | native GL without reimplementing the driver |
| **C. Vendor stack for GL** | none | 720 today, closed source, not the open driver |

There is **no cheap configuration edge**. Every candidate has been measured.

## Warnings for whoever continues

- Never run a GPU compositor (weston/KDE-GL) with `pvrsrvkm` bound: it NULL-derefs the kernel.
- Never hand-roll a driver switch. Use `bench/pvr-vulkan/ab.sh` or `open-wayland-score.sh`: they carry
  the alive-compositor abort and the `pvrsrvkm` refcount-0 gate (measured: 36 s to reach 0). Always
  switch back to `pvrsrvkm` BEFORE restarting display-manager.
- Treat stream content as verified only against a real capture. Two separate mistakes in this session
  were "my builder is wrong" when the builder was right and the test input was wrong.
