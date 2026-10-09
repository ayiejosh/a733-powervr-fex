# The two stacks run different firmware for the same GPU — and it closes the render investigation

## The finding

| stack | firmware file | format |
|---|---|---|
| **open** (`powervr`) | `/lib/firmware/powervr/rogue_36.56.104.183_v1.fw` | mainline FWIF: `rogue_<bvnc>_v<N>.fw` |
| **vendor** (`pvrsrvkm`) | `/lib/firmware/rgx.fw.36.56.104.183` | vendor FWIF: `rgx.fw.<bvnc>` |

**Same BVNC, same GPU, different firmware images — and different firmware interfaces.** The mainline module
builds its filename as `base_<b>.<v>.<n>.<c>_v<ver>.fw` (`pvr_device.c:388`), a different scheme and ABI from
the vendor's.

## Why it matters, and why it is not actionable

**The firmware implements the tile scheduling and ISP control** — which is where the fragment job's cost
lives. The fragment job is the critical path (**13.01 ms open vs 5.31 ms vendor**), the shader is only **~15%**
of it and the PBE **under 10%**, so **roughly three quarters is raster and tile processing driven by the
firmware.**

**So part of the 2.47× per-surface difference could live in the firmware rather than in anything the driver
emits** — which would explain why every configuration the driver exposes has been read and is correct or
maximal, while the gap persists.

**It is not actionable**: the images are on different ABIs, so the vendor's cannot drive the mainline module.
The open driver already uses the only image available for its interface (`/lib/firmware/powervr/`), so there
is nothing newer to try.

## This closes the render investigation

| candidate | status |
|---|---|
| fill rate, bytes/pixel, attachment format, layout | excluded by measurement |
| tile size, macrotile grid, region-header count | read: correct |
| tiles in flight / ISP partitions (6, device max) | read: at maximum |
| ISP AA mode | read: `AA_NONE` at 1 sample |
| `process_empty_tiles`, `skip_init_hdrs` | read: as expected |
| PBE | **< 10%**, by a clean non-discard format probe |
| fragment shader | **~15%** of the render |
| geometry/TA job | **faster** than the vendor's |
| **firmware** | **different image, not interchangeable** |

**The remaining 2.47× is therefore either in the firmware image or in a part of the emitted command stream
that only a diff against the vendor's would reveal.** Both require instruments this board does not have.
**That is the honest end of it.**
