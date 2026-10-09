# BIG PICTURE — everything measured this session, both drivers, all stages

One instrument (`harness.py`), one command per probe, everything logged. Vendor figures below are from a
fresh sweep; open figures were taken earlier this session with the **same** instruments (the open driver is
currently unreachable — `kwin_x11` is alive and the guard correctly refuses to unbind).

## 1. Headline: the lag is TWO independent things

| component | open | vendor | **lag** |
|---|---|---|---|
| **fixed (non-shader) cost** | 13.01 ms | 5.99 ms | **2.17×** |
| **shader execution** | 240.66 ms | 173.68 ms | **1.39×** (was ~4.8×) |
| total (heavy shader) | 253.67 ms | 179.67 ms | **1.41×** |

## 2. Per-stage, all of it

| stage | open | vendor | lag | cause | fixable? |
|---|---|---|---|---|---|
| **geometry / TA job** | 0.35 ms | 0.82 ms | **0.43× — open WINS** | — | already ahead |
| **PR (partial render) job** | 9.31 ms | 2.31 ms | **4.03×** | unknown; Mesa says it performs *no* PRs when unneeded, yet costs 9.31 ms | unproven |
| **fragment fixed cost** | 13.01 ms | 5.99 ms | **2.17×** | firmware or command stream (all driver-visible config correct/maximal) | **not from source** |
| **shader execution** | 240.66 ms | 173.68 ms | **1.39×** | PCO codegen | **4 fixes landed** |
| **per-job sync / ioctl** | ~190 ioctls, **84% frame in kernel** | low (probe CPU 5–29% kernel) | **~17 ms/frame** | handle-based sync; kernel holds refs | **needs UAPI change** |
| **firmware** | `rogue_*.fw` | `rgx.fw` | unknown | different image, different ABI | **not interchangeable** |

## 3. Speed, bandwidth, correctness — one row per probe

| probe | open | vendor | correctness |
|---|---|---|---|
| `vkrender` 2048 | 13.7 ms · 306 Mpix/s | **5.80 ms · 723 Mpix/s** | PASS both |
| `vkrender` 512 | ~1.7 ms | **0.78 ms · 338 Mpix/s** | PASS both |
| `vkheavy` 2048 | 255.8 ms | **180.2 ms** | — / — |
| `cstp` no loop | 324 M/s | **328 M/s** | **at parity** |
| `cstpf` float loop | 87.9 | **139** | — |
| `cstpi` int loop | 72.8 | **140** | — |
| `cstpi1` 1-operand | 112 | **146** | — |
| `cstpin` registers | 71.3 | **146** | — |
| bandwidth | bpp=4 → ~1.2 GB/s @306 Mpix/s | bpp=4 → **2.9 GB/s @723 Mpix/s** | — |
| kernel share of probe CPU | **84% of frame** (client+compositor) | **5–29%** | — |

## 4. What was FIXED (4 commits, all `src/imagination/pco/`)

| commit | change | measured |
|---|---|---|
| `c2bde57` | unroll threshold 16 → 64 | 1.85× / 2.28× / 2.85× |
| `c251c9b` | block-local immediate hoisting | 1.47× / 1.31× / 1.18× |
| `5a1be21` | unroll 64 → 256 | 2.64× on 128-iteration loops |
| `167a943` | unroll 256 → 1024 | 2.68× on 512-iteration loops |
| **combined** | | **2.74× / 2.97× / 3.36×**; gap **5.53× → 2.02×**; suite **46 → 49** |

**Root cause of the shader gap, measured:** it was *instruction count* (2.73× ideal). Unrolling removed loop
control; hoisting removed **125 of 131** immediate materializations (131 → 6). The residue is register-file
moves forced by the assembler's ISA mapping (`pco_map.py`), largely inherent.

## 5. What was PROVEN impossible, and why

| target | proof |
|---|---|
| pool/recycle the per-job syncs | kernel resolves by handle and holds a reference (`pvr_sync.c:82`) → aliasing across in-flight jobs → **GPU hang** |
| timeline-back them | measured **worse** (3404 → 3604 ioctls, waits +50%) |
| spin-wait fix | A/B'd: **no change** |
| use the vendor's firmware | rewrapped, **loads, self-identifies as build 6603887**, then **DABT/level-2 translation fault** — the ABI |
| aggressive unroll limit | **null** for every probe, with unmeasured icache risk → withheld |
| find the render gap in driver config | every config read: tile 16×16, mtiles 4×4, region headers, **6 tiles in flight (device max)**, AA_NONE, PBE **<10%**, format/coverage-independent |

## 6. Method, corrected

**~65% of a core of uncontrollable background load (including this agent) ⇒ wall-clock FPS cannot resolve
below ~35%.** Everything above used throughput, kernel timestamps, or counts — all repeat to **~1%**. The one
client-FPS claim made this session was **withdrawn**.

## 7. Safety, exercised for real

The guard was tested by an actual fault: a bad firmware caused a kernel DABT, and the guard **restored the
firmware byte-exact (md5 `4b70eca8…`), fell back to `pvrsrvkm`, and rebooted** — unprompted. It then refused
a switch with `kwin is alive`. **The harness now enforces that rule itself.**
