# Prior-work audit — recorded numbers vs the machine on 2026-10-09

**Scope:** every measurement/claim in `backups/a733-backup/knowledge/` (29 docs), `ve2/ve2-vaapi/GPU_BENCHMARK.md`,
the `Main/` + `upgrade-20260922/` docs, `Recovery/GPU-FIRMWARE-RE` (190+ entries), and the pvr-vulkan bench dir.
**Method:** re-run what is safe to re-run (GPU via the harness + the original glbench/cpubench), recompute
everything else from the recorded logs. **Nothing was pushed. The driver binding was not touched (kwin_x11
live, vendor `pvrsrvkm` bound throughout). No VE/VPU or NPU workload was run.**

Re-run commands used here:

```sh
# harness (mandatory), vendor arm already bound -> no switch, guard not triggered
cd /mnt/sdcard/_REVIEW/emulation/trixie-prep/bench/pvr-vulkan
python3 harness.py vkheavy 2048 5 --driver=vendor      # 180.398 ms/frame
python3 harness.py cstp 64 5      --driver=vendor
python3 harness.py vkrender 2048 5 --driver=vendor
python3 logcheck.py                 # 198 records, 188 trustworthy (94.9%)
python3 logcheck.py --clean         # A/B source used for the ratio tables below

# original method behind GPU_BENCHMARK.md §TL;DR
gcc -O3 -ffast-math -ftree-vectorize -fopenmp -o /tmp/cpubench cpubench.c -lm
gcc -O2 -o /tmp/glbench glbench.c -lEGL -lGLESv2 -lgbm
for L in 4 16 64 256; do /tmp/cpubench $L 20; done
for L in 4 16 64 256; do LD_LIBRARY_PATH=/usr/local/lib /tmp/glbench /dev/dri/renderD128 $L 300; done
```

---

## (a) Verification table

Direction convention for the A/B rows: **the open Mesa `powervr` driver is the one being improved; the vendor
`pvrsrvkm`/`libVK_IMG` stack is the target.** "gap" = vendor ÷ open.

### a1. GPU_BENCHMARK.md (2026-06-05) — GPU vs CPU (1280×720 offscreen FBO, loop = fragment ALU iterations)

| claim | recorded (2026-06-05, 600 MHz) | current (2026-10-09, 1104 MHz) | verdict |
|---|---|---|---|
| GLES loop=4 | 4198 Mpix/s (4555 fps) | **7355 Mpix/s (7981 fps)** | **DRIFTED** +75 % |
| GLES loop=16 | 1216 Mpix/s | **2211 Mpix/s** | **DRIFTED** +82 % |
| GLES loop=64 | 315 Mpix/s | **574 Mpix/s** | **DRIFTED** +82 % |
| GLES loop=256 | 80 Mpix/s | **147 Mpix/s** | **DRIFTED** +84 % |
| CPU 8-core NEON loop=4 | 28 Mpix/s | **31 Mpix/s** | HOLDS |
| CPU loop=16 | 7 Mpix/s | **7.3 Mpix/s** (7.9 fps) | HOLDS |
| CPU loop=64 | 2 Mpix/s | **1.6 Mpix/s** (1.7 fps) | HOLDS |
| CPU loop=256 | 0.46 Mpix/s | **0.37 Mpix/s** (0.4 fps) | HOLDS (within noise) |
| **GPU advantage ~150–175×** | 150/174/158/174× | **237 / 280 / 338 / 397×** | **DRIFTED — understated today** |
| fill-rate ceiling ≈4.2 Gpix/s | 4.2 | **7.35 Gpix/s** | **DRIFTED** |
| "clean linear ALU scaling" | 4198→1216→315→80 | 7355→2211→574→147 (4.0× per 4× work, both ends) | HOLDS |
| ~600× vs softpipe (0.5 Mpix/s @64) | 0.5 softpipe | not re-measured | UNVERIFIABLE |

**Why the GPU numbers moved: the clock, nothing else.** The GPU ratios 7355/4198=1.75, 2211/1216=1.82,
574/315=1.82, 147/80=1.84 track 1104/600=1.84. The CPU baseline is unchanged, so the *advantage* grew even
though the 150–175× figure was correct when written. Same binary, same shader template, same 1280×720.

### a2. GPU clock

| claim | recorded | current | verdict |
|---|---|---|---|
| GPU clock | **600 MHz fixed** ("DTS sets no DVFS table") | **1104 MHz** | **DRIFTED** |
| mechanism | driver fallback to hard-coded default | DT overlay pins `clk_rate=1104000000`, rail floor 990 mV | changed |
| "no DVFS" | no DVFS | **still no DVFS** — `pvrsrvkm` imports no `dev_pm_opp_*`; `clk_set_rate` above 1104 clamps to 1104 | HOLDS |
| 1008 MHz doc (Main/GPU-CLOCK-1008MHZ) | 600→1008, +25–27 % | **superseded: 1104 is the generator ceiling** | DRIFTED (1008 was an intermediate step) |

Live evidence: `/sys/kernel/debug/clk/clk_summary` → `pll-gpu 1104000000`, `gpu0 1104000000`;
`/boot/dtbo/gpu-clk.dtbo` (md5 `46f8ef2a…`) from `upgrade-20260922/gpu-clk-1104mhz.dts`;
`axp8191-dcdc4 = 990000 µV`; DT still declares `opp@400/600/800/1104000000` and nothing reads it.
The 1008 doc's own follow-up (`Main/PERFORMANCE-RESEARCH-2026-09-22.md` §2) already measured the ceiling;
the *machine* matches the follow-up, not the 1008 doc. **Real DVFS table: no. Fixed override: yes.**

### a3. Current session's cross-driver claims (recomputed from `logcheck.py --clean`)

| claim | recorded | recomputed from clean records (median, n) | verdict |
|---|---|---|---|
| render gap open-vs-vendor **1.68–2.46×** (256–4096) | 1.68–2.46× | **1.63×** (256: 107.1 vs 65.6 Mpix/s, n=5/1) · **2.21×** (512: 377.9 vs 171.3, n=33/8) · **2.27×** (1024: 598.1 vs 263.3, n=5/3) · **2.40×** (2048: 735.8 vs 306.4, n=38/9) · **2.28×** (4096: 708.6 vs 311.3, n=5/3) | **HOLDS** |
| `vkheavy` **1.42×** | 1.42× | **1.42×** (255.31 vs 180.11 ms/frame; 23.3 vs 16.4 Mpix/s) | **HOLDS — exact** |
| `cstp` (no loop) **parity 1.06×** | 1.06× | **parity**: vendor 356.6 vs open 323.0 M inv/s = vendor 1.10× / open 0.91× | HOLDS (parity, within a wide spread) |
| loops **1.63–2.15×** | 1.63–2.15× | `cstpf` 1.64× · `cstpi` 2.00× · `cstpin` 2.11× | HOLDS |
| "~6 % of records physically impossible" | ~6 % | **10/198 = 5.1 %** (94.9 % trustworthy) | HOLDS in magnitude |
| kernel share of the client frame **62.5 %** | 62.5 % | **not re-verified — cannot be, without switching to the open driver** (see (b)) | **UNVERIFIABLE today** |

Reproduction of the "recorded" figures is within a few percent on every render size; `vkheavy` is exact.
Note the open arm's `cstp` range is 101.9–413.1 M inv/s across records, so "parity" is the honest call rather
than any specific ratio.

### a4. VPU / encode (claim 4)

| claim (sunshine-ve2.md, 2026-06-05) | recorded | current-session independent value | verdict / consistency |
|---|---|---|---|
| libx264 superfast 1080p 150 frames | 19.2 s wall, CPU ≈3.8 cores, 30–39 fps ceiling | x264 300 f 720p: **7.2 s CPU idle / 4.40 s loaded** (GPU-FIRMWARE-RE 2026-10-09 13:0x) | CONSISTENT in shape (x264 is 3.6–5.5-core CPU-bound); different resolution/frames → not a like-for-like number |
| VE2 H.264 encode | 1.47 s CPU ≈0.3 cores, 56 fps | VPU 300 f 720p: **0.59 s CPU idle / 0.55 s loaded** | CONSISTENT (0.3–0.4 cores; ~8–12× less CPU than x264). **The recorded row is internally inconsistent**: 150 f / 1.47 s = 102 fps wall, not 56 fps — and 1.47 s at 0.3 cores implies ≈4.9 s wall ≈ 31 fps. Treat 1.47 s CPU as the real number, the fps/cores pair as unreliable. |
| BGR0→NV12 libswscale **~13 fps** = the bottleneck, then libyuv NEON **387 fps / 2.58 ms (30×)** | 13 fps → 387 fps | capture+convert still measured at **3.291 s wall / 2.817 s CPU for 90 frames** (2026-10-09), i.e. the conversion/copy stage is still the capture-side cost | CONSISTENT (the libswscale→libyuv fix is a Sunshine-source patch; the *current* session measured the same stage as the residual cost). Not re-runnable under the safety rule. |
| VE2 decode "no usable hardware decode — proven" | rejected at `checkQualification` | **already falsified by MAPS/LEAD-INTEGRATION-FINDINGS.md** (libvdecoder 1.036 s CPU vs ffmpeg 4.623 s, 300 f 720p) | **FALSIFIED (do not redo)** |
| VE2 encode 41 fps / 74 fps @1080p (lines 35/36) | two different values in the same doc | current 720p numbers scale sensibly (300 f / ~1.45 s wall ≈ 207 fps at 720p) | inconsistent within the prior doc; the 74 fps figure is the one the lead doc already carries |

Headline consistency: **both eras agree that the VPU costs ~8–14× less CPU than x264 and that the bottleneck
was the CPU colour-conversion stage, not the codec.** The `13×`/`12.2×` CPU-saving ratios match.

### a5. FEX / box64 (claim 6)

| claim | recorded | status today | verdict |
|---|---|---|---|
| FEX dynarec floor ≈2× native; unfixable in the emulator | 2× | `/proc/cpuinfo`: `fp asimd aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp` — **no `flagm`, no `lrcpc2`, no `uscat`** | **HOLDS — silicon confirmed** |
| Chrome hot path = 206 MB statically-linked x86; thunking impossible | 206 MB static, ~5 MB dyn | not re-measured | UNVERIFIABLE (needs x86 Chrome + readelf) |
| Parallel/background translation falsified (serial translate→execute) | 1 core 0.76 s vs 5 cores 0.74 s; 2 A76 0.30 s vs 8 cores 0.33 s | not re-measured | UNVERIFIABLE |
| `FEX_MULTIBLOCK=0` → Chrome cold-start 112 s→8.9 s (14×), shipped | 14× | `/opt/fex/bin/FEX` + `FEXServer` present (2026-09-21); `FEX-src` absent (needs re-clone per REFETCH.txt) | config-preserved, effect UNVERIFIABLE |
| box64: int ~0.97×, fp ~2.73×, flags 1.39×, branch 1.39×, memory 1.41× | as recorded | not re-measured | UNVERIFIABLE |
| box64 wraps native ARM libc/libm → ~2× on lib-heavy loads; right choice for ACUITY | ~2× | `/home/radxa/box64-build`, `~/.local/bin/box64` not re-checked | PARTIAL — box64 binary not re-probed |
| box64 Chrome = dead end (windowed SIGSEGV) | SIGSEGV in syscall handler | **directly contradicted by a later entry in the same doc** (2026-06-17: `box64 … chrome --version` exit 0, all 26 NEEDED libs native-wrapped) | **contradicted internally — the later entry wins** |
| FEX `libva` thunk "VE2 decode unproven" blocker | unproven decode | **falsified** (MAPS lead finding) | **FALSIFIED** — the libva thunk scaffolding (`FEX-src/ThunkLibs/libva`, `vaMapBuffer` TODO) is reusable if a VA decode driver is written |

### a6. Other prior claims spot-checked against the live machine

| claim | recorded | current | verdict |
|---|---|---|---|
| NPU has "no lib, tool or package anywhere" (HARDWARE-MAP 13:11) | none | **`/home/radxa/ai-sdk` present with `libVIPhal.so`, `libNBGlinker.so`, built `examples/vpm_run/vpm_run` (mtime 2026-10-09 13:27)**; `/dev/vipcore` present (199,0 rw) | **FALSIFIED as of 13:27 today** |
| NPU userspace absent → NPU is a dead end | dead end | above | **FALSIFIED** |
| RAM pinned at top devfreq | 2400 MHz | `/sys/class/devfreq/a020000.dmcfreq/cur_freq` = **2400000000** | HOLDS |
| NPU devfreq bin | 1008 MHz | `*npu*/cur_freq` = **1008000000** | HOLDS |
| "sudo needs a password" (MEMORY/radxa_a7a_setup) | needs password | `sudo -n true` succeeds | **FALSIFIED (drift)** |
| Vendor DDK version | 24.2@6603887 | `libVK_IMG.so.24.2.6603887`, `libGLESv2_PVR_MESA.so.24.2.6603887`, `libsrv_um.so.24.2.6603887`, `libPVROCL.so.24.2.6603887` | HOLDS |
| `libvdecoder.so` blob size | 135400 B | 135400 B (2026-01-16) | HOLDS |
| Desktop is KasmVNC software-X `:1` | KasmVNC Xvnc :1 | `kwin_x11` live on `DISPLAY=:0`, no Xvnc/Xwayland/weston process | **DRIFTED** |
| PCO fixes shipped in Mesa | 4 commits `c2bde57 c251c9b 5a1be21 167a943` | all four present in `/home/radxa/mesa/mesa-main`; **43 commits ahead of `main`** | HOLDS |
| 4 PCO fixes = probe-level only, no client effect | measured on 2 scenes | not re-measured | UNVERIFIABLE (needs open driver) |

---

## (b) Claims that could NOT be verified, and exactly why

| claim | why unverifiable | what it would take |
|---|---|---|
| **Kernel share of the client frame 62.5 %** | It is the sys-time share of the *open-stack* client frame (Zink/glmark2 under Weston+Xwayland, 2048 s4). Measuring it needs the open `powervr` module bound. **The safety rule forbids touching the binding (kwin_x11 live), and `harness.py --driver=open` would correctly refuse.** Source: `FINAL-HANDOVER-2026-10-08.md:38,891` (S22 tally), `results-2026-10-06-open-stack-wayland.md:2084` (`desktop | 62.5 | 45.5 | 17.0`) | Stop `display-manager`, run `switch-open.sh`, run the open-stack Weston+Xwayland client scene, read `/proc/<client>/stat` utime/stime, then switch back — preferably as one controlled `ab.sh` pair with a trap-restore. **Do not attempt while kwin is live.** |
| softpipe 0.5 Mpix/s @loop=64 and the ~600× number | Would need the software GLES path (`glbench` with system Mesa, surfaceless). The system now has no arm64 Mesa Vulkan/GLES driver packages, only the amd64 Mesa 25.2.8 for FEX | — |
| NPU inference numbers (ResNet-50 ~8 ms/125 fps, YOLOv5s ~26 ms/38 fps, Parrot INT8 130 µs vs CPU 535 µs, 299 GMAC/s) | **Safety rule: do not run NPU workloads.** Only the *userspace presence* was verified (it exists) | A separate NPU-owned session, single instance, after the VE workstream stands down |
| GPU FP32 peak 135 GFLOP/s, ~200 µs dispatch floor, watchdog wedges >~0.5 s | Not re-measured; would need a new peak probe (and risks the watchdog) | A `vkpeak`-style probe with small dispatches, on the vendor arm |
| UFS 1.64 GB/s read / 255 MB/s write / 115k / 44.8k IOPS; CPU 875 / 3204 ev/s | Not re-run (fio/sysbench not exercised in this audit) | `fio` and `sysbench` runs — safe, cheap, not part of this task's GPU mandate |
| Cross-device DRI3 present PASS (native + x86-FEX-thunk vkcube on HDMI) | Needs an Xorg `:2` on card0 and a **connected** HDMI sink; the prior doc records that a *disconnected* HDMI was the original false failure | Reconnect an HDMI sink (or dummy plug) and re-run `pvr-dri3-test/run.sh` |
| All FEX/box64 and Chrome-FEX runtime claims | `/home/radxa/FEX-src` is absent (REFETCH.txt says re-clone); running x86 Chrome and browsing is a different workload class than this audit | Re-clone FEX per `REFETCH.txt`, then re-run the cold-start A/Bs |
| 4 PCO commits' client-level effect | needs open driver (see 62.5 % above) | same ab.sh route |

---

## (c) Version-drift table (MANIFEST/backup ≈ bullseye era vs installed now)

| component | prior (backup / 2026-06 era) | installed now (2026-10-09) | verdict |
|---|---|---|---|
| distro | Debian 11 bullseye (`MANIFEST-vendor-versions.txt`) | **Debian 13.7 trixie**, glibc 2.41 | DRIFTED |
| kernel | 5.15.147-21-a733 (BSP, from Radxa image) | **6.6.98-5-aw2511** | DRIFTED |
| `pvrsrvkm` | built for 5.15.147-21-a733; `img-bxm-dkms 0.1.0-3` | DKMS-built for 6.6.98-5-aw2511, vermagic matches, no version string, no dpkg entry | DRIFTED (rebuilt) |
| vendor DDK | 24.2@6603887 | 24.2.6603887 | HOLDS |
| vendor Vulkan ICD | `libVK_IMG.so.1`, api 1.3.277 | `libVK_IMG.so.24.2.6603887`, api 1.3.277 | HOLDS |
| vendor GLES / OpenCL | `libGLESv2_PVR_MESA.so.24.2.6603887`, `libPVROCL.so` | both present, same versions | HOLDS |
| Vulkan loader | `libvulkan1 1.2.162` (too old for the 1.3 ICD — documented blocker) | amd64 `libvulkan1 1.3.275`; arm64 loader not enumerated | DRIFTED / improved |
| system Mesa (arm64) | `mesa-vulkan-drivers:arm64 20.3.5-1` | no arm64 Mesa Vulkan driver in dpkg; `/usr/lib/aarch64-linux-gnu/dri/` populated | DRIFTED |
| Mesa (open arm, local build) | Mesa 24.0.1 build attempt → 25.0.7 → 25.3.0 | **`/home/radxa/mesa/mesa-main` = 26.3.0-devel**, 43 commits ahead of `main`, `libvulkan_powervr_mesa.so` api 1.4.363 | DRIFTED |
| Mesa (amd64, for FEX) | — | `libegl-mesa0:amd64 25.2.8-0ubuntu0.24.04.2` | new |
| libva | bullseye-era (ve2-vaapi) | **2.22.0** (`libva.so.2.2200.0`) | DRIFTED |
| ffmpeg | 4.3.9-0+deb11u2 (`libavdevice58`) | **7.1.5-0+deb13u1** | DRIFTED |
| libcedarc | `libcedarc-dev-2.0.0-arm64 1.0.1` | no dpkg entry found; `libvdecoder.so` present | DRIFTED |
| `libvdecoder.so` | 135400 B | 135400 B (2026-01-16) | HOLDS |
| `libvencoder.so` | not in manifest | 77096 B (2026-01-16) | new |
| chromium | 120.0.6099.224-1~deb11u1 | not in apt inventory | DRIFTED/lost |
| firefox-esr | 140.11.0esr-1~deb11u1 | not re-checked | UNVERIFIABLE |
| NPU userspace | absent (`REFETCH.txt` says re-clone `ai-sdk`) | **`/home/radxa/ai-sdk` present**, `libVIPhal.so`/`libNBGlinker.so` v2.0, `vpm_run` built 2026-10-09 13:27 | NEW |
| sudo | "sudo needs a password" | passwordless | DRIFTED |
| GPU clock | 600 MHz | 1104 MHz (DT overlay, 990 mV) | DRIFTED |

---

## (d) Ten prior findings the current session does not already have

Current-session knowledge = `MAPS/LEAD-INTEGRATION-FINDINGS.md`, `MAPS/BLOCK-INVENTORY.md`,
`MAPS/G2D-UNLOCK.md`, `Recovery/GPU-FIRMWARE-RE-2026-10-06.md` and the pvr-vulkan bench dir.

1. **The NPU has a working userspace and works end-to-end — the current session calls it a dead end.**
   `npu-viplite-working.md`: VeriSilicon VIPLite (`vipcore`, ABI 2.0.3.2-AW-2024-08-30, chip `0x1000003b`,
   VIP9000-class, 8 NN cores). Runtime = `libVIPhal.so` + `libNBGlinker.so` from
   `github.com/ZIFENG278/ai-sdk`; driver enforces exact major.minor.sub_minor match.
   **Measured: operator net ~3.0 ms/inf; ResNet-50 ~8.0–8.4 ms/125 fps; YOLOv5s ~26 ms/38 fps with correct
   top-5/COCO detections.** NPU memory pool is self-managed (~1–1.6 MB via its own MMU), so `CmaFree` does not
   matter. **Verified present live today** at `/home/radxa/ai-sdk/…/v2.0/`.
2. **NPU INT8 datapath corrupts NEGATIVE input activations** (error grows with |x|, output pins to the quant
   floor −0.955 = code −128; every prior working case fed ≥0 pixel/ReLU values). Fix = shift the input domain
   non-negative and compensate in `b1` (`b1' = b1 − 2·ΣW1`) — exact, no retrain. Any future NPU work needs this.
3. **NPU performance model:** a narrow FC starves the MAC array (~19 GMAC/s); the same math as 1×1 convs over a
   spatial grid runs at **130 µs vs 2016 µs (15.5×)** and 4.1× faster than CPU. GMAC/s rises with channel
   depth: 16 ch 44 · 96 ch 299 · 256 ch 440. **INT8 is the speed floor; FP16 is ~200× slower; INT4 is no faster
   than INT8.** Datapath matrix complete.
4. **The GPU's FP32 compute peak is 135 GFLOP/s**, CPU 50 GFLOP/s FP32, NPU ~600 GOP/s INT8 →
   **NPU > GPU > CPU** on this SoC (`npu-parrot-offload.md`). GPU per-dispatch floor ≈200 µs
   (`vkpeak2 iters=1`).
5. **The PowerVR watchdog self-wedges on an oversized compute dispatch.** A single ~multi-TFLOP dispatch
   (`vkpeak 16384 grp × 200000 iter`) tripped `pvr_device_wdg` and wedged the GPU; it recovers only after the
   job is killed + a watchdog reset. Keep each dispatch under ~0.5 s GPU time. **Directly relevant to any
   scaling of the harness's `vkheavy` probe.**
6. **Cross-device DRI3 present WORKS end-to-end** — render on `renderD128`, DRI3/Present, scanout on
   `card0`/HDMI — for **both** native ARM `vkcube` and x86 `vkcube` through the FEX Vulkan thunk
   (`dri3-reboot-resume.md`, 2026-06-10, verified visually). The earlier failure was a **disconnected HDMI**
   forcing a 1024×768 modeset with glamor disabled, not a software bug.
7. **FEX's translation ceiling is silicon-bound, confirmed today:** `/proc/cpuinfo` still shows no `flagm`,
   no `lrcpc2`, no `uscat` — the ~2× dynarec floor cannot be moved by config/build/kernel. And the shipped
   win is **`FEX_MULTIBLOCK=0` → Chrome cold-start 112 s → 8.9 s (14×)**, which is unrelated to the floor.
8. **FEX's `libva` thunk scaffolding already exists** (`FEX-src/ThunkLibs/libva`, builds; only
   `vaMapBuffer`/`vaUnmapBuffer` TODO). The prior doc's *other* libva blocker — "VE2 decode unproven" — is
   now falsified, so **this thunk is the shortest path to hardware video decode inside the browser** if a
   VA-API decode driver is written.
9. **box64 is the right translator for library-heavy x86, FEX for Chrome.** box64 wraps the native ARM
   libc/libm/libpthread (`BOX64_LOG=1` shows `Using native(wrapped)` for all 26 of Chrome's NEEDED libs) and
   wins ~2× on TF/numpy/7-zip; measured micro-overheads int 0.97×, fp 2.73× (inherent), flags/branch 1.39×,
   memory 1.41×; `BOX64_PROFILE=fast` cuts memory 0.78→0.50. The A76 retune was measured a **no-op** for real
   apps (3389→3390 ms) because `-mcpu` tunes box64's own C, not generated guest code.
10. **Concrete hardware baselines the current session has not measured:** storage is **UFS, not eMMC**
    (`/dev/sda`, `sunxi-ufs-pltfm`, YMUS8A1TE2D1C1) — seq read 1.64 GB/s, write 255 MB/s, rand-4K 115k/44.8k
    IOPS; RAM LPDDR5-4800 pinned at 2400 MHz (bins 400/800/1200/2400); NPU devfreq 492/852/1008; CPU
    875 ev/s single / 3204 all-8 (2×A76@2.002 + 6×A55@1.794, 3.66× scaling).

Two operational constraints worth carrying: **never enable the systemd hardware watchdog** (A733 ~15 s wdt →
reset loop), and the fan curve is `OFF <58 °C ramping to 78 °C`.

---

## (e) Immediately reusable prior work on this machine

| artifact | path | what it buys |
|---|---|---|
| NPU runtime + built demos | `/home/radxa/ai-sdk` (`libVIPhal.so`, `libNBGlinker.so`, `examples/vpm_run/vpm_run`, `examples/{awnn_demo,yolo_dump}.c`) | NPU inference today, no re-clone |
| Cross-device DRI3 present test | `/home/radxa/pvr-dri3-test/` | renderD128 → card0 scanout verification |
| VA-API encode driver source | `backups/a733-backup/ve2/ve2-vaapi/sunxi_ve_drv_video.c` | restores the lost `pvr_drv_video.so` (a previously achieved outcome) |
| GPU clock overlay + rollback | `upgrade-20260922/gpu-clk-1104mhz.dts`, `rollback-gpu-clock.sh`, `/boot/dtbo/gpu-clk.dtbo` | 1104 MHz is live and reversible in one command |
| Benchmark harness | `bench/pvr-vulkan/{harness.py,logcheck.py,ab.sh,sweep.sh,components.sh,README.md}` | driver-aware probes, A/B with trap-restore, record vetting |
| Mesa PCO fixes | `/home/radxa/mesa/mesa-main` (43 commits ahead) | the 4 loop-unroll/immediate-hoist fixes, still applied |
| GPU firmware guard | `gpu-fw-guard.service` + `/home/radxa/gpu-fw-backup/*.orig` + `Recovery/GPU-FIRMWARE-RE` §2b | automatic firmware restore before the GPU probes — the anti-panic-loop safety net |
| FEX runtime + local patches | `/opt/fex/bin/{FEX,FEXInterpreter,FEXServer}`; patches in `publish-repos.md` / `fex/fex-local-uncommitted.patch` (needs `FEX-src` re-cloned) | unaligned-atomic backpatch, thread-context pooling, VAAPI thunk source |
| box64 tuning | `/home/radxa/box64-build`, `~/.box64rc` recipe in `box64-tuning-study.md` | `PROFILE=fast, STRONGMEM=1, CALLRET=1, BIGBLOCK=2, FORWARD=1024, DYNACACHE=1` |
| Quality-gate lesson | `feedback-primary-sources-first.md` | read vendor demo code / newer libs before declaring a hardware limit — the exact mistake that produced the false "no hardware decode" |

---

## Reproducibility notes

* `logcheck.py` today: **198 records, 183 with jobs, 10 suspicious (5.1 %), 188 trustworthy (94.9 %)**.
  The bad records are `vkheavy`/`vkrender` fence-mispairings (two stages within 1 %, or a stage longer than the
  frame). `--clean` is the only safe A/B source.
* Running `harness.py` with kwin live inflates and destabilises *wall-clock* numbers (measured here:
  `vkrender 2048` spread 58 %, `vkrender 256` 26 %, `cstp` 47 %) while `vkheavy`'s per-frame figure was stable
  (180.398 vs the recorded median 180.11, 0.2 %). **Prefer the pre-recorded clean log for A/B ratios, and use
  live runs only for throughput/count/timestamp claims** — exactly the discipline in the bench README.
* `harness.py --driver=vendor` reports `switch to vendor: ok (already bound)` and performs **no unbind**;
  `--driver=open` would have been refused with `kwin is alive - guard would abort; refusing to unbind`.
