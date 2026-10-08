# tiles-in-flight measured at 6 - the last configurable candidate is closed

Instrumented `pvr_arch_setup_tiles_in_flight` to print what it actually computes. This function ends in
`reg.pipe_enable = total_tiles_in_flight - 1`, so it directly sets how many ISP pipes are enabled:

```
[tif] pixel_width=2 partition_size=512 max_partitions=12 partitions_available=12
      total_tiles_in_flight=6 isp_tiles=6 usc_min_output_regs=2 msaa_mode=0
```

**`total_tiles_in_flight = 6`** - exactly the designed value, matching `isp_max_tiles_in_flight = 6` in
`bxm-4-64.h`. The hardware runs 6 tiles in flight, not serialised, so this is **not** the per-tile cost.
Instrumentation reverted; tree clean.

## The render per-tile deficit: fully characterised, not localised

| candidate | measured result |
|---|---|
| tile load/store | `DONT_CARE` no effect |
| tile partition size | 6144, matches the vendor formula exactly |
| **tiles in flight / ISP pipes** | **6, as designed** |
| tiling geometry | 16x16, `skip_init_hdrs=1` confirmed on |
| shader instruction stream | removing 24 prologue instructions: no change |
| PCO codegen | uniform across 1-instruction and 640-op shaders |
| GPU clock | 1104000000 under both drivers |
| bytes/pixel | flat r8 -> rgba8 -> rg16 |
| memory layout | linear/exportable at optimal's rate |
| user sample shading | DOUTU FULL->SELECTIVE: null |
| ISP depth/HSR | colour-only pass, no depth attachment |
| EOT store | `STOREOP=dontcare` no effect; EOT fixed-size per device |
| pixel event PDS | fixed size from device info |

**What remains is the firmware's own per-tile processing** - closed-source, with no counter on this SoC.
Localising further needs the vendor's command stream for the identical draw (inside `libVK_IMG`, closed)
or a firmware-side timing facility (none exposed).

## Honest conclusion

**The render per-tile cost is real, reproducible, compositor-free and 3.68x - and not reachable by any
configuration the driver exposes.** After ~50 rounds on the render half: the deficit is *known to be
per-tile*, *known not to be any of twelve measured candidates*, and *not localisable with the tools
available on this board*. A negative result, but well-bounded: it stops the next person re-checking
those twelve things and points at the firmware.
