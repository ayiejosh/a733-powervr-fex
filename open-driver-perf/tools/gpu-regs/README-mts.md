# MTS tools (Rogue B-series)

- `mtsdecode.c` - decoded MTS register snapshots/watchers (BGCTX, INTCTX, DM association, SCHEDULE, etc.
  per the vendor DDK `rgx_cr_defs_km.h` field layouts). `snap`, `watch <sec>`, `kick <probe>` modes.
- `kickpoke.c` - re-write the firmware-private client-kick register (`thread<<19 + 0xCE0 = 1`) during the
  native pending window; proved the request is a host-visible level that the host cannot re-trigger.
- `mtskick.c` - write documented `MTS_SCHEDULE`/DM2 values during the pending window; proved those are the
  firmware-GP channel and never start a client DM (bits latch in BGCTX and stay).
- `bgwatch.c` - high-rate sampler of `MTS_BGCTX` (0xBA0), EVENT_STATUS (0x130) and context reg 0x1A8;
  shows the working Vulkan kick raises events while only the DM0 firmware-GP bit toggles in BGCTX.

All are read/limited-write `/dev/mem` probes on the open stack; they need root and a running open driver.
