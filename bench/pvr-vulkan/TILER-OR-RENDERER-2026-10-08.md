# The guide names the resources - and the tool needed to measure them

From [Balancing Workloads on PowerVR to Eliminate Bottlenecks](https://docs.imgtec.com/performance-guides/graphics-recommendations/html/topics/balancing-workloads-on-powervr-to-eliminate-bottlenecks.html):

> "The following resources are distinguishable on PowerVR hardware: ALU (shader processing load);
> Texturing load; ISP load; Renderer active; Tiler active. Using **PVRtune**, it is possible to observe
> the usage values for these resources."

**This is the authoritative answer to "which unit", and the measurements already narrow it to two:**

| resource | position |
|---|---|
| **ALU** | **at vendor speed** - compute 1.12x; removing 24 prologue instructions changed nothing |
| **Texturing** | not exercised - the fill has no texture fetches (`vktex` costs only 1.5x) |
| **ISP load** | colour-only pass, no depth, minimal HSR |
| **Tiler active** | **candidate** - cost is per-tile, independent of coverage |
| **Renderer active** | **candidate** - cost is per-tile, independent of bytes/format |

**The 3.68x per-tile deficit is in the Tiler or the Renderer.** Distinguishing them needs **PVRtune**,
which is not installed (proprietary suite), and no sysfs/devfreq/debugfs counter exposes either.

## The honest boundary

I can measure *that* the cost is per-tile, *how much* (3.68x), and *that it is not* any of thirteen
configurable things. **Distinguishing Tiler from Renderer requires the vendor profiler.**

Two options for anyone continuing:
1. **Obtain/port PVRtune** (or the kernel counter it reads) - names the resource in minutes; the single
   highest-value next step for the render half.
2. **Instrument the firmware interface** - time the TA and 3D phases separately in the kernel; the UAPI
   already distinguishes geometry and fragment jobs, so this could separate Tiler from Renderer without
   vendor tools.

## Objective status

* **Render per-tile 3.68x** - narrowed to Tiler-or-Renderer; needs PVRtune or kernel-side phase timing.
* **Per-pass syncobj 74x** - root-caused to the kernel UAPI; ~5% payoff; fix is a UAPI addition.
* **Present 22.6x** - 100% WSI waits; **correct behaviour** on Mesa's side, absent on the vendor's.
* **MSAA 4.87x** - reclassified as **expected architecture behaviour**, not a defect.
