# The A733 hardware map

Produced by 73 rounds of measurement on a Radxa Cubie A7A (Allwinner A733), 2026-10-09.

## Start here

**`HARDWARE-MASTER-MAP.md`** (655 lines) — **read the banner at the top first.** The document is
**layered**: an original status table with correction sections appended below it, and **six rows in that
table are superseded**. The banner names them and says what the later sections establish.

## The headline finding

> **The A733's problem is neither silicon nor power. It is software wiring, in two forms.**

1. **140 of 221 device-tree nodes are `disabled`** — **63% of the described hardware ships switched off**,
   across 55 device types.
2. **The accelerators that ARE enabled are unwired** — `g2d`, the crypto engine, VPU decode and the
   deinterlacer are each reachable only through a private or absent interface, **while the CPU does the
   work in software.**

## The documents

| file | what it is |
|---|---|
| `HARDWARE-MASTER-MAP.md` | the master map, with a read-me-first banner |
| `BLOCK-INVENTORY.md` | the device-tree block inventory |
| `CPU-RAM-POWER-MAP.md` | CPU, DRAM, thermal and power domains |
| `VPU-MAP.md` · `VPU-MOTION-VECTORS.md` | the VPU, and what the motion-vector API actually exposes |
| `NPU-MAP.md` | the VIP9000 |
| `G2D-UNLOCK.md` · `G2D-BUILD.md` | the 2D engine: why it was dead, and how its driver was built |
| `LSFG-MAKO-RESEARCH.md` | frame-generation feasibility — **with a correction banner: its shading rates are ~1.8x low** |
| `PRIOR-WORK-AUDIT.md` | which previously recorded numbers held up, and which did not |
| `CORRECTIONS-ROUND2.md` · `LEAD-INTEGRATION-FINDINGS.md` | the correction trail |
| `MEASUREMENT-LOG.md` | **the dated measurement log, 13,910 lines** — every claim traces to an entry here |

## Honest limitations

**40 self-corrections** are on the record. **Several remaining questions are blocked externally, not
unanswered:** `g2d`'s engine (needs a datasheet) · the encoder bitstream (vendor `libvencoder`) ·
the zero-copy integration (vendor library / OMX / VCS) · 9 VI scalers (inside the disabled VIN stack) ·
camera (no hardware). **Cache geometry and the DRAM part number are not exposed by this kernel.**
