# Reverse-engineering the PowerVR firmware — method and first results (2026-10-06)

The Rogue firmware is a stripped microMIPS binary with no symbols and no strings, so
the usual anchors are absent. This records how to get in anyway, because the method is
the reusable part.

## What the image is

`/lib/firmware/powervr/rogue_36.56.104.183_v1.fw`, 131072 bytes.

```
elf32-mips, EXEC, little-endian, entry 0xbfc00001
```

**microMIPS, not MIPS32.** The low bit of the entry point is the ISA-mode bit, and the
tell is that a plain MIPS32 scan finds **zero** `jr $ra` (0x03e00008) while a microMIPS
scan finds 52 `jr $ra` (0x45bf, 16-bit). Getting this wrong makes the whole image look
like garbage.

**Not encrypted, not compressed.** `.text` entropy is 6.908 bits/byte with 256 distinct
byte values and 10.4% zeros — ordinary compiled code. No unpacking step is needed.

Sections (file offset → VMA):

| section | file off | VMA | size |
|---|---|---|---|
| `.exctext` | 0x0d4 | 0x9fc02000 | 0xc10 |
| `.bootandnmitext` | 0xce4 | 0xbfc00000 | 0x6d4 |
| `.text` | 0x13b8 | 0xc0000000 | 0x16df4 |
| `.data` | 0x182c8 | 0xc0032008 | 0x360 |
| `.shdata` | 0x18640 | 0xc0032380 | 0x140 |
| `.shdatabss` | 0x18780 | 0xc00324c0 | 0x1784 |
| `.rodata` | 0x19f08 | 0xc0033c48 | 0x718 |

`.pdr`, `.mdebug.abi32` and `.reginfo` are present but `.pdr` does not parse as a clean
fixed-stride table, and there is no symbol table.

## Tooling

No MIPS disassembler was installed. `llvm-objdump` from the local llvm-mingw tree reads
the ELF but was built without a MIPS target; `binutils-mipsel-linux-gnu` would not
install (unmet deps). What worked:

```bash
pip3 install --target /home/radxa/pylibs capstone     # PEP 668 blocks a normal install
PYTHONPATH=/home/radxa/pylibs python3 ...
```

capstone 5.0.7 with `CS_ARCH_MIPS | CS_MODE_MIPS32 | CS_MODE_MICRO | CS_MODE_LITTLE_ENDIAN`.
`md.skipdata = True` is required or linear disassembly stops at the first odd byte.

Result: **29114 instructions, 588 bytes skipped (0.6%), 3.22 bytes/instruction** — the
right ratio for microMIPS, which mixes 16- and 32-bit encodings.

## The anchor: firmware trace IDs

The firmware emits numeric trace IDs; the **kernel** turns them into the text visible in
`pvr_fw/trace_0`. So the strings are not in the blob — but the IDs are, and the table
that names them is `pvr_rogue_fwif_sf.h` in the kernel driver.

Encoding, documented in that header:

```
bits  0-11  id number
bits 12-15  group id
bits 16-19  number of parameters
bits 28-30  active marker (0x7)
SFID = 0x70000000 | id | (group << 12) | (nparams << 16)
```

**The IDs are not stored as 32-bit literals.** Searching for the full constants finds
nothing. They appear as **16-bit `(id | group << 12)` values**, and the marker and
parameter count are added at the call site by a `lui`/`ori` pair. With that correction,
**519 of the 775 table entries are found in the image.**

Example — the context-deactivation path, which is also a live match against the trace
(`Deactivate MemCtx` is immediately followed by `Ungrab reg set 1 refcount now 0`):

```
0xc00054de  lui   $a0, 0x7001
0xc00054e2  ori   $a0, $a0, 0x8002      <- 0x70018002 = CREATESFID(2, BIF, 1) "Deactivate MemCtx=0x%08x"
0xc00054e6  jals  0x1f68                <- trace emit
0xc00054ea  move  $a1, $s1
...
0xc00054fa  lui   $a0, 0x7002
0xc0005500  ori   $a0, $a0, 0x8005      <- CREATESFID(5, BIF, 2) "Ungrab reg set %u refcount now %u"
0xc0005506  j     0x1f9c                <- tail-call trace emit
```

Both call sites are gated on the same flag:

```
0xc00054c8  lw    $v1, -0x1e1c($gp)
0xc00054cc  andi16 $v1, $v1, 0x20
0xc00054ce  bnez16 $v1, +0x4e           <- skip both traces if bit 5 is set
```

## Located message sites

File offsets of the 16-bit SFID constant, which sit immediately before the emit call:

| message | sfid16 | file offsets |
|---|---|---|
| `Perform TPC flush.` | 0x1049 | 0x12950, 0x12a3a, 0x12a7d |
| `Initiate powoff query for RD-DMs.` | 0xa004 | 0x16ce1 |
| `Kick 3D` | 0x1001 | 0x1e0f, 0x2323, 0x3035, 0x4691, 0x4753 |
| `Store Freelist` | 0x500d | 0x3809, 0x4ddd |
| `Deactivate MemCtx` | 0x8002 | 0x689c, 0xd78f, 0xd94b, 0xdc5f, 0x11204 |
| `Ungrab reg set` | 0x8005 | 0x68ba, 0x68de, 0x6bb9 |
| `Grab reg set` | 0x8004 | 0x6736, 0x1015f |
| `Loading stack-pointers` | 0x500c | 0x2b57, 0x2e99, 0x3961, 0x768f, 0xae0b, 0xb0e5 |
| `Phantom %u: USCTiles` | 0x1087 | 0x111a0, 0x11b68 |
| `FL different between TA/3D` | 0x4002 | 0x1407, 0x5b67, 0x60ef, 0x72f1, 0x7bc5, 0x7d53 |

`Kick TA` (0x1007) and `Updating Tiles In Flight` (0x1086) were not found as 16-bit
constants, so those two are constructed differently — not yet explained.

File offset → VMA: `vma = 0xc0000000 + (off - 0x13b8)`.

## Reference material

The TI DDK at `linuxws/scarthgap/k6.12/24.2.6643903` is public and dual MIT/GPL:

```bash
git clone --depth 1 --branch linuxws/scarthgap/k6.12/24.2.6643903 \
  https://git.ti.com/git/graphics/ti-img-rogue-driver.git
```

It carries `hwdefs/` (the `RGX_CR_*` register map the firmware itself programs),
`include/rgx_fwif_sf.h` and `services/server/devices/rogue/rgxfwutils.c`. **It does not
contain firmware source** — only `generated/rogue/rgxfwdbg_bridge`, a debug bridge.

Its SF table does **not** match this board's firmware (that tree is build 6643903, the
board is 6603887), and its ID packing differs slightly. The kernel driver's own
`pvr_rogue_fwif_sf.h` is the table that matches, and is the one to use.

## Where this leaves it

The firmware is readable: microMIPS, capstone, and trace IDs as anchors. Function
bodies can be found from any trace call site. What is not yet done is reading the
TA→3D transition sequence end to end, and no patch has been attempted.

## Correction: how the IDs are actually referenced

The "16-bit constant" search finds real call sites, but the earlier claim that the
marker and parameter count are always added by a `lui`/`ori` pair is only half right.
There are **two idioms**, and the SFID16 lands in a different place in each:

**A. `lui` + `ori`** — the MemCtx path (file 0x689c → vma 0xc00054e2):

```
0xc00054de  lui   $a0, 0x7001
0xc00054e2  ori   $a0, $a0, 0x8002      <- SFID16 is the high half of the ori word
0xc00054e6  jals  0x1f68
```

**B. `addiu $a0, $v0, <sfid16>`** — the TPC-flush path (file 0x12950 → vma 0xc0011596):

```
0xc001158e  lw     $a0, -0x16f8($gp)
0xc0011592  jal    0x1f34
0xc0011596  addiu  $a0, $v0, 0x1049     <- SFID 0x1049 = "Perform TPC flush."
```

**The trace emit functions are at 0x1f34, 0x1f68 and 0x1f9c.**

Practical consequence: the `ori`/`addiu` instruction word is 32-bit and self-delimiting,
so **decoding from `hit - 2` is self-aligning** and gives correct code even where a
linear sweep of the whole section has desynchronised. Do that rather than sweeping.

## Correction: the boundary test

A linear `skipdata=True` sweep from 0xc0000000 yields 29261 instructions and decodes the
MemCtx path correctly, but it **does desynchronise**, and using it to decide whether an
address is a valid instruction boundary gives wrong answers — it reported the MemCtx call
site as data even though decoding from `hit - 2` produces textbook code.

So: do not use a whole-section sweep to classify hits. Decode from the hit.

## Status of the sites

Working (decode cleanly from `hit - 2`):

- `Perform TPC flush.` — 0x12950, 0x12a3a, 0x12a7d
- `Deactivate MemCtx` / `Ungrab reg set` — 0x689c / 0x68ba

Not yet resolved — these decode as `.byte` garbage from `hit - 2`, so they are either
data or use a third idiom:

- `Initiate powoff query` (0x16ce1)
- `Kick 3D`, `Store Freelist`, `Loading stack-pointers`, `Phantom`
- `FL different between TA/3D` at 0x1407 and 0x5b67 etc.

`Kick TA` (0x1007) and `Updating Tiles In Flight` (0x1086) are not found as 16-bit
constants at all.

## Where this leaves it

The firmware is readable and trace call sites can be located and decoded reliably. What
is **not** done: the TA→3D transition has not been read end to end, no patch has been
built, and no modified image has been loaded. The next step is to resolve the remaining
idiom(s) so every site in the 41-operation gap can be read, then reconstruct the
sequence.
