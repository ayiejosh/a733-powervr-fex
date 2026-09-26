# Full benchmark + stability/deployability check — 2026-09-24

One session (03:08–03:58 local), one board, clocks held at the documented operating point:
**GPU 1104 MHz, DSU 1027 MHz, governor `schedutil`, `cpu-boost` active, GPU 61 °C, no thermal
cooling state engaged**. Every number below was produced in this session; nothing is copied
from an earlier run except where the column says "baseline".

Question being answered: **are the gains stable, and can they be used?**

## Phases and raw evidence

| phase | what | log |
|---|---|---|
| 1 | shipping D3D11 stack (vendor ICD): 27-app matrix + timed suite | `phase1-console.log`, `p1-*.log` |
| 2b | session-wide layer/fake controls (GS, MRT, MSAA, tess) | `phase2b-control-031327.log` |
| 2c | GS A/B (after build ×3, nogs ×2, cube control) | `phase2c-gs-ab-20260924-032328.log` |
| 2d | GS before/after same-session + baseline-gap discovery | `phase2d-before-after-20260924-033650.log` |
| 4 | canonical `bench/run.sh` (CPU/FEX/GLES) + glbench rows | `p4-bench-run.txt` |
| — | windowed GL: zink vs llvmpipe + renderer identity | inline below |
| 3 | open driver (module swap): full `regress.sh` + repeats | `/home/radxa/kspike/open-run-20260924-035750.log` |

---

## 1. Shipping D3D11 stack — 21/27 apps clean

`d3d11.dll = 4c4bd57926b7c0fce96783202e0e7f68` (deployed BCn build), vendor ICD, GPU 1104 MHz.

Clean (rc=0 + their own OK marker): `tri` (6000 frames), `cube` (CUBE_DONE, 50.5 s), `tex`,
`mrt2`, `depth`, `drawbench`, `rtt`, `compute`, `bench`, `bench2`, `bench_flip`, `bctex`,
`bc2t`, `bc4t`, `bc5t`, `bcbench`, `bcdxvk`, `bcdxvk3`, `cgs`, `cgs2`, `min_d3d11`.

Failures — **all pre-existing, and all reproduce identically with the session layer removed**
(phase 2b measured each with `PVR_FAKE_*`/`VK_INSTANCE_LAYERS` on and off):

| app | result | why |
|---|---|---|
| `mrt.exe` | MRT_FAIL rc=3 | the `SV_VertexID` + no-input-layout variant; `mrt2.exe` (real VB, same shaders) is MRT_OK |
| `msaa.exe`, `msaa2.exe` | MSAA_FAIL rc=4 | BXM blob has no MSAA |
| `tess.exe` | rc=3 | BXM blob has no tessellation |
| `d7test.exe`, `d3d7test.exe` | rc=1 | no D3DHALDevice (no D3D7) |

Timed suite (same session):

| metric | today | BENCHMARKS.md baseline |
|---|---|---|
| drawbench a (baseline) | **4.690 µs/draw** (213k draws/s) | 7.14 |
| drawbench a repeat | **4.697** (+0.15%) | — |
| drawbench b (PSO swap) | **7.210** (+2.52 µs, +54%) | 10.09 (+2.96) |
| drawbench c/d/e (cbuf/SRV/vbuf) | 5.168 / 5.325 / 5.160 | 7.50 / 7.66 / 7.52 |
| drawbench f/g (drawidx/drawinst) | 4.823 / 5.033 | ~7.1 |
| realbench 500/1000/2000/4000 draws | cpuRec 0.26/0.54/1.06/2.09 ms · gpuFin 2.35/4.89/7.81/14.72 ms — all GPU-bound | cpu 0.53/0.89/1.78/4.63 · gpu 3.17/6.03/10.96/19.45 |
| present (800×600, 300 frames) | 2.986 ms/present, 330 fps | ~1.87 ms |
| bench.exe 256 draws · bench2 instanced | 1819 draws/s · 190,641 tris/s (1000 inst) | ~1000–1100 · ~370k |
| bcbench BC1 2048² | 46.53 ms/tex, 343.9 MB/s decoded | — |

The PSO swap is still the only state op that moves the needle (+54%), and a realistic frame is
still GPU-fill-bound with CPU-record ≤13% of it. Kernel log clean.

## 2. The GS gain, same-session before/after

Both builds made in this session from the committed source, A/B back-to-back:

| run | dll | ms/frame | µs/draw |
|---|---|---:|---:|
| nogs, gs-compute dll | 243f88bd (3dd76cfd) | 0.7519 | 11.748 |
| **before** gs ×2 | 243f88bd | 64.3290 / 64.7371 | **1005.140 / 1011.518** |
| **after** gs ×4 | 24b206a3 (edc08235) | 43.8710 / 43.5679 / 43.6623 / 43.6998 | **685.484 / 680.748 / 682.224 / 682.810** |

**−32.4% per draw (mean 1008.3 → 682.8 µs), spread ≤0.8% across four after-runs.** Correctness
gate `gs.exe` → `center RGB=0,255,0` / `GS_OK geometry shader ran (green)` / rc=0 on both builds.

nogs on four different dlls in the same session: 0.7677 (shipping deployed), 0.8281 (amortised),
0.7519 (gs-compute), 0.7024 (shipping rebuilt today) — ±9%, so the *ratio* only spans 53–97×;
the per-draw absolutes are the stable evidence.

**Method note:** the first nogs run after a dll swap is ~20% slow (cold shader cache). An
interleaved repeat settles it — amortised 0.8896 then **0.7230** ms/frame, shipping **0.7181**
then **0.7136**: the folded build does not regress the non-GS path (≤1.3% once warm). The
0.8281/0.8896 figures are cold-start artifacts, not a slowdown.

## 3. The blocker found: the GS branch is 4830 commits behind the shipping branch

`cube.exe` — no geometry shader, plain conf — **page-faults** on the GS dlls:

```
err:   D3D11: Cannot create texture: Format: 71 (BC1_UNORM) ... Usage: 8
wine: Unhandled page fault on read access to 0000000000000000
```

It is a baseline gap, not the folds:

```
merge-base(bcn-update-20260921, gs-amortise) = 27c050b5 (main)
bcn-update-20260921 is 4830 commits ahead of that base
```

So the whole GS branch (gs-compute + the three folds) is built on June `main` and contains none
of the shipping branch's work — no BC1-5 decode patch, no WSI rework, none of the 1.14 line.
`cube.exe` (a BC1 texture) fails on **both** gs-compute and gs-amortise for that reason, and
succeeds in 51 s on the deployed dll *and* on a from-source build of the shipping branch made
today (`abd63519`). `tri.exe` (no BC texture) is unaffected: 101.5 s shipping vs 101.6 s
amortised.

## 4. Open driver — full suite green

`open-run.sh` (module swap, desktop restored afterwards), ICD `/home/radxa/kspike/open-icd.json`:

```
regress: 29 passed, 0 failed, 0 known-open
```

including `vk13`, `vk16`, `vkbits`, IO16, both depthClamp cases, vertex-stage SSBO store+atomic,
`vkdescidx`, `bda`, `pctest`, 4096/6144/8192 render targets, and GL/zink over the *open* ICD.
Stability repeats (not part of the suite):

| repeat | result |
|---|---|
| zink GL 512×20 ×3 | PASS ×3 — 7.225 / 7.466 / 8.138 ms/frame, 262144/262144 px |
| zink GL cached memory type | PASS — 262144/262144 px |
| vkrender 512 ×3 | PASS ×3 — 1.942 / 2.147 / 2.155 ms/frame |

Kernel during the run: 5× `powervr 1800000.gpu: [drm] Received unknown FWCCB command 2abc006b`
(a firmware-CCB notice from the ring, no fault, no wedge — worth watching, not a failure).

## 5. Session-wide GL (zink) and the canonical bench

Live check with the session's own environment — `GL_RENDERER: zink Vulkan 1.3(PowerVR B-Series
BXM-4-64 MC1 (IMAGINATION_PROPRIETARY))`, **0 swap errors** in every run:

| workload (windowed, same tool) | zink → PowerVR Vulkan | llvmpipe | today's ratio |
|---|---|---|---|
| 320×240 loop16 | 232.6 fps (17.9 Mpix/s) | 105.2 fps (8.1 Mpix/s) | 2.2× |
| 800×600 loop64 | 29.9 fps (14.4 Mpix/s) | 8.6 fps (4.1 Mpix/s) | 3.5× |

`plasmashell` has `libVK_IMG` mapped and the session has been up 13.6 h.

Canonical `bench/run.sh` vs `bench/baseline.txt` — CPU/FEX side reproduces:

| metric | today | baseline |
|---|---|---|
| cpu.1thread.evps | 847.83 | 847.80 |
| fex.atomic.off0 / off2 / off14 | 153.23 / 60.80 / 56.80 | 152.14 / 61.40 / 56.85 |
| fex.tcreate.nsop | 157167 | 159850 |
| fex.x87.RP0 / RP1 → ratio | 76.8 / 4.1 → **18.7×** | 77.3 / 4.1 → 18.9× |
| gpu.glbench loop4/16/64/256 Mpix | 7589 / 2225 / 579 / 147 | 7392 / 2229 / 579 / 147 |

**Tooling gotcha found (fixed, see §8.2):** in the canonical run the four `gpu.glbench` rows read
**NA**. Cause is not the GPU: `bench/run.sh` inherits the Plasma session environment, and
`MESA_LOADER_DRIVER_OVERRIDE=zink` makes Mesa's EGL try zink on the GBM platform, so the vendor
GLES path fails `eglInitialize (0x3001)`. With the session GL variables unset the same binary
gives the four rows above, matching baseline within 3%.

## 6. Verdict per gain

| # | gain | measured | stable? | usable? |
|---|---|---|---|---|
| 1 | GPU clock 600→1104 MHz (overlay) | glbench 7589/2225/579/147 Mpix, +2.7%/−0.2%/0/0 vs baseline | yes — matches baseline in-session | **yes**, applied at boot |
| 2 | DSU/L3 780→1027 MHz (overlay) | `clkctl dsu` = 1027000000 | yes | **yes**, applied at boot |
| 3 | FEX tuning (TSO off, Multiblock) + box64 CALLRET | cpu 847.83 evps, atomics 153/61/57 Mops, x87 18.7× | yes — within 1% of baseline | **yes**, default binfmt |
| 4 | Session-wide GL: zink → PowerVR Vulkan | 2.2–3.5× over llvmpipe, 0 swap errors, shell on GPU 13.6 h | yes | **yes**; known cost: `PVR_FAKE_*` is session-wide (narrow it to `glrun` if a Vulkan app is confused by a faked GS) |
| 5 | DXVK BCn decode patch (BC1–5) | bctex/bc2t/bc4t/bc5t/bcdxvk/bcdxvk3 all OK; BC1 2048² 46.53 ms | yes | **yes**, deployed dll |
| 6 | D3D11 FL 11_0 stack | 21/27 apps clean; 4.690 µs/draw; GPU-fill-bound frames | yes — 0.15% repeat spread | **yes**; not usable for MSAA/tessellation/D3D7 (blob) |
| 7 | Open driver (feature parity) | regress 29/29, GL over open ICD ×3 PASS | yes | **no** — needs a module swap and is 2.4× slower rendering; research/fallback path |
| 8 | GS compute emulation (−32% per draw) | 1008 → 683 µs/draw same-session | measurement is stable (≤0.8%) **but** the dll is not shippable | **no** — opt-in only; see blockers below |
| 9 | multiViewport | 4 viewports → SIGSEGV, 2 → nothing | n/a | **no** — reverted, hardware wall |
| 10 | Non-uniform descriptor indexing | probe PASS 4/4 on hardware | yes | not needed by any app here; feasibility only |

### What GS needs before it can be used (from this session + review)

1. **Rebase onto the shipping branch** (4830 commits). Today the dll regresses every BC-textured
   app; the fold's numbers were measured on the June baseline.
2. **Shape validation / fallback.** The path assumes stride 8, a 3-slot (48 B) record, 3-in/3-out
   triangle lists, and ignores indexed draws and the app VS. Only `Draw`/`DrawIndexed` intercept;
   `DrawInstanced`, `DrawIndexedInstanced`, both indirect variants and `DrawAuto` fall through
   with a compute-stage module bound to the graphics stage.
3. **Restore an explicit barrier between the compute dispatch and the indirect draw.** The second
   fold removed the `copyBuffer(counter → args)` that used to force `m_execBarriers.recordCommands`;
   correctness now relies on `startRenderPass` flushing incidentally.
4. Then the only remaining big lever: batch the per-draw pass chain across a frame's draws
   (per-draw record bases + per-draw indirect args) — 64 serialised chains → 1.

## 7. Final state after the run

`d3d11.dll = 4c4bd57926b7c0fce96783202e0e7f68` (shipping BCn), DXVK-Sarek back on
`bcn-update-20260921` with 0 tracked changes, `pvrsrvkm` loaded, `X=1 kwin=1 plasma=1 picom=1`,
GPU 1104 MHz / DSU 1027 MHz, GUI answering. Nothing pushed.

---

## 8. Addendum — both findings fixed the same day

### 8.1 The GS dll is no longer on an old base

Why it was stale: the GS work was written **2026-06-21** against `main` as of 2026-06-16
(`27c050b5`); the shipping branch `bcn-update-20260921` was assembled afterwards from upstream
1.14 + the 2026-07-02 BC1-5 patch and reached **4830 commits** past that common base. The GS
branch was never rebased — it stayed a research branch whose dll was swapped in by hand for
benchmarks, which is exactly why nobody noticed that a BC-textured app could not run on it.

| branch | tip | contents |
|---|---|---|
| `gs-latest` | `80756906` | shipping base + all 9 GS commits (M1–M3 + the 3 folds) |
| `gs-latest-nofold` | `8652b433` | shipping base + M1–M3 only (the fold reference point) |

Both are merges of the GS line onto `bcn-update-20260921`; one conflict each, the same one
(`d3d11_options.h`, upstream and GS both appended option fields — both kept).
`dxbc_compiler.cpp` auto-merged although 455 GS lines met 235 lines of upstream drift, and
upstream drift touches **no GS-added line**: the two ported branches differ by exactly the folds
(3 files, +107/−194 — the same shape as on the old base). **Builds clean with no source change.**

Verification, vendor ICD, one session:

| check | result |
|---|---|
| `gs.exe` gate on the ported build | `GS_OK`, green, rc=0 |
| `cube.exe` (the app that page-faulted) | **`CUBE_DONE`, rc=0** |
| BC battery | `bctex`/`bc2t`/`bc4t`/`bc5t`/`bcdxvk`/`bcdxvk3` all OK |
| other non-GS apps | `tri`, `tex`, `compute`, `rtt`, `mrt2`, `depth`, `cgs`, `cgs2`, `min_d3d11` — 15/15 |
| nogs on the ported dll | 0.7093 / 0.7148 ms/frame vs shipping 0.7169 — parity |

And the folds keep their value on the shipping base (same session, same base):

| run | µs/draw |
|---|---:|
| no folds (`gs-latest-nofold`) | 1007.479 / 1004.254 |
| folded (`gs-latest`) | 674.817 / 677.519 / 680.514 |
| nogs baseline | 11.083 / 11.168 / 11.201 / 12.391 |

**−32.6% per draw** (1005.9 → 677.6) — the same as on June main. Penalty ≈ **60×** the no-GS
baseline. Still not default-on: the path's shape assumptions, the un-intercepted draw verbs, the
implicit post-dispatch barrier and the missing fallback all stand (see §6, item 1 is now done).

### 8.2 `bench/run.sh` GPU rows

`bench/run.sh` now strips the session GL variables for the glbench section and prints a note if a
row still comes back empty. Re-run after the fix: **7616 / 2224 / 579 / 147** Mpix vs the
7392 / 2229 / 579 / 147 baseline — the four rows are back and the tool no longer reports a
phantom GPU regression.

---

## 9. The benchmark is now a single command

Everything above was assembled by hand, phase by phase. It is now packaged:

| file | what it adds |
|---|---|
| `bench/full.sh` | one entry point, four selectable phases (`canonical`, `d3d`, `gs`, `open`), per-phase logs + `SUMMARY.txt`, non-zero exit if any phase failed |
| `bench/d3d11/` | the 30-program D3D11 harness in-tree: `matrix` (with expectations and the five known-open rows classified), `timed` (the suite behind `BENCHMARKS.md`), `gsab` (dll-swapping A/B), `build.sh` for mingw-w64 |
| `docs/FULL-BENCHMARK.md` | what is covered, what is **not** (no real apps, no D3D9/10, no soak, no WoW64), and when two numbers may be compared |
| `.gitignore` | `bench/logs-*/`, `bench/d3d11/logs-*/` |

### The end-to-end run — one command, four phases, all green

`GS_DLL=<gs-latest dll> bench/full.sh all`, logs in `bench/logs-20260924-094539/`, `failed: none`:

| phase | result |
|---|---|
| canonical | cpu.1thread **847.30**, glbench **7589/2227/579/147** (baseline 7392/2229/579/147), FEX atomics 153.9/61.9/56.5 Mops, x87 ratio **19.3×** |
| d3d | matrix **21 clean, 6 known-open, 0 unexpected**; drawbench a **4.686 µs/draw** (repeat 4.440), PSO swap 7.665, present 3.19 ms/frame, bench 184 fps, instancing 185k tris/s |
| open | **29 passed, 0 failed, 0 known-open**, desktop restored |
| gs | `gs.exe` **GS_OK**, gs **688.3 / 687.1 µs/draw**, BC-texture canary `CUBE_DONE`, deployed dll restored |

### Three bugs the first end-to-end runs found (all fixed)

1. `run.sh matrix` crashed with `QUICK: unbound variable` whenever `QUICK` was not set — the
   quick path had only ever been tested with it set.
2. Every phase is piped through `tee`, and without `set -o pipefail` a *failing* phase was
   reported as **ok**: the first full run announced "phase d3d: ok" over a matrix that had died
   on the line above it.
3. The `open` phase must strip the session zink/feature-strip variables before handing the board
   to `regress.sh`, or the strip layer fakes and removes features on the open driver too and the
   `depthClamp`/`vk13` cases stop testing what their names say.

One caveat for reading the `gs` phase: it runs right after `open`, while the desktop is coming
back, so its *nogs* baseline is inflated (20.7 µs/draw against 11.2 quiet). The `gs` absolutes
are unaffected (688 vs 677 µs/draw) — run the phase on a quiet board when the ratio is the point.
