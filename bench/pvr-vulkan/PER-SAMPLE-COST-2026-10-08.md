# The plateau is a per-SAMPLE cost, not bandwidth - plus two leads killed

## Bytes/pixel does not matter

Format sweep, open driver, 2048:

```
FORMAT=r8    (1 byte/pixel)  294.9 Mpix/s
FORMAT=rgba8 (4 bytes)       297.4 Mpix/s
FORMAT=rg16  (4 bytes)       318.5 Mpix/s
```

**Flat across a 4x change in bytes per pixel** - so the ~300 Mpix/s plateau is not surface-store
bandwidth. With the MSAA sweep (s1 303.7 -> s4 62.4; 4.87x for 4x samples) the cost is a **per-sample
fixed cost in the pixel-backend path**. Sharper than "tile resolve/store": it is per *sample* and not
proportional to bytes.

## Killed: phantoms

`pvr_arch_device.c:74` asserts `num_phantoms == 1`; `pvr_arch_job_common.c:390` uses it as
`max_phantoms`. So dmesg's `phantoms=1` is one core, not two - consistent with the board being
BXM-4-64 **MC1**. Not a 2x deficit.

## Killed: tiles in flight

The driver's own arithmetic for 16x16 tiles:

```
partition_size        = uor(2) * 16 * 16                     = 512
usable_partition_size = MIN2(6144, 512 * 12)                 = 6144
partitions_available  = MIN2(12, 6144/512)                   = 12
usc_tiles_in_flight   = partitions_available                 = 12  (16x16 skips the cluster divide)
isp_tiles_in_flight   = isp_max_tiles_in_flight(6)/phantoms  = 6
tiles_in_flight       = MIN2(12, 6)                          = 6
```

6 matches `isp_max_tiles_in_flight = 6` in `bxm-4-64.h`. Correctly configured.

## Killed, and a correction to my own reading

`pvr_usc.c:1201` sets `b.shader->info.fs.uses_sample_shading = msaa`, which looked like forced
per-sample shading on any multisampled pipeline. **It is not**: the three such assignments
(`:1201`, `:806`, `:1386`) are inside **internal shader generation** (the `spm_load` render-target
load program), not the user fragment shader. The user shader's flag comes from
`sample_shading_enable` at `pvr_arch_pipeline.c:2743`, correctly. Calling it a bug from one line was
wrong.

## Status and next

Correct and excluded: GPU clock (1104 MHz both), tile partition (6144 both), tiles in flight (6, as
designed), phantoms (1, as designed), user sample shading, bytes/pixel.

Remaining: a per-sample fixed cost in the pixel-backend path - 2.4x at 1 sample, 4.45x at 4 samples,
independent of bytes.

Next probe: vary **samples** together with something that changes *how* the PBE processes them -
`minSampleShading`, or an MSAA target that never needs resolving - to separate "more samples cost
more" from "the resolve costs more". `SAMPLES=4` stays the sharp discriminator.
