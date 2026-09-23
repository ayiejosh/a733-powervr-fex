# Three stacks, one board: stock vs the published repo vs the local work

**Date:** 2026-09-23 · **Board:** Radxa Cubie A7A (Allwinner A733), PowerVR BXM-4-64 MC1
**Question:** is the work rewarding - does the default now work where it did not, is it faster where it
lagged, and should anyone prefer the default, the published repo, or what is only in the local tree?

Machine-readable results: [`bench/pvr-vulkan/results-2026-09-23-*.txt`](../bench/pvr-vulkan/results-2026-09-23-vendor-1104mhz.txt).

## 1. The three columns, defined by what is actually in each tree

They are not "before/after" of one driver - they are three different *deliverables*, and the difference
matters more than any single number:

| column | what is in it | evidence |
|---|---|---|
| **A. Stock board, nothing from this repo** | GPU at the driver's own fallback clock, DSU at the bootloader's 780 MHz, desktop GL forced to software, vendor DDK 24.2 for Vulkan/GLES, nothing wired to the GPU | no `overlays/gpu-clk.dts` applied, `10-software-render.sh.bak-forced-software-20260922` |
| **B. Published repo** (`origin/trixie`) | the two device-tree overlays (GPU 1104 MHz, DSU 1027 MHz), the FEX/box64 tuning, and the zink-on-vendor-Vulkan GL recipe | `git cat-file -e origin/trixie:overlays/gpu-clk.dts` → present; `gpu/zink-trixie.md`, `docs/FEX-BOX-TUNING-2026-09-22.md` present |
| **C. Local tree** (`HEAD`, **+52 commits**, unpublished) | everything in B, plus the entire PowerVR open-driver effort: Mesa pvr fixes, the `powervr` kernel patches, the test suite, and the desktop GL un-forcing | `docs/GPU-RESEARCH-2026-09-22.md`, `kernel/open-driver-spike/` and `bench/pvr-vulkan/` are **absent** from `origin/trixie`; `git diff --stat origin/trixie..HEAD` = 97 files, +19464 lines |

So the honest headline before any measurement: **the PowerVR/open-driver work is not published.** A
reader who clones the repo today gets the clocks, the emulation tuning and a documented zink recipe -
all of which run on the vendor DDK - and none of the driver work in `docs/GPU-RESEARCH-2026-09-22.md`.

## 2. Method, and what would have made these numbers lies

- **One harness per metric.** GL is `bench/glbench.c` (1280×720, ALU-loop fragment shader) everywhere,
  and always with the **surfaceless** EGL config, because it is the only config that exists on all four
  GL paths: with GBM+pbuffer, zink returns `EGL_BAD_CONFIG` ("no context 0x3005") and produces no
  result at all. The repo's recorded GBM numbers (`7392/2229/579/147`) are 1.0-3.8× the surfaceless
  ones for the *same driver at the same clock*, so the two configs are not interchangeable.
- **Clocks, not guesses.** The stock column was measured by setting the GPU and DSU clocks live through
  `bench/clkctl` (`stocklane.sh`) and reading the achieved rate back, then restoring the repo's
  settings - no reboots, and no quoting of history. Requesting 600 MHz lands on **552 MHz**; the
  generator steps in ~552 MHz units.
- **Both open ICDs in one boot** (`stacklane.sh`), so module, clocks and machine state are constants.
- **Compatibility is reported as PASS/FAIL, never skipped** - "it crashed before" is a result.

## 3. Result 1: the clock overlay is the single biggest win in the repo

Same vendor driver, same harness, only the two clocks moved:

| metric | stock (552 MHz / 780 MHz) | repo (1104 MHz / 1027 MHz) | ratio |
|---|---|---|---|
| GLES glbench loop4 | 3760 Mpix/s | **7231** | **1.92×** |
| GLES glbench loop16 | 745 | **1468** | 1.97× |
| GLES glbench loop64 | 192 | **383** | 1.99× |
| GLES glbench loop256 | 20 | **39** | 1.95× |
| Vulkan compute, ms/dispatch | 1.336 | **0.773** | 1.73× |
| Vulkan render 512, ms/frame | 0.837 | **0.704** | 1.19× |
| Vulkan render 1024 | 2.077 | **1.818** | 1.14× |
| Vulkan render 2048 | 7.185 | **6.300** | 1.14× |
| Vulkan readback 512² | 3.304 ms | 3.119 ms | 1.06× |

GL scales with the clock almost exactly 2×; **Vulkan drawing barely scales at all** (1.14-1.19×). That
is §19 restated as a measurement across configurations: the open driver's draw cost is dominated by
per-tile driver work, and the vendor's is too - neither is clock-bound, which is why the overlay does
not fix the draw gap and why the draw gap is the thing to attack next.

## 4. Result 2: the four GL paths are not close

Mpix/s at 1280×720, same binary, same config, 1104 MHz unless marked:

| GL path | loop4 | loop16 | loop64 | loop256 | renderer |
|---|---|---|---|---|---|
| **Stock fallback** (desktop default) | **1** | - | - | - | softpipe |
| **Vendor native** (what a stock EGL app gets) | **7231** | **1468** | **383** | **39** | PowerVR B-Series BXM-4-64 |
| **Repo**: zink → vendor Vulkan (`glrun`) | 5964 | 1338 | 328 | 34 | zink Vulkan 1.3 (IMAGINATION_PROPRIETARY) |
| **Local**: zink → **open** driver | 101 | 41 | 6 | 2 | zink Vulkan 1.2 (IMAGINATION_OPEN_SOURCE_MESA) |
| *vendor native, at the stock 552 MHz clock* | *3760* | *745* | *192* | *20* | *same* |

Three things worth saying out loud:

1. **A stock board is not GL-less off-screen.** `/etc/ld.so.conf.d/00_xserver-xorg-img-bxm.conf` puts
   `/usr/local/lib` on the default path, so a plain EGL app gets the vendor's GLES 3.2 on the GPU with
   no environment at all. `bench/GPU_BENCHMARK.md`'s "GLES requires `LD_LIBRARY_PATH=/usr/local/lib`"
   is no longer true of this install.
2. **What the repo's GL work fixes is the desktop and GLX path, not raw GLES.** The desktop was
   *forced* to software (`LIBGL_ALWAYS_SOFTWARE=1`); the zink wiring replaces that, and the published
   doc's recipe (`glrun`) reaches 5964 Mpix/s - **82-91 % of the vendor's own GLES** at every load,
   which is a good result for a translation layer.
3. **The open driver's zink path is 59× slower than the vendor-backed one** at loop4 (101 against
   5964). Our driver work made that path *work*; it did not make it fast, and nobody should choose it
   for GL today. It is still ~100× faster than the software fallback it replaces.

## 5. Result 3: on Vulkan the vendor driver is still ~2.3× faster, and our fixes did not change that

1104 MHz, same boot, both open ICDs (`build/` = before the BDA and 64-bit work, `build-x11/` = now):

| metric | vendor 1.3.277 | open, before | open, now |
|---|---|---|---|
| compute ms/dispatch | **0.773** | 0.814 | 0.875 |
| compute GB/s | **2.71** | 2.58 | 2.40 |
| render 512 ms/frame | **0.704** | 1.99 | 2.05 |
| render 1024 | **1.818** | 4.53 | 4.26 |
| render 2048 | **6.300** | 13.87 | 14.23 |
| render 4096 / 8192 | PASS / PASS | PASS / PASS | PASS / PASS |
| readback 512² total | **3.12-3.63 ms** write-combined, **0.46 ms** via its cached type | 3.45 ms | 3.38 ms (no default cached type) |
| allocate+bind, 1 MiB host-visible | **0.075 ms** | 0.862 ms | 0.844 ms |

Two honest readings:

- **Against the vendor, the open driver loses on draw by 2.3-2.9×** and on allocation by 11×, and is
  only level on readback. That is the price of the open stack today, and it is a per-tile cost (§19).
- **Our recent Mesa work did not change any of these numbers.** Render 512 with 60 frames twice reads
  1.987/1.986 ms before and 1.916/2.175 ms now - the "13 % faster" a 20-frame run suggested was noise.
  The value of that work is in §6, not here.
- **One row got *worse* by design.** The pre-change driver exposed a second host-visible type whose
  readback was 0.711 ms against 3.446 ms - and that type corrupted GL through zink because it claimed
  `HOST_COHERENT` while never being synchronised (§21.10-§21.12). It is now opt-in and fixed in the
  kernel, so the default is the slower honest type. The vendor, for comparison, gets 0.457 ms from its
  cached type *and* is correct: closing that gap needs the per-submit flush/invalidate UAPI.

## 6. Result 4: compatibility is where the local work actually paid

| case | vendor | open before | open now |
|---|---|---|---|
| `bufferDeviceAddress` advertised | yes | **no** | **yes** (9/9 on descriptors *and* push constants) |
| 64-bit push constants | PASS | **SIGSEGV** (`Unsupported alu instruction: unpack_64_2x32_split_x`, after 1000 iterations of `WARNING! Infinite opt loop!`) | **PASS** (`pctest` 6/6) |
| 2× MSAA advertised | `0x7` (1\|2\|4) | `0x5` (1\|4) | `0x7` |
| X11 WSI (`xcb`/`xlib`) | yes | **no** (wayland/display only) | **yes** |
| host-visible memory types | 2 (incl. a *working* cached type, 0.457 ms readback) | 2, one **lying about `HOST_COHERENT`** | 1 honest type (the cached one is opt-in, and still imperfect) |
| regression suite | - | bda/pctest cannot run | **20 passed, 0 failed, 0 known-open** |
| API version | 1.3.277 | 1.2.363 | 1.2.363 |

The `pctest` line is the clearest answer to "did it not work before?": on the pre-change driver the
64-bit push-constant test does not fail, it **crashes the process**, in the same way it would have
crashed any application that pushed a `uint64_t`.

## 7. Result 5: presentation moved a little, not a lot

`pvranimate`, dma-buf scanout with page flips, both open ICDs, zero flip timeouts in every run:

| resolution | open before | open now | repo's recorded number |
|---|---|---|---|
| 1920×1080 | 53.2 fps | 55.6 fps | 59.6 fps |
| 3840×2160 | 41.8 fps | 42.2 fps | 40.0 fps (27.8 fps before the extent work) |

The large 4K win in the repo (27.8 → 40+ fps) comes from the **extent-limit fix, which is in both
ICDs** - it landed before `build/` was linked, so this A/B cannot show it; today's 42 fps is that work
still holding. The remaining 1080p difference from the recorded 59.6 fps is within run-to-run and
panel-refresh variation.

## 8. Verdict: which column to choose

**For performance and compatibility today, the stock driver plus the repo's overlays and recipes (B)
is the better choice, and the local driver work (C) is not yet competitive on speed.**
Concretely:

- **If the goal is a fast, working GPU**: apply the two overlays (≈2× on GL, 1.7× on compute, free),
  keep the vendor DDK, and use `glrun` for GL apps and `d3drun` for D3D. Nothing in the open-driver
  work beats that combination yet.
- **If the goal is GL specifically**: the vendor's native GLES is fastest (7231), the repo's zink path
  is within 10-18 % and is the answer where the native blob is broken; the open driver's zink is 59×
  slower and only wins against softpipe.
- **If the goal is a driver that can be fixed**: only C exists. Before it, the open stack refused
  `bufferDeviceAddress`, crashed on 64-bit push constants, could not render 8192, had no X11 surface
  path and offered a memory type that corrupted GL. After it, the open stack passes everything the
  vendor passes on this suite. That is real progress - it is just not *faster* progress, and the
  headline numbers say so.

### What would change this verdict

1. **The per-tile fix** (§19): the open driver's draw is 5.5× the vendor's per tile. Closing that is
   the only thing that makes C a performance choice rather than a research choice.
2. **Publishing it**: 52 commits and 19 464 lines are invisible to anyone who clones the repo. Until
   they are pushed, B *is* the repo.
3. **The desktop**: the un-forcing of software GL is local-only too, so even the desktop improvement
   in B is only as good as its documentation until `system/desktop-gl-zink/` is published.

## 9. What these numbers are not

- **Not application fps.** glbench and vkrender are clean GPU metrics; a real application adds
  uploads, state changes and presentation. The DXVK/vkd3d path has not been re-run on the open driver
  at all, so "does a game work on C" is still unmeasured, not answered.
- **Not a like-for-like of the vendor and open drivers' feature sets.** The vendor still advertises 21
  device features and 30 device extensions that the open driver does not (8/16-bit storage,
  `variablePointers`, `drawIndirectCount`, `vulkanMemoryModel`, `depthClamp`, the three 1.3 core
  features, timestamps). §21.13 has the full list; compatibility is closer than it was, not equal.
- **Not clock-independent.** All columns except the two marked 552 MHz ran at 1104 MHz; a stock board
  will be ~2× slower on GL than every number in §4 except the marked row.
