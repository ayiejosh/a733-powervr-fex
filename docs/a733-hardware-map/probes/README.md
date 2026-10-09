# The probes behind the map

**Sources only.** These are the small programs that produced the map's claims; the built modules
(`*.ko`, `*.so`, binaries) and the vendor trees they were compiled against are **not** here, because
they are either build output or fetchable from the vendor BSP (the Makefiles record where from).

Every number in `../HARDWARE-MASTER-MAP.md` and `../MEASUREMENT-LOG.md` comes from one of these or from
`bench/pvr-vulkan/`.

| dir | what it establishes |
|---|---|
| **`ce/`** | **The crypto engine works, and is not worth using.** `ce-rng-test.c` drives `/dev/ce` and verifies the hardware RNG (**54–56/64 unique bytes, differs per run**). `ce-hash-test.c` hashes a known buffer and prints the digest — **byte-identical to `sha256sum`**, but **16.7 µs/call vs the CPU's 136 ns**. `Makefile` builds the BSP `drivers/ce` v5 path out-of-tree. **Do NOT load the socket variant: it registers at priority 260, and its hash path returns `-EINVAL` and crashes the kernel.** |
| **`g2d/`** | **The 2D engine is enabled but its engine will not execute.** `g2d-probe.c` does `G2D_CMD_QUERY_VERSION` (works, `0x10112114`). `g2d-fill-test.c` allocates from dma-heap and does a fill — **the driver's own parameter dump shows every field correct, and the IRQ never fires** (`G2D irq pending flag timeout`). |
| **`dmabuf/`** | **Zero-copy works, both import directions.** `dmabuf_render_test.c` — GBM on `renderD128` (**self** path). `dmabuf_foreign_test.c` — `/dev/dma_heap/system` (**foreign** path, the one a real pipeline uses). **Both: GPU fills 65536/65536 words, CPU readback matches, 0 mismatch.** |
| **`vpu/`** | **The vendor decode library will not construct an H.264 decoder.** `dectest.c` is prior work's decode test. It fails in `CreateSpecificDecoder` — and the format it passes, **`0x115`, IS `VIDEO_CODEC_FORMAT_H264`** (the vendor prints it with `%x`, which is what made it look like a bogus id). Ruled out: the format value, a header/library skew, the VE ops selection (`VE_DEC_MODE` vs `0`), and the VE hardware (the driver logs `enable_cedar_hw_clk`). See `../MEASUREMENT-LOG.md`. |

## Running them

**One VE session at a time.** Concurrent use of the video engine has crashed this board.
**Do not inspect `5400000.deinterlace`** — a read-only sysfs/IOMMU inspection produced a kernel NULL
dereference at `0x18` and a board reset, twice, reproducibly.
