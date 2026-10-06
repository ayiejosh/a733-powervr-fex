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

## Solved: the third idiom, and a full trace-site map

The sites that would not decode were being looked for in the wrong place. **The SFID is
in the `jal` delay slot, after the call, not before it:**

```
0xc0001c12  lui    $a0, 0x7000        <- marker + nparams
0xc0001c16  jal    0x1f34             <- trace emit
0xc0001c1a  addiu  $a0, $a0, 0x1090   <- SFID16, in the delay slot
```

So the layout is `lui` at -8, `jal` at -4, `addiu` at 0. Searching for the 16-bit value
as a standalone constant finds real sites only when it happens to be the high half of a
32-bit word at an even offset; otherwise it is a coincidental match inside other data,
which is exactly what the "garbage" sites were.

**Robust method:** find every `jal 0x1f34` (bytes `00 f4 9a 0f`), then read the SFID from
the instruction at `jal + 4`. That is alignment-independent and needs no guessing.

44 call sites found, 43 resolved:

| file | vma | sfid | group | message |
|---|---|---|---|---|
| 0x02a0c | 0xc0001654 | 0x1044 | MAIN | GPU init |
| 0x02fce | 0xc0001c16 | 0x1090 | MAIN | GPU deinit |
| 0x02ffc | 0xc0001c44 | 0x1091 | MAIN | GPU units deinit |
| 0x07d80 | 0xc00069c8 | 0x6023 | SPM | SPM State = wait for HW |
| 0x08f02 | 0xc0007b4a | 0x700e | MTS | Irq Task complete. |
| 0x0bce6 | 0xc000a92e | 0xa033 | POW | Power controller returned ABORT for last request so retry |
| 0x0be58 | 0xc000aaa0 | 0xa033 | POW | Power controller returned ABORT for last request so retry |
| 0x0c3b4 | 0xc000affc | 0xa019 | POW | Null command executed, repeating initiate powoff query |
| 0x0c4e6 | 0xc000b12e | 0xa00d | POW | Initiate powoff query for RD-DMs. |
| 0x0de32 | 0xc000ca7a | 0xb01b | HWR | Analysis: Need freelist reconstruction |
| 0x0e846 | 0xc000d48e | 0x10c7 | MAIN | GPU has locked up |
| 0x0f752 | 0xc000e39a | 0xb026 | HWR | GPU has overrun its deadline |
| 0x0f784 | 0xc000e3cc | 0x104b | MAIN | HWR has been triggered - deadline |
| 0x0f8ce | 0xc000e516 | 0xb027 | HWR | GPU has failed a poll |
| 0x0f900 | 0xc000e548 | 0x104c | MAIN | HWR has been triggered - poll |
| 0x101ba | 0xc000ee02 | 0x1006 | MAIN | Compute finished |
| 0x106e0 | 0xc000f328 | 0x1023 | MAIN | Unsetting BP Registers |
| 0x11410 | 0xc0010058 | 0x1030 | MAIN | No Depth/Stencil Buffer used for partial render (load) |
| 0x11730 | 0xc0010378 | 0x102f | MAIN | No ZS Buffer used for partial render (store) |
| 0x117c4 | 0xc001040c | 0x300b | CSW | *** 3D context store start |
| 0x12210 | 0xc0010e58 | 0x1004 | MAIN | 3D Transfer finished |
| 0x122aa | 0xc0010ef2 | 0x3009 | CSW | *** 3D context store complete |
| 0x1280a | 0xc0011452 | 0x1008 | MAIN | TA finished |
| 0x1294a | 0xc0011592 | 0x1049 | MAIN | Perform TPC flush. |
| 0x12a10 | 0xc0011658 | 0x5007 | RTD | Perform VHEAP table store |
| 0x12a34 | 0xc001167c | 0x1049 | MAIN | Perform TPC flush. |
| 0x12a42 | 0xc001168a | 0x3010 | CSW | *** TA context store complete |
| 0x12c66 | 0xc00118ae | 0x3011 | CSW | *** TA context store start |
| 0x141b8 | 0xc0012e00 | 0x6021 | SPM | SPM State = PR blocked |
| 0x141d8 | 0xc0012e20 | 0x6025 | SPM | SPM State = PR avoided |
| 0x156de | 0xc0014326 | 0x6021 | SPM | SPM State = PR blocked |
| 0x15b1c | 0xc0014764 | 0x6025 | SPM | SPM State = PR avoided |
| 0x15b88 | 0xc00147d0 | 0x600b | SPM | Partial Render finished |
| 0x15bba | 0xc0014802 | 0x6026 | SPM | SPM State = PR executed |
| 0x15cf6 | 0xc001493e | 0x6025 | SPM | SPM State = PR avoided |
| 0x15f9e | 0xc0014be6 | 0x6020 | SPM | SPM State = none |
| 0x15fec | 0xc0014c34 | 0x100a | MAIN | Resume TA without partial render |
| 0x16092 | 0xc0014cda | 0x1009 | MAIN | Restart TA after partial render |
| 0x16c78 | 0xc00158c0 | 0x1006 | MAIN | Compute finished |
| 0x16d9e | 0xc00159e6 | 0x3004 | CSW | *** CDM FWCtx store complete |

Only the first 40 rows are shown here; the full list is 43.

## What this map says about the TA→3D transition

The gap is **not** an opaque delay. It is, in the firmware's own vocabulary, a
**context store sequence**:

```
*** TA context store start     0xc00118ae
TA finished                    0xc0011452
Perform TPC flush.             0xc0011592 / 0xc001167c
Perform VHEAP table store      0xc0011658
*** TA context store complete  0xc001168a
...
*** 3D context store start     0xc001040c
*** 3D context store complete  0xc0010ef2
```

with a power-management decision in the middle (`Initiate powoff query for RD-DMs.`
0xc000b12e, and `Null command executed, repeating initiate powoff query` 0xc000affc),
and the SPM partial-render state machine running alongside (`SPM State = PR blocked /
avoided / executed / none`, `Partial Render finished`, `Resume TA without partial
render`, `Restart TA after partial render`).

So the 176 µs is TA context store + power query + SPM partial-render arbitration + 3D
context store. Every one of those is deliberate state management, which is consistent
with the earlier finding that it does not amortise and does not scale with pixels.

## Next

Read the two context-store routines and the powoff routine end to end from these
anchors, and identify which parts are conditional and which are unconditional. Still
read-only; no patch built, no modified image loaded.
