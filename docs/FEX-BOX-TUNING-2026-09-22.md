# Making the x86-64 emulation stack faster on the Cubie A7A

**Board:** Radxa Cubie A7A, Allwinner A733 — 6× Cortex-A55 (1794 MHz) + 2× Cortex-A76 (2002 MHz), ~6 GB LPDDR5, PowerVR BXM-4-64.
**Date:** 2026-09-22. **Kernel:** 6.6.98-5-aw2511 (untouched by this work).

This is a measurement-first study. Two of its most useful results are negative, and
one of them is a correction to a claim in the version of this document that would
have been written from documentation alone.

---

## 1. What the stack actually is

Worth stating precisely, because the obvious reading is wrong and it changes which
knobs matter:

| Path | What runs the guest code | Where the tuning lives |
|---|---|---|
| **x86-64 Linux** (`fexrun`, `steam-fex`) | **FEX-2609** at `/opt/fex/bin/FEX` | `~/.fex-emu/Config.json` + `FEX_*` env |
| **Windows programs, 32-bit PE** (`d3drun`, `winrun`, `guirun`) | **`wowbox64.dll`** (box64 0.4.4) inside Wine 11.16 **Hangover** | `<prefix>/drive_c/users/radxa/.box64rc` — **not** `/etc/box64.box64rc` |
| **DXVK** (d3d9/11, dxgi) | **native ARM64/ARM64EC** (DXVK-Sarek v1.14.0) | `/home/radxa/dxvk.conf` |
| Windows API side (ntdll, cmd, …) | **native ARM64** builtins | — |

Consequences that were verified on the board rather than assumed:

- `wine cmd /c ver` prints **no** `[BOX64]` banner — the 64-bit builtin is native ARM64.
  `wine 'C:\windows\syswow64\cmd.exe' /c ver` **does** print the banner. So the emulated
  path is the 32-bit PE path, and it is reachable for testing without a game.
- There is no `x86_64-windows` PE directory in this Wine: the builtins are `i386-windows`
  and `aarch64-windows`. 64-bit Windows *applications* are emulated by `wowbox64.dll`,
  while the API layer they call into is native.
- Both backends ship (`wowbox64.dll` and `libwow64fex.dll`), so the same Windows program
  can be run under box64 or under FEX by changing `HODLL`. **This A/B has not been run
  yet** and is the most promising unexplored item in this document.
- `FEXServer` is already resident (`--persistent=999999`), so repeated FEX launches
  already avoid re-reading the rootfs.

---

## 2. Baseline: how much does emulation cost here?

`fexbench.c` (in `emulation/bench/`) isolates one translation category per test.
Same source compiled for both ISAs, static, pinned to one A76 core, minimum of 3
interleaved rounds:

| test | native A76 | FEX-2609 | ÷native | box64 0.4.4 | ÷native |
|---|---|---|---|---|---|
| `alu` (64-bit int chain) | 750 ms | 1053 ms | 1.40× | 1093 ms | 1.46× |
| `branch` (50M unpredictable) | 101 ms | 213 ms | 2.12× | 209 ms | 2.08× |
| `fp_double` (SSE2 chain) | 200 ms | 550 ms | **2.75×** | 550 ms | **2.75×** |
| `fp_x87` (80-bit chain) | n/a | 114 ms | — | **66 ms** | — |
| `memcpy` (1.28 GB) | 252 ms | 331 ms | 1.31× | 550 ms | 2.18× |
| `memloop` (byte copy) | 27 ms | 35 ms | 1.29× | 47 ms | 1.75× |
| `syscall` (200k `getpid`) | 33 ms | 46 ms | 1.39× | 44 ms | 1.33× |
| **total** | **1357 ms** | **2342 ms** | **1.73×** | **2527 ms** | **1.86×** |
| `atomics` (4×5M, threaded) | 498 ms | 991 ms | **1.99×** | *crashed* | — |
| `threads` (4×ALU) | 399 ms | 462 ms | 1.16× | *crashed* | — |

Read that as: **FEX is the faster emulator on this board overall (1.73× vs 1.86× cost),
box64 wins x87 (66 vs 114 ms) and loses badly on bulk memory copies (2.18× vs 1.31×).**
Scalar float is the worst category for both — 2.75× — and is where a game's physics
and animation math lives.

### Correctness check built into the benchmark

Every test ends in a checksum. FEX and box64 produce **bit-identical** results to each
other on every test (`alu` = 14814868388957047875, `branch` = 16347769694003629214,
`atomics` = exactly 20000000 = 4 × 5,000,000). The aarch64 build differs on `fp_double`
only, and for a legitimate reason: GCC contracts `a*b+c` into an FMA on ARM but not on
baseline x86-64. So the emulators are both faithful to x86 semantics here.

### A cheap lesson about benchmark choice

The pre-existing `cpubench/bench.c` on this board reported native 4.73 s, FEX 6.70 s,
box64 7.47 s — but **82% of FEX's time was a single `qsort` test**, i.e. emulated *guest
libc*, not JIT quality. A "CPU benchmark" that spends most of its time inside guest libc
measures the library, not the translator. `fexbench` avoids libc in every test except
`memcpy`, deliberately.

Also worth knowing: the old benchmark's `mem_copy` test reports `0.0 ms` because GCC
deletes the whole `memset`+`memcpy` pair (dead stores into freed memory). It has never
measured anything. `fexbench` prevents this with a checksum over the destination.

---

## 3. The FEX knob surface is already exhausted

15 configurations swept through `FEX_*` env overrides, interleaved, pinned. Baseline
total reproduced to **0.08%** between rounds (2371.6 / 2369.8 ms), so the null results
below are real and not noise:

| variant | total | verdict |
|---|---|---|
| baseline (board config) | 2371.6 ms | — |
| `FEX_DYNAMICL1CACHE=0` / `=1` | 2349.9 / 2344.4 | noise |
| `FEX_SMCCHECKS=0` | 2385.1 | noise |
| `FEX_DISABLEL2CACHE=1` | 2345.5 | noise |
| `FEX_VOLATILEMETADATA=0` / `EXTENDED…=0` | 2366.3 / 2365.6 | noise |
| `FEX_MEMCPYSETTSOENABLED=0` | 2371.0 | noise |
| `FEX_HALFBARRIERTSOENABLED=0` | 2372.2 | noise |
| `FEX_VECTORTSOENABLED=0` | 2354.7 | noise |
| `FEX_KERNELUNALIGNEDATOMICBACKPATCHING=1` | 2353.7 | noise |
| `FEX_HIDEHYBRID=1` | 2353.3 | noise |
| `FEX_DISABLE_VIXL_INDIRECT_RUNTIME_CALLS=1` | 2345.5 | noise |
| **`FEX_X87REDUCEDPRECISION=0`** | **3762.3** | **control fires: `fp_x87` 113 → 1536 ms (13.5×)** |

The x87 control proves the overrides really apply, and confirms the existing
`X87ReducedPrecision: "1"` is worth **13.5×** on x87-heavy (mostly 32-bit and older
Windows) code. Nothing else on the surface moves this workload class.

**Two controls that did *not* fire, and why that is a finding about the benchmark:**

- `FEX_TSOENABLED=1` — no change (2357.2 ms), because the `atomics` test uses
  `lock`-prefixed RMW instructions, which are serialising on x86 *regardless* of TSO.
  An atomic benchmark cannot measure the TSO setting. An independent shell-loop
  measurement puts TSO on at **−11%** versus off.
- `FEX_MULTIBLOCK=0` — no change (2373.0 ms), because the hot loops here are single
  JIT blocks; multi-block only merges *sequential* blocks. The same independent
  measurement puts `Multiblock=0` at **+111%** (i.e. 2.1× slower).

So: **the installed FEX config is at the plateau for this workload, and the remaining
headroom in FEX is in launch cost and thunking, not in JIT options** (see §6).

---

## 4. Core selection: the largest lever, and it cuts both ways

Clean serialised run, minimum of 3, single-threaded x86-64 under FEX:

| test | on an A55 | on an A76 | speedup |
|---|---|---|---|
| `alu` | 1510 ms | 1054 ms | 1.43× |
| `branch` | 420 ms | 213 ms | 1.97× |
| `fp_double` | 671 ms | 550 ms | 1.22× |
| `memcpy` | 793 ms | 336 ms | **2.36×** |
| **total** | **3394 ms** | **2152 ms** | **1.58×** |

The clocks differ by only 11.6% (2002 vs 1794 MHz), so this is mostly IPC and memory
subsystem, not frequency.

And the other direction:

| threaded test | all 8 cores | pinned to 2 A76 |
|---|---|---|
| 4× ALU threads | 464 ms | **718 ms (1.55× worse)** |
| 4-thread atomic contention | 1010 ms | **826 ms (1.23× better)** |

**Rule: pin single-threaded work to an A76; never pin a multithreaded title.** Blanket
`taskset` in the game wrappers would be a pessimisation, which is why none was added.

*Method note:* the first attempt at this comparison put "A55 faster than A76 by 1.8×"
on the screen, because a disk-cache test was running on the other cluster at the time.
Every timing number here comes from a run where nothing else was executing. That is not
a formality on a board with syncthing, udev workers and a web harness in the background.

---

## 5. The one config change that was actually applied

**`/etc/box64.box64rc` is dead config for Windows programs on this board.** That file
contains a carefully tuned set of DXVK sections which have never applied, because
`wowbox64.dll` reads the rcfile from `%USERPROFILE%`, not from `/etc` and not from
`~/.box64rc`. Before this change, **zero box64 settings were in force for `d3drun`.**

Proven by putting `BOX64_NOBANNER=1` in each candidate location and counting the
`[BOX64]` banner lines from `wine 'C:\windows\syswow64\cmd.exe' /c ver`:

| rcfile location | banner lines | read? |
|---|---|---|
| none | 2 | — |
| `~/.box64rc` | 2 | **no** |
| `/etc/box64.box64rc` | 2 | **no** |
| `<prefix>/drive_c/users/radxa/.box64rc` | **0** | **yes** |

Written to both `~/.wine-dxvk` and `~/.wine-hangover`, with `[*setup*]`/`[*install*]`
safety sections first and then a global section carrying the settings measured on this
board in earlier paired A/B work: `CALLRET=1` (**−9.4%**), `SAFEFLAGS=0` (**−4.2%**,
≈−15% together), `BIGBLOCK=2`, `FORWARD=512`. `NATIVEFLAGS=0` (+35%) and
`ALIGNED_ATOMICS=1` are deliberately omitted, with the reasons recorded in the file.

### box64 crash found while testing this

`box64` 0.4.4 aborts on a 4-thread atomic workload in a **static** glibc binary:

```
Fatal glibc error: pthread_mutex_lock.c:94 (___pthread_mutex_lock):
  assertion failed: mutex->__data.__owner == 0
```

| setting | result |
|---|---|
| defaults | crash |
| `BOX64_DYNAREC_SAFEFLAGS=2` | crash |
| `BOX64_DYNAREC_ALIGNED_ATOMICS=1` | crash |
| `BOX64_DYNAREC_BIGBLOCK=0` | passes, checksum exact |
| `BOX64_DYNAREC_STRONGMEM=2` | passes, checksum exact |

This is a static-glibc pattern, not the Wine/PE path, so it is not necessarily a
game-path bug — but it is reproducible and it is the reason `ALIGNED_ATOMICS` is not
in the new rcfile.

---

## 6. Remaining headroom, ranked

1. **A/B the Windows backend: `HODLL=libwow64fex.dll` vs `wowbox64.dll`.** Both ship.
   Given FEX beats box64 by 1.73× vs 1.86× on the Linux side and wins bulk memory
   copies by a wide margin, the FEX backend may well be faster for games — and it costs
   one environment variable to find out. *Untested; highest expected value.*
2. **Thunk ALSA for the FEX path.** `libasound` ships in `/opt/fex/lib/fex-emu/HostThunks/`
   but `ThunksDB` enables only `Vulkan` and `drm`, so guest audio is emulated. Adding
   `"asound": 1` moves it to native. Not applied here: it changes audio behaviour and
   audio output could not be verified from this session. Do **not** thunk GL/EGL —
   display `:0` exports no GLX, which is why the nested-Xephyr path exists.
3. **`FEX_DISKCACHE=1`** (new in 2609): measured **0.418 s → 0.378 s** (~10%) on a 0.4 s
   guest launch, cache 1.6 MB. Real but small on this workload, and upstream notes it
   grows without bound and is not invalidated when guest files change. Worth testing on
   an actual title before adopting; not enabled by default.
4. **Scope `TSOEnabled` per application** — see §7.
5. **`dxvk.numCompilerThreads` / `shaderCompilationMethod`** are already set to the
   conservative values in `/home/radxa/dxvk.conf`; DXVK-Sarek upstream recommends
   `DXVK_FRAME_RATE_PACING=sliced` under Box64/FEX when a frame limiter is active.

---

## 7. A risk this study measured instead of repeating

`~/.fex-emu/Config.json` sets `"TSOEnabled": "0"`. Upstream calls that *"highly likely
to break any multithreaded application if disabled"*, and failures are silent: wrong
results, corrupt saves, anti-cheat flags. Nobody had measured it here, so `tsolitmus.c`
does: a writer stores a uniquely-valued pair to two **cache-line-separated** fields with
plain stores; a reader flags any pair whose halves disagree. Under x86 TSO the second
store cannot become visible before the first.

| run | samples | true store→store reorders |
|---|---|---|
| native ARM64 — **positive control, must be able to fire** | 2.37 M | **210** |
| FEX with the board's `TSOEnabled:"0"` | 2.15 M | **35** |
| FEX with `FEX_TSOENABLED=1` — **negative control** | 1.93 M | **0** |

(The detector only became valid once the two fields were a cache line apart. The first
version put them adjacent, reported 0 everywhere *including native ARM*, and was
therefore proving nothing.)

**Conclusion: the risk is real but small here — about 1.6×10⁻⁵ of store pairs were
observed out of order. TSO emulation on eliminates it entirely at roughly −11%
throughput.** A lock-free multithreaded x86-64 program has a genuine, if low, chance of
silent misbehaviour on this board as configured. The appropriate response is per-app
scoping (`~/.fex-emu/AppConfig/<App>.json` with `TSOEnabled: "1"`) for titles that are
multithreaded *and* lock-free-heavy, rather than turning it on globally and paying 11%
everywhere.

---

## 8. Automatic post-reboot continuation (harness side)

The harness runs at boot, so a boot-time hook can tell the agent that the machine
rebooted — no human typing. Implemented as a Host plugin, `dsh-boot-notify`
(`~/.dsh/local-bundles/dsh-boot-notify/`), installed into the `web` profile.

It delivers through `sessionController.prompt()`, the same documented service the
browser itself uses ("admit one prompt after explicitly resuming its Session"), so a
rebooted harness with no page open still delivers, and the message appears in the chat
as a normal user message.

Three triggers, one send path:

| trigger | behaviour |
|---|---|
| **boot** | kernel `boot_id` differs from the recorded one → compose message, deliver |
| **outbox** | any file dropped in `~/.dsh/boot-notify/outbox/` → delivered within ~5 s |
| **poll** | re-checks the outbox every few seconds |

Design decisions that matter:

- **It does not fire on first run** (nothing to compare against) and **does not fire on
  a harness restart** — only on a real boot-id change. Verified: `boot-first-run`
  recorded the id and stayed quiet; `boot-unchanged` logged *"harness restart, not a
  reboot"* and stayed quiet.
- **A boot with nothing armed stays quiet**, because every message costs a full-context
  model turn. Arm it by writing `~/.dsh/boot-notify/pending.md`; the file's contents are
  embedded in the message. Set `requirePending: false` plus `bareMessage` to be notified
  on every boot regardless. It is armed now with a standing post-reboot state check.
- Everything it knows lives in `~/.dsh/boot-notify/` (`config.json`, `state.json`,
  `pending.md`, `log.jsonl`) — **not** in the composition, so the target session can
  change without editing a profile.

Verified end-to-end without rebooting: the outbox path delivered into this very session
unattended (`sent … sessionId: session-0fe00149…`), and the boot branch was exercised by
seeding a stale boot id, producing `boot-detected` with real kernel/uptime facts and
then `send-failed attempt 1/2` retries against a deliberately non-existent session. The
only step not exercised is a literal power cycle; the comparison it depends on was
demonstrated firing on a boot-id mismatch.

---

## 9. Reproducing all of it

`emulation/bench/` in this repository contains every tool, all dependency-free:

| file | purpose |
|---|---|
| `fexbench.c` | the 9-category microbenchmark, checksums included, builds for both ISAs |
| `tsolitmus.c` | the TSO/ordering detector, with its positive control |
| `matrix.sh` | interleaved A/B driver (round-robin, minimum-of-N, pinning) |
| `sweep-fex.sh` | the 15-configuration FEX knob sweep |
| `clean-measure.sh` | core-selection and DiskCache measurements, serialised |

```bash
gcc -O2 -static -pthread -o build/fexbench_arm64 fexbench.c
x86_64-linux-gnu-gcc -O2 -static -pthread -o build/fexbench_x64 fexbench.c
REPS=3 ./matrix.sh                      # native vs FEX vs box64
REPS=3 ./sweep-fex.sh                   # FEX knob surface
./clean-measure.sh                      # core selection + disk cache
```
