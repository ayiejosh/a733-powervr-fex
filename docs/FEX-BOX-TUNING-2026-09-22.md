# Making the x86-64 emulation stack faster on the Cubie A7A

**Board:** Radxa Cubie A7A, Allwinner A733 — 6× Cortex-A55 (1794 MHz) + 2× Cortex-A76 (2002 MHz), ~6 GB LPDDR5, PowerVR BXM-4-64.
**Date:** 2026-09-22. **Kernel:** 6.6.98-5-aw2511 (untouched by this work).

This is a measurement-first study. Three of its most useful results are negative, and
one of them corrects an earlier revision of this very document: a box64 config change
was applied here, then found to hang the emulated Windows path, and reverted (§5). The
traps that hid the problem are worth as much as the findings.

---

## 1. What the stack actually is

Worth stating precisely, because the obvious reading is wrong and it changes which
knobs matter:

| Path | What runs the guest code | Where the tuning lives |
|---|---|---|
| **x86-64 Linux** (`fexrun`, `steam-fex`) | **FEX-2609** at `/opt/fex/bin/FEX` | `~/.fex-emu/Config.json` + `FEX_*` env |
| **Windows programs, 32-bit PE** (`d3drun`, `winrun`, `guirun`) | **`wowbox64.dll`** (box64 0.4.4) inside Wine 11.16 **Hangover** | `BOX64_*` environment variables — **not** a rcfile, which hangs this build (§5) |
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

## 5. The Windows path: a change that was applied, found harmful, and reverted

This section is a correction. An earlier revision of this document claimed that
`<prefix>/drive_c/users/radxa/.box64rc` was the rcfile WowBox64 actually reads, and
that writing it there fixed "zero box64 tuning in force". **That claim was wrong, the
evidence for it was a measurement artifact, and the change was removed.** What follows
is what survived re-testing.

### The rcfile route hangs this build

With a purge that verifiably empties the process table before every trial, and three
repetitions:

| rcfile state | run 1 | run 2 | run 3 |
|---|---|---|---|
| **absent** | 1.05 s ok | 1.03 s ok | 1.06 s ok |
| tuned content, mode 600 | HANG | HANG | HANG |
| tuned content, mode 644 | HANG | — | — |
| empty `[*]` | HANG | — | — |

Every one of seven different contents hung, including a file containing nothing but
`[*]`, at both file modes. Removing the file restored ~1.0 s every time, in both
`~/.wine-dxvk` and `~/.wine-hangover`. So this is not a settings problem — **creating
any `.box64rc` at `%USERPROFILE%` makes WowBox64-emulated processes hang on this build.**
`/etc/box64.box64rc` and `~/.box64rc` remain not-read (which is what made the distro's
tuned file inert in the first place).

### Three measurement traps, all of which produced a wrong answer first

1. **A count of zero from a killed process is not evidence of success.** The original
   "proof" was `timeout 90 wine … | grep -c BOX64`: it prints `0` both when a banner is
   genuinely suppressed *and* when the run is killed before printing anything. The `0`
   was a hang. The banner was never a valid signal for anything — later,
   `BOX64_NOBANNER=1` was reported as an applied override *and the banner still printed*.
2. **A timed-out Wine run poisons the prefix.** Orphaned `services.exe` /
   `winedevice.exe` / `plugplay.exe` survive it, and a stuck `winedevice.exe` ignores
   SIGTERM — it needs SIGKILL. Two bisect attempts produced confident nonsense
   ("every setting hangs, even an empty file") because every trial started against
   leftovers. Any Wine A/B on this board must purge with SIGKILL and **assert an empty
   process table** before each trial.
3. **Launch cost is not throughput.** `cmd /c ver` completes in ~1.0 s cold and ~0.2 s
   with a warm wineserver, and exercises almost no guest code.

### What is actually true, and usable

- **Environment variables are the working route**, and box64 proves it applied them
  itself:
  ```
  [BOX64] BOX64ENV: Variables overridden:
      BOX64_DYNAREC_CALLRET=1
      BOX64_DYNAREC_BIGBLOCK=2
  ```
- **`BOX64_DYNAREC_SAFEFLAGS=0` is a no-op here** — it is not reported as an override
  because `0` *is* the default. The "−4.2%" attributed to it in earlier session notes
  therefore does not apply to this build; there is nothing to gain.
- Confirmed settable and non-hanging: `CALLRET=1`, `SAFEFLAGS=1`, `BIGBLOCK=2/3`,
  `FORWARD=512`.
- **Both Windows CPU backends launch equally fast**: ~0.2 s warm / ~1.0 s cold for
  `wowbox64.dll` and the same for `HODLL=libwow64fex.dll`. An earlier reading of "FEX is
  100× faster" was the orphan poison, not a real difference.

Net effect on the live system: the rcfile was written, found harmful, and **deleted from
both prefixes**, each verified back at ~1.0–1.2 s. The contribution of this whole
section to speed is negative knowledge — the kind that is worth more than a hopeful
config file.

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
game-path bug — but it is reproducible, and it is a reason to leave
`ALIGNED_ATOMICS` alone.

---

## 6. Remaining headroom, ranked

1. **A/B the Windows backend on a real title: `HODLL=libwow64fex.dll` vs
   `wowbox64.dll`.** *Answered for throughput in §10 — FEX's backend won by 17.1%.
   What remains open is whether a real game agrees, since the workload used was a
   console builtin rather than a title.*
2. **Thunk ALSA for the FEX path.** `libasound` ships in `/opt/fex/lib/fex-emu/HostThunks/`
   but `ThunksDB` enables only `Vulkan` and `drm`, so guest audio is emulated. Adding
   `"asound": 1` moves it to native. Not applied here: it changes audio behaviour and
   audio output could not be verified from this session. Do **not** thunk GL/EGL —
   display `:0` exports no GLX, which is why the nested-Xephyr path exists.
3. **`FEX_DISKCACHE=1`** (new in 2609) — **corrected in §11: it is −64%, not ~10%.**
   The figure here was measured on a cache that had not finished warming and against a
   workload that barely compiles anything. See §11 for the proper numbers and the
   invalidate-on-update caveat.
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
| `wine-throughput-ab.sh` | the Windows-path throughput A/B that produced §10 |
| `wine-backend-ab.sh` | box64 vs FEX Windows backend, with the SIGKILL purge discipline and a refusal to run against leftovers |
| `box64-rcfile-ab-clean.sh` | the A/B that established the rcfile hang |
| `box64-rcfile-salvage.sh` | mode/content variants, and the recovery check that proved removal fixes it |

The two earlier bisect attempts are deliberately **not** included: they are the ones
that produced wrong answers by running against a poisoned prefix, and keeping them
would invite repeating the mistake. Their lesson is in the comments of the survivors.

```bash
gcc -O2 -static -pthread -o build/fexbench_arm64 fexbench.c
x86_64-linux-gnu-gcc -O2 -static -pthread -o build/fexbench_x64 fexbench.c
REPS=3 ./matrix.sh                      # native vs FEX vs box64
REPS=3 ./sweep-fex.sh                   # FEX knob surface
./clean-measure.sh                      # core selection + disk cache
REPS=3 ./wine-throughput-ab.sh          # Windows emulated throughput, with the purge discipline
```

---

## 10. The Windows path measured properly, and the change that was applied

`cmd /c ver` cannot rank a codegen setting. An emulated i386 PE builtin doing real work
can: `syswow64\find.exe /C the C:\bench200.txt` over a 198 MB / 4,000,000-line file,
with a full SIGKILL purge and an asserted-empty process table before every run, and the
line count checked on every run.

| variant | run 1 | run 2 | run 3 | best | vs default |
|---|---|---|---|---|---|
| box64 default | 6.37 | 6.35 | 6.25 | 6.25 s | — |
| box64 `CALLRET=1` | 5.76 | 5.75 | 5.85 | **5.75 s** | **−8.0%** |
| box64 `CALLRET=1 BIGBLOCK=3 FORWARD=512` | 5.98 | 5.96 | 6.08 | 5.96 s | −4.6% |
| **FEX backend** (`HODLL=libwow64fex.dll`) | 5.27 | 5.27 | 5.18 | **5.18 s** | **−17.1%** |

Every run exited 0 and printed exactly `4000000` lines, so none of this speed is bought
with wrong results.

Three things follow:

1. **`CALLRET=1` is worth 8.0% here, measured first-hand** — consistent with the −9.4%
   in earlier session notes, and it is the same setting the stock `/etc/box64.box64rc`
   already applies to the DXVK DLLs.
2. **`BIGBLOCK=3` + `FORWARD=512` gave back most of that gain** (5.96 s vs 5.75 s).
   Adding more settings from a list of "known good" options made it *slower*; that is
   the argument for measuring each one on the workload you actually care about.
3. **FEX's Windows backend is 17.1% faster than box64's for emulated work.** This was
   the open question, and the answer is not what the Linux-side numbers predicted —
   box64 looked competitive there thanks to its x87 advantage, but on this workload it
   is simply slower.

### Applied

`BOX64_DYNAREC_CALLRET=1` is now exported by **`d3drun`, `winrun` and `guirun`** — the
only route that works, since the rcfile route hangs (§5). Verified live: box64 self-reports
`BOX64ENV: Variables overridden: BOX64_DYNAREC_CALLRET=1` through both `d3drun` and
`winrun`, both still start in ~1.1 s, and the wrapper diffs are pure insertions.

- Originals archived at `/home/radxa/fex-tune/wrapper-backups/`.
- Revert for a single run: `BOX64_DYNAREC_CALLRET=0 d3drun <app.exe>`.
- Risk, stated plainly: `CALLRET` assumes a callee does not read the caller's flags,
  which is why the distro file scopes it to specific DLLs rather than globally. Output
  was identical on this workload, but if a title renders or saves wrongly this is the
  first line to remove.

**Not applied, deliberately:** switching the backend to FEX. It is the bigger win, but
it replaces the emulator for every Windows app and the evidence is a console builtin,
not a game. It is one commented line in each wrapper.

---

## 11. JIT cost: where it actually is, and the two wins

"Can the JIT be optimized further?" splits into two costs that need different fixes:

- **compile cost** — paid once per translated block. This is what launch delay and
  in-game stutter are made of.
- **run cost** — paid per block dispatch and per emulated instruction, and set by the
  *quality* of the emitted ARM64.

Both JITs are built from source on this board (`FEX-2609/build/`, `box64-0.4.4-src/build-a76/`),
so build flags are in scope as well as runtime knobs.

### The emitted code is invariant — all the recoverable cost is compilation

Across every knob tested, the nine benchmark checksums stay **bit-identical** and the
per-test timings do not move outside noise:

| knob | effect on emitted code / throughput |
|---|---|
| `MaxInst` 1000 / 5000 / 20000 | none (1000 marginally worse) |
| `Multiblock` (already on) | — |
| `DynamicL1Cache`, `DisableL2Cache`, both heuristics | none measurable |
| `EnableCodeCachingWIP` | none (checksums identical) |
| TSO, x87 | no throughput change (x87 changes *accuracy*) |

So FEX's JIT has **no code-quality headroom left in its configuration surface**. What
follows is compile cost, and it is large. Measured on an import-heavy python start-up
(a real amount of x86-64 gets translated):

| configuration | time | vs no cache |
|---|---|---|
| no cache — recompiles every launch | 1.238 s | — |
| `FEX_ENABLECODECACHINGWIP=1` (in-memory, experimental) | 0.77 s | −38% |
| **`FEX_DISKCACHE=1`, cache warm** | **0.44 s** | **−64%** |
| both together | 0.64 s | −48% — *worse than disk cache alone* |
| trivial `python3 -c pass`: no cache → warm | 0.62 s → **0.19 s** | −70% |

**The two caching mechanisms do not stack.** Disk cache alone wins; the WIP code cache is
only worth having if the disk cache is unacceptable (0.77 s vs 1.24 s).

Two corrections this produced:

- **`DiskCache` is −64%, not the ~10% reported earlier in this document.** That number
  came from a cache that had not finished warming (still growing, 3.4 → 5.6 MB, across
  the three runs) and from comparing means rather than steady state. The trivial
  workload made it worse: `-c pass` barely compiles anything.
- **`MaxInst` is not a stutter lever here**, despite being `AffectsCodeGen`.

Correctness: the full nine-test suite with the experimental code caching enabled gives
**identical checksums** to baseline — only the `syscall` sum differs, and that is a sum of
process ids. Nothing was miscompiled.

Caveats for `DiskCache`: upstream documents unbounded growth and **no invalidation when
guest files change**, so clear it when a title is updated, or if something starts
misbehaving: `rm -rf ~/.cache/fex-emu`. It is per-user and off by default.

### Applied: `DiskCache: "1"` in `~/.fex-emu/Config.json`

Enabled 2026-09-22 with the user's approval, verified through the **config file** rather
than an environment override, with the cache cleared first so run 1 pays full price:

| launch | time | cache size |
|---|---|---|
| 1 (cold, rebuilds everything) | 1.239 s | 5.8 MB |
| 2 | 0.733 s | 9.7 MB |
| 3 | 0.519 s | 12 MB |
| **4 (steady state)** | **0.439 s** | 12 MB |

No `Unknown configuration option` warnings, and the pre-change file is backed up beside
it as `Config.json.bak-prediskcache-*`. Because `libwow64fex.dll` reads the same file,
this applies to Windows programs on the FEX backend as well. Clearing rule if a title is
updated or misbehaves: `rm -rf ~/.cache/fex-emu`.

Correction to an earlier claim in this report: the suggestion that a rebuild would gain
from `-mcpu` was **wrong**. The installed binary already carries `-mcpu=cortex-a76`
(`TUNE_CPU=native` auto-detects it via `Scripts/aarch64_fit_native.py`). The earlier
check looked in `CMakeCache.txt`, but compiler flags live in `flags.make` / `build.ninja`,
so it saw nothing and drew the wrong conclusion. **LTO is therefore the only remaining
build lever**, since this build sets `ENABLE_LTO=False` while the source's own default is
`TRUE`.

### It applies to the Windows path too

`/usr/lib/wine/aarch64-windows/libwow64fex.dll` — the FEX Windows backend — carries the
same `FEX_DISKCACHE`, `FEX_ENABLECODECACHINGWIP`, `FEX_MAXINST`, `FEX_ROOTFS` and
`FEX_TSOENABLED` strings and looks for `Config.json` under `/.fex-emu/`. So these settings
are not limited to the x86-64 Linux path.

### Build-time headroom — the largest remaining, untested

| | installed | source default | note |
|---|---|---|---|
| `ENABLE_LTO` | **False** | `TRUE` | this build is *less* optimized than upstream's default |
| `TUNE_CPU` | unset | — | FEX's own code targets generic ARMv8, not the A76 it runs on |
| `ENABLE_FEXCORE_PROFILER` | OFF | OFF | would show where compile time goes |

Because the dominant JIT cost is **the compiler's own speed**, the most direct remaining
lever is making that compiler faster: rebuild with `-DENABLE_LTO=ON` and
`-DTUNE_CPU=cortex-a76` (or `native`), from a **separate build directory**, and A/B it
against the installed binary before anything is installed anywhere. With a warm ccache
and ninja on 8 cores this is a real but bounded cost.

Also noted: box64 was configured with `-DRK3588=ON` — a Rockchip SoC flag — on an
Allwinner A733, and with no `-mcpu` either. It happens to set only `BAD_SIGNAL` (which
the cache shows as OFF), so it is probably inert, but it is a build-script bug worth
fixing before any box64 rebuild is trusted.

---

## 12. The LTO rebuild, and where the JIT time actually goes

Both built from source into side directories. **Nothing was installed**: the judgement
below is that it is not worth installing.

### LTO is real but marginal — about 5%

`-DENABLE_LTO=ON`, `-mcpu=cortex-a76` (already what the installed build uses), everything
else matched to the working build's flags.

| python import start-up, DiskCache forced off | installed | LTO | delta |
|---|---|---|---|
| rep 1 | 1.253 s | 1.182 s | −5.7% |
| rep 2 | 1.236 s | 1.164 s | −5.8% |
| rep 3 | 1.253 s | 1.206 s | −3.8% |

`-flto=thin` appears 410 times in the generated build graph, so it is genuinely applied.
Throughput and all nine checksums are **identical** — LTO optimises FEX's own code, not
the code its JIT emits, so this is a compile-speed win only.

**Recommendation: leave it uninstalled.** Five percent off an *un-cached* launch is small
next to DiskCache's −64%, which already removes most of that time, and replacing a
validated system binary with a local build for that margin is a bad trade. It stays at
`FEX-2609/build-lto/Bin/FEX` if that judgement ever changes.

### Profiler attribution: the JIT is IR-bound, not encoding-bound

`-DENABLE_FEXCORE_PROFILER=ON` writes plain-text events to ftrace's `trace_marker`
(`"<name> (lduration=-<ns>)"`), so **no GUI viewer is needed** — but the run must be root,
because a non-root user cannot even list `/sys/kernel/tracing`. An earlier attempt
silently captured nothing for exactly that reason.

Aggregated over one profiled run of the import-heavy start-up (1.43 s, 85,334 events):

| phase | calls | total | mean |
|---|---|---|---|
| **`CompileBlock`** | 30,791 | **1031 ms** | 33.5 µs |
| `GenerateIR` | 7,777 | 760 ms | 97.7 µs |
| `Run` (pass pipeline) | 7,777 | 276 ms | 35.4 µs |
| `RA` (register allocation) | 7,777 | 151 ms | 19.4 µs |
| `CompileCode` (instruction encoding) | 7,777 | 151 ms | 19.4 µs |
| `DFE` | 7,777 | 100 ms | 12.8 µs |
| `DecodeInstructions` | 7,777 | 13 ms | 1.7 µs |

Scopes nest, so these durations overlap; the ratios are the point, not the sums.

Two conclusions:

1. **Compilation dominates** — about 72% of the run is inside `CompileBlock`, which
   independently confirms the ~65% compile fraction inferred from the DiskCache delta
   (§11). Two different methods, same answer.
2. **The cost is IR-bound, not encoding-bound.** IR generation and the pass pipeline
   (register allocation, dead-flag elimination) are the bulk; the final ARM64 encoding
   step is a smaller slice. So the lever for FEX itself is fewer blocks to translate —
   which is what `DiskCache` does — not faster encoding.

The accumulation counters (`AccumulatedJITTime`, `AccumulatedDiskCacheHitCount`) do **not**
appear in the binary even with the profiler enabled, so disk-cache hit/miss counts are
not available from this backend; the strings check confirmed they are compiled out. The
`CompileBlock`/`GenerateIR` call-count ratio (≈4:1) is not explained by this study and is
recorded here rather than guessed at.

### Build notes worth keeping

- FEX requires clang; a fresh build directory picks `cc`→gcc and stops with
  "FEX doesn't support GCC". Name the compilers explicitly.
- `BUILD_FEXCONFIG` defaults to TRUE and needs Qt; the working build sets it OFF.
- `X86_DEV_ROOTFS` must point at an amd64 sysroot (`/home/radxa/crd-rootfs` here) or the
  thunk shims cannot link.
- `BUILD_THUNKS=OFF` was used for these builds: the guest thunk shims cross-compile for
  x86-64 **and i686**, and this box has no i686 sysroot — a pre-existing gap, not
  something introduced here (the original `build/Guest_32/` is empty too). Thunks only
  affect GL/Vulkan/audio redirection, not JIT compile speed or CPU throughput.
