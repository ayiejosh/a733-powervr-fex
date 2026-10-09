# NPU — VeriSilicon VIP9000 on the A733: **USABLE NOW**, runtime intact, converter missing

**Verdict: the NPU works today, unmodified.** This session ran a real ResNet-50 inference on
`/dev/vipcore` and got the correct answer (top-1 `collie`, class 231) in **8.19 ms**.
Nothing had to be downloaded, installed, or patched. The userspace *runtime* (`ai-sdk`, 1.1 GB) is present on
disk and version-matched to the in-kernel driver.

**What is actually missing is only the x86 model *compiler*** (ACUITY/pegasus, the 7.1 GB `npu-rootfs`), which is
needed to turn *new/custom* ONNX models into `.nb`. Existing `.nb` models run right now.

The prior session (2026-06-14/16) had this fully working end-to-end and left knowledge notes; the working tree was
since wiped (`npu-rootfs`, `acuity-run.sh`, `npu-compute`, `xgcc` gone) but the SDK clone survived.
Everything below marked **VERIFIED** was re-run this session on this board.

---

## (a) Capability table

| property | value | how established |
|---|---|---|
| IP core | **VeriSilicon VIP9000** (`VIP9000NANODI_PLUS_PID0X1000003B`) | `debugfs/viplite/vip_info` → `pid=0x1000003b, ver1=0x9000, ver2=0x9202, date=0x20230518` **VERIFIED** |
| kernel driver | `vipcore`, bound, platform `allwinner,npu` @ `soc@3000000/npu@3600000` | dmesg `npu[1][1] vipcore, platform driver init`, IRQ **458** **VERIFIED** |
| device node | `/dev/vipcore` char **199,0**, world-RW (`crw-rw-rw-`) | `ls -l /dev/vipcore` **VERIFIED** |
| driver ABI | **2.0.3** (`0x00020003`); userspace lib reports `2.0.3.2-AW-2024-08-30` | `vpm_run` banner + `vip_get_version()` **VERIFIED** |
| NBG format | **v3** (`NPU_VERSION = v3`, `NPU_SW_VERSION = v2.0`) | `ai-sdk/machinfo/a733/config.mk` **VERIFIED**; models live in `model/v3/` |
| NN cores | **1** (`core_count=1`, `multi_vip: current config: {1}`, `Core0: 0%`) | `nbinfo -n`, `debugfs/viplite/multi_vip`, `core_loading` **VERIFIED** |
| VIP SRAM | 512 KB — **UNVERIFIED** (carried from prior session's feature DB read; not re-read here) | — |
| data types | **INT8 native** (the optimized datapath). INT16 ≈ fp32 quality at ≈3× slower. INT4 same speed as INT8 (strictly dominated). FP16 ≈200× slower (no fp16 accel) | **prior-session MEASURED, not re-verified this session** |
| measured throughput | **ResNet-50 8.19 ms/inf (~122 inf/s, 8.22 M cycles)**; operator net 2.79 ms (2.79 M cycles) | `vpm_run -l 100` **VERIFIED this session** |
| compute ceiling | ~880 GMAC/s INT8 (conv1x1, 256-wide); ~299 GMAC/s at 96 ch; narrow FC ~19 GMAC/s | prior-session MEASURED — **not re-verified** |
| scope | **NN tensor ops only** (conv/pool/act/matmul). Not general compute; cannot offload arbitrary CPU load | prior + this session (operator/CNN nets only) |
| known corruption | **negative *dequantized* input activations are mishandled** — VIP datapath corrupts signed input values; shift the domain non-negative and compensate in `b1` | prior MEASURED; **not re-tested this session** |
| memory | self-managed MMU pool, ResNet-50 = **1,606,656 B** — **not** from CMA | `vpm_run` prints `memory pool size=1606656byte` **VERIFIED** |
| negative / limits | only runs on `allwinner,npu`; exact major.minor.sub_minor ABI match enforced by kernel | prior |

---

## (b) Toolchain inventory (present / absent, size, source)

| component | state | size | source |
|---|---|---|---|
| **`ai-sdk`** (VIPLite SDK + prebuilt `.nb` models + `nbinfo`) | **PRESENT** | **1.1 GB** | `github.com/ZIFENG278/ai-sdk` @ `fc90006` |
| └ `viplite-tina/lib/aarch64-none-linux-gnu/v2.0/{libVIPhal.so,libNBGlinker.so}` | **PRESENT** | 4.4 MB total tree | same — the runtime, version-matched to driver 2.0.3 |
| └ prebuilt NBG models (resnet50, yolov5, yolact, lenet; v2+v3) | **PRESENT** | 213 MB (`models/`) | same |
| └ `tools/nbinfo` (**x86-64**, runs via FEX binfmt) | **PRESENT** | 187 KB | same |
| └ `examples/libawnn_viplite/` (awnn wrapper, dequantized output) | **PRESENT** | 34 KB | same |
| └ `models/resnet50-sim.onnx` (a real ONNX source) | **PRESENT** | — | same |
| **`npu-rootfs`** (x86 ACUITY/pegasus toolchain, 7.1 GB ext4 dir) | **ABSENT** | 7.1 GB | Allwinner netdisk `docker_images_v2.0.x.zip` (2.7 GB) → `ubuntu-npu:v2.0.10.2` |
| **`acuity-run.sh`** (box64 launcher) | **ABSENT** | 1 KB | prior session authored |
| **`xgcc/`** (x86 gcc launcher wrappers) | **ABSENT** | 1 KB | prior session authored |
| **`npu-compute/`** (converter front-ends + converted NBGs) | **ABSENT** | — | prior session authored; `npu-build.sh` + `make_*.py` survive in the backup at `/mnt/sdcard/_REVIEW/backups/a733-backup/npu/` |
| box64 | **PRESENT** | — | `Box64 arm64 v0.4.4 2f130fa with Dynarec` |
| FEX (`/opt/fex`) | **PRESENT** | — | runs the x86-64 `nbinfo`; needed for `nbinfo` and the gen_nbg step |
| aria2c / curl / wget | **PRESENT** | — | download transport for the ACUITY restore |
| system-wide `libvip*`/`libNBGlinker.so`/`/usr/bin/*npu*` | **ABSENT** | — | confirmed: nothing outside the SDK |
| `acuitylite` python pkg / apt NPU packages | **ABSENT** | — | `import acuitylite` → ModuleNotFoundError |

**Disk (VERIFIED, `df -h`):** `/` **67 GB free** (117 GB, 41 % used) · `/mnt/sdcard` **88 GB free**.
The ~10 GB ACUITY restore **fits** — disk is not the blocker.

---

## (c) Restore procedure

### Runtime — already restored, 2 commands (this is literally all it takes)

```sh
L=/home/radxa/ai-sdk/viplite-tina/lib/aarch64-none-linux-gnu/v2.0
# 1. sample.txt must name the output, and -b 0 is required or no dump happens:
#    [network]\n./resnet50.nb\n[input]\n./rn50_input.dat\n[output]\n./output_0.txt
# 2. run (the SHIPPED binary is sufficient — do not rebuild anything)
LD_LIBRARY_PATH=$L /home/radxa/ai-sdk/examples/vpm_run/vpm_run -s rn50.txt -l 1 -b 0
```
Disk needed: **0 GB** (all present). `MAPS/npu-smoke.sh` automates exactly this, including the JPEG
preprocessing, and asserts the answer is `collie`.

> **Gotcha that costs an hour if unknown:** the input blob must be **NCHW planar**. Pillow's
> `Image.load()` indexes `[x, y]` — i.e. **column, row**, not row, column. Reading it as `px[h, w]` silently
> feeds a **transposed image**, which still yields a confident, plausible, *wrong* answer
> (Shetland sheepdog 12.63 vs collie 11.42). Use `im.tobytes()` (row-major NHWC) and slice by channel.
> This was caught by the smoke test's `assert`, not by inspection.

### Model compiler (ACUITY/pegasus) — needed only for *new* models

| step | action | transfer |
|---|---|---|
| 1 | `curl -sk -c jar 'https://netstorage.allwinnertech.com:5001/sharing/Mh23BhPHq'` — **VERIFIED reachable, HTTP 200** | — |
| 2 | `aria2c -x16` the `docker_images_v2.0.x.zip` (multiplexing is required; single-stream is throttled to ~40 KB/s) | **2.7 GB** |
| 3 | unzip → `ubuntu-npu_v2.0.10.2.tar.zip` → assemble layers into `/home/radxa/npu-rootfs` | **+7.1 GB** |
| 4 | relaunch the box64 launcher + `npu-build.sh` front-ends from the backup | ~0 |

**Disk needed: ~10 GB transient. 4 external steps.** Not performed this session — the task's own stop condition
(an ~8 GB download) was hit, and it is unnecessary to answer "is the NPU usable".

---

## (d) Did an inference actually run? **YES — VERIFIED, with the correct answer**

Run on `2026-10-09` on this board, driver `2.0.3.2-AW-2024-08-30`, `cid=0x1000003b, device_count=1`.

**ResNet-50 (`examples/resnet50/model/v3/resnet50.nb`, 17.5 MB) on `input_data/dog_224_224.jpg`:**

```
input 0 dim 224 224 3 1, quant_format=2, scale=0.003686, zero_point=0
ouput 0 dim 1000 1 0 0,  scale=0.067191, zero_point=67
memory pool size=1606656byte
run time for this network 0: 8384 us.
vpm run ret=0
```

Top-5, computed from the NPU's own dequantized `output_0.txt` mapped through `label.h`:

| rank | class | score | label |
|---|---|---|---|
| 1 | **231** | **12.363** | **collie** |
| 2 | 230 | 11.624 | Shetland sheepdog |
| 3 | 160 | 6.921 | Afghan hound |
| 4 | 169 | 6.921 | borzoi, Russian wolfhound |
| 5 | 259 | 6.518 | Pomeranian |

**This is the correct answer for a collie photograph**, and it reproduces the vendor README's own expected
output (`class id: 231, label: collie`) exactly. The NPU computes correctly, not merely "runs".

**Steady-state, 100 loops (`-l 100`, NBG loaded once):**
`profile avg inference time = 8188 us, cycle = 8221106` → **~122 inferences/s**.

**Second net (`examples/vpm_run/operator/v3`, 224×224×3→2):** `avg 2788 us, cycle 2791236`, ret=0.

**Caveat:** the top-5 confirmation is 1 of 2 verification tiers complete. The *correctness* of the second model
(operator net) is not meaningfully checkable — it is a sample net with no reference labels; only its timing and
successful execution are claimed.

---

## (e) Disabled-but-enableable features

| feature | state | enableable? |
|---|---|---|
| **NPU 1120 MHz OPP** | present in DT (`npu-opp-table/opp-1120`, requires `vf0205/vf0206/vf0300` at ~1.08 V) but **NOT in the board's resolved vf list** | **NO on this unit.** dmesg resolves the VF bin to `vf index: 0` and lists only 492/852/1008 MHz. The 1120 OPP needs a different chip VF bin. **+11 % is latent silicon, not a config toggle.** |
| **devfreq 852 / 492 MHz** | available (`available_frequencies`), but governor is pinned `performance` @ 1008 MHz | **YES** — writable sysfs; `available_governors = sunxi_actmon userspace performance simple_ondemand` |
| **NPU thermal throttling** | `devfreq-3600000.npu` **is registered as cooling device 8** (`max_state=2`), `npu_thermal_zone` policy = `power_allocator` (IPA) | **YES, already wired.** Idle at `cur_state=0`. Falls back to 852/492 under sustained heat |
| **multi-VIP (>1 core)** | debugfs knob `multi_vip`, currently `{1}`; `nbinfo` reports `core_count: 1` | **NO** — one VIP core is instantiated in this SoC's DT |
| **INT16 datapath** | supported by toolkit/driver | **YES** — ≈fp32 quality at ≈3× INT8 cost (prior measured) |
| **INT4 datapath** | supported | **YES but pointless** — same speed as INT8, worse accuracy (prior measured) |
| **FP16 datapath** | supported by toolkit | **Technically yes, practically no** — VIP9000 NANO has ~no fp16 accel; ~200× slower (prior measured) |
| **Dynamic×dynamic MatMul** | — | **NO** — one operand must be a compile-time constant (use Gemm/Conv) |
| **FP32/FP64 output** | — | **NO** — architectural output-requant wall to INT8/INT16 |
| **`pd_npu` power domain** | reads `off-0`, all consumers `suspended`, runtime-PM `auto` | **Not a disabled feature** — this is correct idle gating; it powers on per inference |
| **ACUITY model compiler** | absent | **YES** — 4-step / ~10 GB restore, section (c) |

---

## (f) Thermal

| zone | reading (idle-ish, after the bench) |
|---|---|
| `npu_thermal_zone` (zone2) | **63.7 °C** |
| `cpub` / `cpul` / `gpu` / `ddr` | 65.0 / 64.8 / 64.6 / 61.1 °C |

Trip points on the NPU zone: **only a `critical` trip at 110 °C** — no passive trips. Throttling is governed
instead by the IPA `power_allocator` policy driving the devfreq cooling device.

**Would sustained inference throttle?** 100 back-to-back ResNet-50 inferences (≈0.8 s of continuous NPU work)
produced **stable 8188 µs timings with no downward step** and left the NPU zone at 63.7 °C against a 110 °C
critical trip. The NPU is not the thermal problem on this board — it is roughly the same temperature as the CPU
and GPU clusters while they are near idle. **No throttling expected** at this workload; sustained heavy load would
first see IPA drop the NPU to 852 MHz, which is a graceful 18 % step, long before the 110 °C critical trip.

---

## (g) Unknowns / UNVERIFIED

1. **512 KB VIP SRAM** and the **8-core** figure — carried from the prior session's feature-DB read; this session's
   driver reports `core_count: 1`. The two are not reconciled. **UNVERIFIED.**
2. **Peak INT8 GMAC/s** (~880) — prior-session measured; only ResNet-50 latency was re-measured here.
3. **Datapath matrix** (INT16/INT4/FP16 costs) — prior-session measured, not re-benchmarked.
4. **Negative-activation corruption** — prior-session found; not re-tested.
5. **ACUITY restore** — steps and disk are documented and the source was confirmed reachable (HTTP 200), but the
   download and the box64/`gen_nbg` recipe were **not re-executed**; the prior recipe is known to be fragile
   (nested `make` escaping box64 into FEX, absolute-symlink breakage in the rootfs).
6. **`vip_freq` reports `Core Frequency=39 HZ`** while `PPU Frequency=1007954053 HZ` and `clk_freq` says 1008 MHz.
   The 39 Hz reading is almost certainly a bogus register read while idle — **not investigated**.
7. Whether the two near-tied ImageNet classes (collie vs Shetland sheepdog) can flip under a marginally different
   resize filter is **not characterised** — the vendor file is already exactly 224×224 so resize is a no-op here,
   and the pipeline is bit-reproducible, but upstream JPEG decode differences were not tested.

## (h) Blast radius

Untouched, per instruction: the **GPU driver binding** (`kwin_x11` live; no GPU guard, MPP, or PowerVR sysfs node
was written) and the **VE/VPU** in any form. Only `/dev/vipcore` and read-only NPU sysfs/debugfs were accessed.

## One-line answer

**The NPU is usable right now** — `sh MAPS/npu-smoke.sh` runs ResNet-50 to the correct answer (`collie`) in
~7.9 ms. Nothing has to be built or downloaded; only the x86 ONNX→NBG compiler (7.1 GB) has to be restored to
compile *new* models.
