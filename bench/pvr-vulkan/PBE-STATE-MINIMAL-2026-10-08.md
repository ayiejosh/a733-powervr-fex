# The PBE state is minimal - pbe_emits=1, tile_buffers_count=0

The internal `spm_load` shaders are named "spm_load(4 output regs, **7 tile buffers**, ...)", suggesting
the driver might allocate the maximum tile-buffer count for a single render target - which would
multiply the PBE's work. Instrumented `pvr_setup_emit_state`:

```
[tb] pbe_emits=1 tile_buffers_count=0 eot_surfaces=1
```

* **`pbe_emits = 1`** - one emit for one colour attachment.
* **`tile_buffers_count = 0`** - **no tile buffers at all**; the render target is written straight to
  memory. "7 tile buffers" is the *maximum the internal program supports*, not the count in use.
* `eot_surfaces = 1`.

**The PBE state is already minimal.** So the 3.2x PBE-write deficit is not extra emits, tile buffers, or
EOT surfaces - it is the cost of the per-pixel write processing the firmware does on state that looks
correct.

## Closes the last Mesa-side suspicion about the PBE state

Excluded for the PBE write term, each by measurement: bytes/pixel (flat), colour format (flat),
load/store ops (no effect), packmode/components (format-derived), emit count (1), tile buffer count (0),
EOT surface count (1), sample-rate mode (null), shader instructions (null).

**What remains is the firmware's per-pixel write processing itself** - closed source, no counter on this
SoC. Same boundary as the Tiler half.

## Session tally of refuted hypotheses

Fourteen before this round, fifteen now. The pattern is consistent: **every Mesa-side hypothesis
reachable by configuration or reading has been tested and refuted**, and the two remaining terms (PBE
write 3.2x, Tiler 4x-MSAA excess) both bottom out at the same place - the closed firmware, needing
either the vendor's command stream or PVRtune.
