# The full benchmark — what it covers, how to run it, and when numbers are comparable

One entry point:

```sh
bench/full.sh                 # the always-safe phases: canonical + d3d      (~25 min)
bench/full.sh open            # adds the open-driver suite (module swap)     (~2 min)
GS_DLL=<dll> bench/full.sh gs # adds the geometry-shader emulation A/B       (~6 min)
bench/full.sh all             # everything
bench/full.sh --list          # phase summary
```

Everything lands in `bench/logs-<timestamp>/`, with `SUMMARY.txt` at the end. A phase that
fails does not stop the others; the exit status is non-zero if any failed.

## Coverage

| what is being verified | phase | harness | evidence it produces |
|---|---|---|---|
| CPU single-thread, FEX atomics/threads/x87, GPU GLES throughput | `canonical` | `bench/run.sh` | `metric: value` rows directly diffable against `bench/baseline.txt` |
| D3D11 correctness: present, textures, BC1–5, RTT, depth, MRT, compute, indirect | `d3d` | `bench/d3d11/run.sh matrix` | per-app `rc` + a marker string, `known-open` classified |
| D3D11 cost: per-draw state, realistic frame split, present, instancing | `d3d` | `bench/d3d11/run.sh timed` | µs/draw, CPU-record vs GPU-finish, fps |
| Geometry-shader emulation (vendor blob has no GS) | `gs` | `bench/d3d11/run.sh gsab` | `gs.exe` gate, nogs vs gs µs/draw, BC-texture canary |
| Open driver: Vulkan 1.3 features, limits, render, GL/zink | `open` | `bench/pvr-vulkan/regress.sh` via `kernel/open-driver-spike/open-run.sh` | `N passed, 0 failed, 0 known-open` |
| Desktop GL (zink → PowerVR) through a real window | by hand / `bench/gles-x11.c` | `bench/gles-x11.c` | fps, Mpix/s, swap errors, renderer string |
| Board clocks and thermals | any phase | `bench/clkctl`, sysfs | GPU 1104 MHz, DSU 1027 MHz, cooling state 0 |

## What is **not** covered

* **No real application workload.** Everything is a harness or a demo; a game or a
  benchmark suite (3DMark, Unigine, a Proton title) is not part of this.
* **D3D9 and D3D10 are not exercised.** The matrix is D3D11 (`FL 11_0`), plus two D3D7
  probes that are expected to fail. `d3drun` routes 9/10 through the same DXVK build, but
  nothing here draws with them.
* **No soak/leak test.** `gpu-stress.sh` and `gpu-clock-bench.sh` exist for that and are
  not in `full.sh`; a long run is a separate decision.
* **No 32-bit (WoW64) row.** The harnesses are x86-64; the FEX-vs-box64 WoW64 choice is
  covered by `docs/FEX-BOX-TUNING-2026-09-22.md`, not here.
* **The geometry-shader phase needs a hand-built dll.** The GS work is not in this repo;
  `GS_DLL` must point at one built from a `gs-*` branch of DXVK-Sarek.
* **The D3D harness binaries are not in this repo** (they are PE binaries). `bench/d3d11/build.sh`
  rebuilds them from `bench/d3d11/src/` with mingw-w64.

## When numbers are comparable

**Same-session, back-to-back: yes — that is the only comparison treated as evidence.**
Cross-session drift on this board is large enough to invent or hide a change: the *same*
unmodified D3D build measured 64.35 ms/frame in June and 75.48 ms/frame a session later;
`drawbench a` reads 4.69 µs/draw in a quiet session and 7.14 µs/draw in `BENCHMARKS.md`.
Ratios taken inside one session are stable (PSO swap ≈ +54%, nogs repeats within 0.2%).

**Against `bench/baseline.txt`: yes, with two preconditions.**

1. **The clock overlays must be applied** — GPU 1104 MHz and DSU 1027 MHz. Check with
   `bench/clkctl` (`gpu_clk`, `dsu`); at the driver's default 600 MHz every `gpu.*` row is
   ~45% low and looks like a regression.
2. **The session GL environment must be stripped for the GPU rows.** Running inside a
   Plasma session inherits `MESA_LOADER_DRIVER_OVERRIDE=zink` (from the desktop-gl change),
   which makes Mesa's EGL try zink on GBM and blanks all four `gpu.glbench` rows with
   `eglInitialize 0x3001`. `bench/run.sh` does this for you since 2026-09-24.

**Against `docs/BENCHMARKS.md`: no, not directly.** Those tables are a different session;
use them for orders of magnitude and for method, and re-measure anything you intend to act
on.

**Vendor vs open driver: only within one session**, through `open-run.sh` (the module swap
is the variable). The open path renders ~2.4–2.5x slower and needs the module swap, so it
is a compatibility/parity target, not a replacement.

**Hashes are not evidence for the D3D dll.** The arm64ec link is not bit-reproducible —
the same source produced two different dlls in one afternoon — so compare the gate rows
(`gs.exe` → `GS_OK`, `cube.exe` → `CUBE_DONE`) rather than md5.

## Adding a harness

Drop a self-contained `*.cpp` in `bench/d3d11/src/`, add a row to `MATRIX` in
`bench/d3d11/run.sh` with the marker it prints and whether it is expected to pass, and
document what it proves in `bench/d3d11/README.md`. Anything that reports a rate rather
than a verdict belongs in the `timed` list instead.
