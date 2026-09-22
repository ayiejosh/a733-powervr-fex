# Performance work — 2026-09-22 (trixie / kernel 6.6.98-5-aw2511)

Everything below was measured on a Cubie A7A sitting on a desk, not estimated. Three wins, one
wall, and a correction to what this repo previously claimed about the SoC's clock ceiling.

Illustrated summary: [`comics/2026-09-22-unlocking-the-a733.svg`](comics/2026-09-22-unlocking-the-a733.svg)
and [`comics/2026-09-22-ceiling-hunt.svg`](comics/2026-09-22-ceiling-hunt.svg).

## Short version

| | before | after | how |
|---|---|---|---|
| **GPU** | 600 MHz (driver fallback) | **1104 MHz @ 990 mV** | DT overlay supplies the `clk_rate` the driver asks for; ceiling measured by sweeping |
| **L3 / DSU fabric** | 780 MHz, never scaled | **1027 MHz** | DT overlay sets the clock at boot (the vendor's scaling driver is **not compiled in**) |
| **CPU** | 1508 / 1716 MHz (stale thermal clamp) | **1794 / 2002 MHz** | clear the frozen cooling-device state |
| **CPU idle** | always max clock | **416 MHz idle, max on demand** | demand-driven floor boost (cgroup-based) |
| CPU clock itself | 1794 / 2002 MHz | *unchanged — already at the ceiling* | see §4 |

## 1. The GPU ran at 600 MHz because one DT property was missing

`pvrsrvkm`'s `sunxi_platform.c` does not use the OPP table. It reads a plain `clk_rate` property off
the GPU node and falls back to its own default if absent:

```
pvrsrvkm 1800000.gpu: warning: default clk_rate is NOT set in DTS, set it to default:600000000
pvrsrvkm 1800000.gpu: sunxi_parse_dts clk_rate:600000000
```

Radxa never set it, so the GPU ran at 600 MHz on every boot, despite the DT's own `gpu-opp-table`
declaring `opp@400/600/800/1008000000`. A 372-byte overlay fixes it (shipped as
[`../overlays/gpu-clk.dts`](../overlays/gpu-clk.dts), which uses the final 1104 MHz value from §2):

```dts
target-path = "/soc@3000000/gpu@1800000";   __overlay__ { clk_rate = <1104000000>; };
target-path = ".../regulators@0/dcdc4";     __overlay__ { regulator-min-microvolt = <990000>; };
```

The driver also ignores the OPP table for **voltage** (it imports no `dev_pm_opp_*` symbols): it calls
`regulator_get_voltage()` at probe and re-applies whatever it finds. At 800 mV that is below even the
best speed bin's requirement for 1008 MHz, so the rail floor is pinned to the OPP's own voltage for
the target point — the driver then reads that back as its own default
(`sunxiSetVoltage:990000`). The first working step was 1008 MHz @ 960 mV; §2 and §3 took it to
**1104 MHz @ 990 mV**, which is what ships.

## 2. The GPU ceiling is 1104 MHz — measured, not assumed

`bench/GPU_BENCHMARK.md` previously said the 600 MHz clock "could add headroom — needs verification
… do NOT assume; risk of instability". Verified: the clock generator tops out at **1104 MHz**.
Requests above it are silently clamped:

| requested | actual | glbench loop4 |
|---|---|---|
| 1008 (the value the first overlay shipped) | 1008 | 6976 Mpix |
| 1104 | **1104** | 7428 Mpix |
| 1152 / 1200 / 1248 / 1296 / 1344 / 1392 | **all clamped to 1104** | 7624–7641 Mpix |

One trap from the same sweep: a *runtime* request of exactly `1008000000` on `gpu_clk` came
back as `552000000` — reproducibly, twice. The clock framework picks the parent PLL it
likes for a request, not the one you meant, so `overlays/gpu-clk.dts` ships the value that
reads back as requested (**1104 MHz**), and every sweep reads the achieved rate instead of
trusting the write. See [`../bench/clkctl/`](../bench/clkctl/).

Method: a small debugfs module (`clkctl`) exposes `clk_set_rate()` for the GPU clocks so an entire
sweep runs in one boot instead of one reboot per frequency — see §6.

At 1104 MHz @ 990 mV: 58–61 °C, zero error lines across every run, and **+84 % clock** (and
+77–84 % on glbench) over the day's starting 600 MHz. **Note:** glbench (pure GPU throughput) gains
+6–10 % over 1008 MHz, but `glmark2` is unchanged (826 vs 816–829) — glmark2 on this board is
CPU/driver-bound, so the extra GPU clock shows up in GPU-bound work, not in that score.

## 3. The hidden one: the L3 / DSU fabric was stuck at 780 MHz

```
$ grep CONFIG_AW_SUNXI_DSUFREQ /boot/config-$(uname -r)
# CONFIG_AW_SUNXI_DSUFREQ is not set
```

The vendor's DSU (L3 + coherency fabric) frequency-scaling driver is **not built into this kernel**,
so nothing ever changes its clock: it stays at the bootloader's 780 MHz while the CPU clusters run to
2002 MHz. The DT even ships a full `dsu-opp-table` (up to 1352 MHz) that no driver consumes.

Evidence it was a real bottleneck: at 780 MHz the big core's **L3 read bandwidth (10.2 GB/s) barely
exceeded DRAM read (8.2–9.3 GB/s)** — L3 should be well clear of DRAM.

Raising it to 1027 MHz (via `assigned-clock-rates`, §6) gave:

| metric (big core) | DSU 780 | DSU 1027 |
|---|---|---|
| `l3read` | 10.2 GB/s | **10.9–12.2** |
| `l3shared` (coherency) | 12.4 | **13.4–15.6** |
| `dramread` | 8.2–9.3 | **10.3–11.8** |
| `dramcopy` | 3.2 | 3.4 |
| `fex.tcreate` (thread start) | 181951 ns/op | **123820** |
| SHA-256, GPU, atomic benches | — | unchanged (as expected) |

**Knee:** 1196 MHz was tested too and bought nothing further (all metrics within noise), so the safer
1027 MHz setting is kept. Stability: two sustained mixed-load passes (60 s + 40 s), 0 kernel-trouble
lines, 62–65 °C.

## 4. The CPU is already at its ceiling — overclocking it does not work here

The CPU clusters were believed "firmware-capped at 1716 MHz" (see §7). They are not: clearing a stale
thermal clamp restores the full **1794 / 2002 MHz** (§5).

Going *above* that was attempted and failed, and the failure is informative. New OPP entries
(2100–2400 MHz big, 1900–2000 little) were added to `cluster0/1-opp-table` with the vendor's complete
property set, plus raised rail floors (1.10 V / 1.02 V). Result:

* the extra frequencies were **not offered** by cpufreq — this BSP selects the frequency list from the
  chip's **factory speed grade** (efuse vf bin), not from the DT table;
* the reported maximum even *moved down* (big 2002 → 1992 MHz, little 1794 → 1800 MHz), because the
  added entries perturbed the bin matching.

Two useful side-findings from that attempt:

1. OPP entries **must** carry voltage information. Entries without it make the OPP core reject the
   whole table (`opp_parse_microvolt: opp-microvolt missing although OPP managing regulators` →
   `OPP table can't be empty`), which **kills cpufreq entirely** and pins the CPU at 1014 MHz.
2. Because of (1) the OC test overlay was reverted; the board runs the vendor table, and the boost
   controller never selects anything untested.

Conclusion: the CPU cannot be overclocked through the DT on this BSP. Any further gain would mean
ignoring the factory speed grade, which is not worth random crashes on a headless board.

## 5. The CPU had been silently capped ~14 % since June

`fan-curve.sh` switched the thermal zones to `user_space` but never cleared the cpufreq clamp the
kernel had already applied at boot (temperatures cross the 60 °C trip immediately):

| cooling device | state found | effect |
|---|---|---|
| `cpufreq-cpu0` | 3 / 8 | little cores pinned to 1508 MHz |
| `cpufreq-cpu6` | 4 / 11 | big cores pinned to 1716 MHz |

That clamp cannot be overridden by writing `scaling_max_freq` (the script's own `restore_clock()` was
silently ineffective) — only clearing the cooling-device state releases it.
**Measured: SHA-256 8-thread 5,778,636k → 6,593,437k (+14.1 %).**

## 6. Two techniques worth reusing

**Clock sweeping without reboots.** [`../bench/clkctl/`](../bench/clkctl/) is a ~60-line debugfs
module exposing `/sys/kernel/debug/clkctl/{gpu_clk,gpu_parent,dsu}`: read = current rate,
write = `clk_set_rate()`. It turned "one reboot per frequency" into a single boot that tests
8 GPU + 3 DSU + 5 CPU levels. Extra safety: sweeps only ever write
`scaling_max_freq`/clock rates at runtime, so a crash mid-sweep reboots back to the safe
saved configuration.

**Demand-driven CPU clocking.** `CONFIG_UCLAMP_TASK` is not set and neither `schedutil`'s nor
`ondemand`'s tunables are exposed, so the kernel cannot express "max while this runs, idle otherwise".
`schedutil` alone costs **−29 %** on CPU-bound GL scenes here (a single busy thread only reaches
1.2–1.4 GHz because the work is bursty — threads block on GPU ioctls). The workaround is a small
Python daemon that keeps schedutil as the base governor and raises the per-cluster *frequency floor*
to max while `user.slice` shows demand: idle 416 MHz, and every core at max while work lasts.
Background daemons (syncthing, tailscaled) live in `system.slice` and cannot trigger it — which
matters, because syncthing alone burns ~30 % of a core continuously.

## 7. Corrections applied to this repo's existing docs (same commit)

These statements were measured to be wrong. All are now fixed — if you have an older
checkout, this is what changed:

| file | was | now |
|---|---|---|
| `README.md` baseline table | "max ~1716 MHz (firmware-capped; 1794 unreachable)" | 1794 / 2002 MHz are reachable; the cap was a **stale thermal clamp** |
| `docs/BENCHMARKS.md` CPU section | "~1716 MHz ceiling (1794 MHz firmware-locked, not reachable from sysfs)" | same correction, with the cause |
| `docs/BENCHMARKS.md` thermal section | "the 1716 cap is a static policy limit, not live throttling" | it was a **frozen cooling-device state** — real throttling that stuck, and `scaling_max_freq` could not override it |
| `system/README.md` fan section + CPU section | "1794 MHz is firmware-locked; 1716 MHz is the ceiling" | same correction, plus the fan script's `release_clamps()` documented as **required** |
| `bench/GPU_BENCHMARK.md` header + Optimization | "GPU clock: 600 MHz fixed … a DTS/clk change could add headroom — needs verification" | **verified**: overlay gives 1104 MHz; the generator's ceiling is **1104 MHz**; banner added so the 2026-06-05 numbers are read in context |
| `docs/BENCHMARKS.md` governor note | "The CPU governor (performance vs ondemand) is ~0% for sustained compute" | true for **saturated all-core** work, **false for bursty single-thread** work (−29% under schedutil) |

`bench/baseline.txt` was refreshed in the same commit (kernel `6.6.98-5-aw2511`, GPU/DSU
clocks recorded in the header), and `RESULTS.md` gained the 1104 MHz rows.

## 8. Final measured numbers

| metric | 2026-07-02 baseline | this morning | final |
|---|---|---|---|
| `gpu.glbench.loop4` | 4186 | 6976 (1008 MHz) | **7392–7644** |
| `gpu.glbench.loop16` | 1215 | 2034 | **2227–2229** |
| `gpu.glbench.loop64` | 315 | 529 | **578–579** |
| `gpu.glbench.loop256` | 80 | 134 | **147** |
| `glmark2-es2` vendor GLES | 659 (600 MHz) | 816–829 | **826** |
| `glmark2-es2` zink | 454 | 573–588 | **581** |
| `cpu.1thread.evps` | 759.8 | 847.4 | **847.8** |
| SHA-256 8-thread | 5.78 M (clamped) | 6.75 M | **6.75 M** |
| `fex.atomic` off0/off2/off14 | 136.6 / 53.8 / 51.1 | 153.1 / 61.6 / 56.9 | 152.1 / 61.4 / 56.9 |
| `l3read` (big core) | — | 10.2 (DSU 780) | **10.9–12.2** (DSU 1027) |

## 9. Reproduce / roll back

Everything used here ships in this repo — no paths outside it are needed.

```sh
# the two clock overlays (GPU 1104 MHz, DSU 1027 MHz) and the rollback procedure
overlays/README.md          # build with dtc, cp to /boot/dtbo, sudo u-boot-update, reboot

# revert either one, then reboot:
sudo rm /boot/dtbo/gpu-clk.dtbo     # GPU back to the 600 MHz default
sudo rm /boot/dtbo/dsu-clk.dtbo     # DSU back to 780 MHz
sudo u-boot-update

# CPU profile (idle power vs always-max), plus the demand-driven boost
system/README.md            # cpu-mode.sh auto|max|balanced|eco, cpu-boost.service

# the clock-sweep module used for the ceiling tests
cd bench/clkctl && make && sudo insmod clkctl.ko     # then read/write .../debug/clkctl/*

# the memory-hierarchy probe that found the starved DSU
gcc -O3 -fopenmp -march=native -ffast-math bench/membw.c -o membw -lm && taskset -c 6 ./membw
```

> The bootloader region is untouched by all of this: the U-Boot graft hash
> (`ed75909431fcc607999684a2a1fc318de81d6798b742a5d7001ef03966f807d6`) was verified unchanged before
> and after every reboot above.
