# CPU, RAM, power-domain and clock map — Radxa Cubie A7A (Allwinner A733)

**Date:** 2026-10-09 · **Kernel:** 6.6.98-5-aw2511 (Debian trixie) · **Board:** radxa-cubie-a7a
**Method:** every number below is read from the live kernel or measured on this board; each table
names its command. **Nothing was changed** — no clock, governor, voltage or power setting was written.
The GPU driver binding was not touched (bound driver stayed `pvrsrvkm`, kwin_x11 alive throughout),
and no VE/VPU or NPU workload was run.

## HEADLINE — nothing that matters can be raised, but the CPU never idles

1. **Every domain with an authoritative maximum is already at it** — CPU (1794/2002 MHz), GPU
   (1104 MHz), NPU devfreq (1008 MHz), DDR (2400 MHz) and every PMIC rail with a target. The DT's DSU OPP table goes to
   1352 MHz but the clock sits at 1027 MHz — and that is *deliberate*: 1196 MHz was measured to buy
   nothing (prior work, §d/N1), and the vendor DSU scaling driver is not compiled in.
2. **The real misconfiguration is the opposite of "below max": the CPU can never idle.** The `auto`
   profile (`cpu-boost.py`) raises `scaling_min_freq` to the hardware maximum whenever **user.slice**
   burns ≥0.60 cores and only releases it after user.slice < 0.15 cores for five 0.2 s windows. On this
   box **user.slice never falls below 1.1 cores** — the DSH web server itself (`node .../dsh`, pid 1171)
   runs in `user.slice/user@1000.service/app.slice` — so **both clusters sat at max (1794/2002 MHz) for
   the whole 30 s observation window**, at 61–67 °C with the fan at PWM 50–109. The 416 MHz idle state is
   unreachable in practice. Command: 30 s sample of `/sys/fs/cgroup/user.slice/cpu.stat` +
   `scaling_min_freq` (see §e/E3 for the one-line revert).
3. **Memory bandwidth is saturated by a single big core** at ~12.0 GB/s read / 10.3 GB/s write /
   13.1 GB/s copy (aggregate). Adding the 2nd big core or all 8 changes it by <1 %. The 6 little cores
   reach only 4.9/7.0/7.5 GB/s — *less than one A76*. Any memory-bound parallel workload is already at
   the wall with one big core.
4. **The GPU is not power-gated at all**: `pd_gpu_core` and `pd_gpu_top` never registered
   (`pdtest ... deferred probe timeout ... error -110`) and are absent from
   `pm_genpd_summary`, so `axp8191-dcdc4` (990 mV) stays on permanently; only the clock gate
   (`gpu0-gate`) saves idle power.

---

## (a) CPU

```
$ lscpu ; cat /proc/cpuinfo ; cat /sys/devices/system/cpu/cpu*/cpufreq/*
```

| cluster | cores | part | ARM core | sockets | OPPs exposed (kHz) | cur/min/max (kHz) | governor |
|---|---|---|---|---|---|---|---|
| little | cpu0–5 | `0xd05` r2p0 | Cortex-A55 | 1 | 416000 780000 1014000 1196000 1404000 1508000 1612000 1716000 1794000 | 1794000 / 1794000 / 1794000 | schedutil |
| big | cpu6–7 | `0xd0b` r4p1 | Cortex-A76 | 1 | + 1898000 1950000 2002000 (12 total) | 2002000 / 2002000 / 2002000 | schedutil |

* `scaling_driver = cpufreq-dt`; available governors: `ondemand performance schedutil`.
* **`scaling_min_freq == scaling_max_freq == hardware max` on both clusters** — the floor is being held
  up by `cpu-boost.py`, not by the governor. `cpuinfo_min_freq` = 416000 for both, i.e. 416 MHz is
  *available* but unreachable while the boost holds.
* DT OPP tables (`/proc/device-tree/cluster0-opp-table`, `cluster1-opp-table`) contain more numbers than
  cpufreq exposes (little up to 1800 MHz, big up to 2002 MHz); the extras are the `opp-microvolt-vfXXXX`
  speed-bin variants of the same points. cpufreq's list is the effective set.
* **ISA — big and little are identical**: all 8 cores report
  `fp asimd aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp`
  → NEON (`asimd`) ✓, **no SVE/SVE2**, dotprod (`asimddp`) ✓, crc32 ✓, AES/PMULL ✓, SHA1/SHA2 ✓,
  LSE atomics ✓, LDRC/RCPC ✓. The clusters differ **only** in microarchitecture and clock.
* **Caches**: `/sys/.../cache/index*` expose L1d, L1i, L2 (Unified, shared 0-7) and L3 (Unified,
  shared 0-7) for *both* clusters — and **no `size` file exists**, `lscpu -C` prints empty sizes, and
  neither the live DT nor `/usr/src` nor the DTB images contain `cache-size`. **L2/L3 sizes are
  UNVERIFIED.** L1 is fixed by the core (A55 32 KiB+32 KiB, A76 64 KiB+64 KiB). The fact that both
  clusters see one L2 shared across 0-7 is implausible for a big.LITTLE part and is reported as the
  kernel describes it, not as a fact about the silicon.
* **cpuidle**: driver `psci_idle`, governor `menu`, 3 states — `WFI` (lat 1 ns), `cpu-sleep-0`
  (lat 105 ns), `cluster-sleep-0` (lat 121 ns); none disabled.
* **Are the big cores used preferentially? YES.** A single unpinned CPU-burner was sampled 30 times at
  150 ms: **cpu6 every time (30/30)**. The task-level answer: one busy thread gets an A76.
* **Are the 6 little cores the ceiling for parallel work?** No — see (b): 6 little cores deliver
  2175 ALU events/s vs 2 big cores' 1690, so the little cluster is collectively *more* throughput, but
  4.9 GB/s of memory bandwidth vs 12.0 for one big core (§c). For memory-bound work the **big cores are
  the ceiling and one of them is enough**.

### `/proc/schedstat` does not exist
`# CONFIG_SCHEDSTATS is not set` — the per-CPU run-queue accounting the task suggested is absent.
Per-CPU placement was measured instead via `/proc/<pid>/stat` field 39 (above) and `/proc/stat`.

---

## (b) Measured compute ceiling

Two deliberately different workloads, because **SHA-256 is crypto-unit-bound** and flatters the A55
(one A76 is only 1.72× an A55 at SHA-256, but 2.33× at integer ALU).

```
openssl speed -seconds 4 -evp sha256 -multi N        (best of 2, pinned with taskset)
sysbench cpu --cpu-max-prime=20000 --threads=N --time=5 run   (best of 2)
```

| configuration | exact command (taskset prefix) | SHA-256 (kbytes/s) | MB/s | per core | sysbench events/s | per core |
|---|---|---|---|---|---|---|
| 1 little | `taskset -c 0` … `-multi 1` / `--threads=1` | 706750 | 707 | 707 | 363.79 | 364 |
| 1 big | `taskset -c 6` | 1213587 | 1214 | 1214 | 847.27 | 847 |
| 6 little | `taskset -c 0-5` … `-multi 6` / `--threads=6` | 4257196 | 4257 | 710 | 2174.95 | 362 |
| 2 big | `taskset -c 6-7` … `-multi 2` / `--threads=2` | 2428127 | 2428 | 1214 | 1690.40 | 845 |
| all 8 | `taskset -c 0-7` … `-multi 8` / `--threads=8` | 5538466 | 5538 | 692 | 3739.92 | 467 |

* Software: `OpenSSL 3.5.7`, `sysbench 1.0.20`.
* **Controls (same core, 2 procs)**: `taskset -c 0 … -multi 2` = 706883 (little) and `taskset -c 6 …
  -multi 2` = 1212076 (big) — both equal the single-proc number, confirming **1 core = 1 core, no SMT**.
* Cluster scaling is linear: 6 little = 6.00× one little (SHA) / 5.98× (sysbench); 2 big = 2.00× / 2.00×.
* **Big-vs-little per core: 1.72× (SHA-256) / 2.33× (integer ALU)** against a clock ratio of only 1.12×.
* all-8 vs the sum of the two clusters measured separately: 5538/6685 = 83 % (SHA), 3739.92/3865 = 97 %
  (sysbench). **Caution:** the same all-8 sysbench run also measured 3047.88 once — the live desktop's
  own load leaks into every unpinned window. The 3739.92 figure was taken with a clock/temp sampler that
  showed both clusters at max throughout (60.4→65.2 °C, no throttle), so it is the better estimate and
  the 3048 is reported as contamination, not a scaling wall.

---

## (c) Memory and bandwidth

```
cat /proc/meminfo ; cat /sys/class/devfreq/a020000.dmcfreq/* ; cat /proc/swaps
taskset -c <set> ./stream 20        # STREAM-style, 3×64 MiB arrays = 192 MiB working set, best of 20
```

| property | value | source |
|---|---|---|
| MemTotal | **6056724 kB (5.78 GiB)** | `/proc/meminfo` |
| MemFree / MemAvailable | 87 MB / 2.29 GB (at sample time) | `/proc/meminfo` |
| DRAM clock | **2400 MHz → 4800 MT/s, LPDDR5-class** | `a020000.dmcfreq` `cur_freq`; boot log `dram_clk:2400` |
| DRAM part / exact type | **UNVERIFIED** (SPL log only; no kernel-visible part ID) | — |
| dmcfreq | governor `performance`, cur/min/max = 2400 MHz, bins 400/800/1200/2400, `sunxi_actmon` also available | `/sys/class/devfreq/a020000.dmcfreq` |
| CMA | **CmaTotal 262144 kB (256 MB); CmaFree 8284 kB → 97 % in use** | `/proc/meminfo` |
| dma_heap | `/dev/dma_heap/system` (rw), `/dev/dma_heap/reserved` (root only) | `ls -l /dev/dma_heap` |
| zram | **active**: zram0, disksize 3101044736 B (2.89 GiB), algorithm `lz4`, used 8960 kB, swap priority **100** | `/sys/block/zram0/*`, `/proc/swaps` |
| swap file | `/swapfile` 8388604 kB, priority −2, 0 used | `/proc/swaps` |
| vm tunables | swappiness 100, vfs_cache_pressure 500, min_free_kbytes 16384 | `/proc/sys/vm/*` |
| coherent_pool | 4 MiB | `/proc/cmdline` |
| memory latency | **not measured** — no `lat_mem_rd`/`mbw` present; UNVERIFIED | — |

### Measured DRAM bandwidth (best of 20 reps, aggregate GB/s)

| pinned to | read | write | copy (read+write counted) |
|---|---|---|---|
| **1 big (cpu6)** | **11.96** | **10.29** | **13.06** |
| 6 little (cpu0-5) | 4.91 | 7.03 | 7.50 |
| 2 big (cpu6-7) | 11.90 | 10.30 | 13.07 |
| all 8 | 11.98 | 10.29 | 13.07 |

**One A76 saturates the memory controller.** The 2-big and all-8 rows are within noise of the 1-big row
(<1 %), so DRAM is a hard shared ceiling at ~12 GB/s read / ~10.3 GB/s write. The 6 little cores are a
*worse* memory engine than a single big core (41 % / 68 % / 57 %). This is the number to compare every
other workstream against: it also explains why the DSU/L3 clock mattered (prior work: raising DSU
780→1027 MHz moved `dramread` 8.2–9.3 → 10.3–11.8 GB/s).

---

## (d) FULL clock table — current vs maximum

```
sudo cat /sys/kernel/debug/clk/clk_summary
```
387 clocks are exposed. **The kernel publishes a clock's *current* rate but never its maximum** — the
`max MHz` column is filled only where a maximum is authoritative from an OPP table, a devfreq table, or a
measured ceiling (noted per row); elsewhere it is `—` and `below max?` says so rather than guessing.
`hw=N` means the hardware gate is closed (block runtime-suspended or unused); it is not an error when
`en>0` — those are simply consumers whose block is runtime-suspended right now.

Only **two** clocks in the whole tree are below an authoritative maximum:

* **`cpu_dsu` = `pll-cpu-dsu` = 1027 MHz vs DT `dsu-opp-table` max 1352 MHz (24 % below)** — N1.
* *(none other)* — every CPU/GPU/NPU/DDR clock with a known max is at it, exactly.

| clock | cur MHz | max MHz | below max? | en | hw | consumer |
|---|---|---|---|---|---|---|
| `ext-32k` | 0.03 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `ext32k-gate` | 0.03 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `rc-16m` | 16.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `iosc` | 16.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `iosc-div32k` | 0.03 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `osc32k` | 0.03 | — | n/a — no max exposed | 0 | Y | `2000000.pinctrl` |
| `hdmi-ref` | 0.03 | — | n/a — gated | 0 | N | `5520000.hdmi0` |
| `irrx` | 0.03 | — | n/a — gated | 0 | N | `deviceless` |
| `rtc32k` | 0.03 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `r-irrx` | 0.03 | — | n/a — gated | 0 | N | `deviceless` |
| `fanout0` | 0.03 | — | n/a — gated | 0 | N | `deviceless` |
| `fanout1` | 0.03 | — | n/a — gated | 0 | N | `deviceless` |
| `fanout2` | 0.03 | — | n/a — gated | 0 | N | `deviceless` |
| `fanout3` | 0.03 | — | n/a — gated | 0 | N | `deviceless` |
| `rtc-32k-fanout` | 0.03 | — | n/a — gated | 0 | N | `deviceless` |
| `rtc-1k` | 0.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `sys24M` | 24.00 | — | n/a — no max exposed | 11 | Y | `timer@3009000` |
| `usb2-suspend` | 24.00 | — | n/a — no max exposed | 1 | Y | `6a00000.xhci2-controller` |
| `spi0` | 8.00 | — | n/a — no max exposed | 1 | Y | `2540000.spi` |
| `sys12M` | 12.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `apb2jtag` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `csi-master2` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `csi-master1` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `csi-master0` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `ledc` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `hdmi-sfr` | 24.00 | — | n/a — no max exposed | 1 | Y | `5520000.hdmi0` |
| `dsi1` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `dsi0` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `gmac-ptp` | 24.00 | — | n/a — no max exposed | 1 | Y | `4500000.ethernet` |
| `pcie0-aux` | 24.00 | — | n/a — no max exposed | 1 | Y | `6000000.pcie` |
| `usb2-u2-pipe` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `usb2-u3-utmi` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `usb2-mf` | 24.00 | — | n/a — no max exposed | 1 | Y | `6a00000.xhci2-controller` |
| `usb2-u2-ref` | 24.00 | — | n/a — no max exposed | 1 | Y | `6a00000.xhci2-controller` |
| `usb-ref` | 24.00 | — | n/a — no max exposed | 3 | Y | `4200400.ohci1-controller` |
| `sgpio` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `irtx` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `gpadc0-24m` | 24.00 | — | n/a — no max exposed | 2 | Y | `2522000.ths` |
| `spi4` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `spi3` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `spif` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `spi2` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `spi1` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `smhc3` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `smhc2` | 0.80 | — | n/a — gated | 0 | N | `4022000.sdmmc` |
| `smhc1` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `nand0-clk1` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `nand0-clk0` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `avs` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `timer9` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `timer8` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `timer7` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `timer6` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `timer5` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `timer4` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `timer3` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `timer2` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `timer1` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `timer0` | 6.00 | — | n/a — gated | 0 | N | `deviceless` |
| `trace` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `apb-uart` | 24.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `uart6` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `uart5` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `uart4` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `uart3` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `uart2` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `uart1` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `uart0` | 24.00 | — | n/a — no max exposed | 1 | Y | `uart@2500000` |
| `apb1` | 24.00 | — | n/a — no max exposed | 2 | Y | `2000000.pinctrl` |
| `twi12` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `twi11` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `twi10` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `twi9` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `twi8` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `twi7` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `twi6` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `twi5` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `twi4` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `twi3` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `twi2` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `twi1` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `twi0` | 24.00 | — | n/a — no max exposed | 1 | Y | `2510000.twi` |
| `dcxo26M` | 26.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `dcxo` | 26.00 | — | n/a — no max exposed | 62 | Y | `5520000.hdmi0` |
| `ahbs-auto-clk` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `cpu-icache-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `tt-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `vdd-ddr` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `vdd-sys2cpus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `vdd-sys2usb` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `vdd-usb2cpus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `r-cpucfg` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `riscv` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `riscv-cfg` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `riscv-24m` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `rtc` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `r-irrx-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `r-cpus-bist` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `r-tzma` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `r-ppu` | 26.00 | — | n/a — no max exposed | 1 | Y | `7060000.pck-600:power-controller` |
| `r-mbox` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `r-spi-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `r-spi` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `r-pwm` | 26.00 | — | n/a — no max exposed | 1 | Y | `7023000.pwm` |
| `r-bus-pwm` | 26.00 | — | n/a — no max exposed | 1 | Y | `7023000.pwm` |
| `r-twd` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `r-timer` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `r-timer3` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `r-timer2` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `r-timer1` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `r-timer0` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-de-4x-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-de-3x-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-npu-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-audio1-4x-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-audio1-div2-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-audio1-div5-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-ve0-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-ve1-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-gpu0-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-video0-4x-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-video1-4x-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-video2-4x-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-video0-3x-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-video1-3x-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-video2-3x-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri1-200m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri1-400m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri1-400m-all-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri1-150m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri1-300m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri1-300m-all-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri1-160m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri1-480m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri1-480m-all-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri1-600m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri1-600m-all-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri1-800m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-200m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-400m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-400m-all-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-150m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-300m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-300m-all-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-160m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-480m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-480m-all-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-600m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-800m-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-2x-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-ddr-auto` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `fanout-24m` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `fanout-12m` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `fanout-16m` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `fanout-25m` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `res-dcap-24m` | 26.00 | — | n/a — no max exposed | 4 | Y | `4200400.ohci1-controller` |
| `csi-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `dsc` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `ledc-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `dpss-top1` | 26.00 | — | n/a — no max exposed | 1 | Y | `5510000.vo1` |
| `dpss-top0` | 26.00 | — | n/a — gated | 0 | N | `5500000.vo0` |
| `hdmi` | 26.00 | — | n/a — no max exposed | 1 | Y | `5520000.hdmi0` |
| `edp` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `tcontv1` | 26.00 | — | n/a — gated | 0 | N | `5731000.tcon4` |
| `tcontv0` | 26.00 | — | n/a — no max exposed | 1 | Y | `5730000.tcon3` |
| `dsi1-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `dsi0-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `vo0-tconlcd2-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `vo0-tconlcd1-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `vo0-tconlcd0-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `gmac1` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `gmac0` | 26.00 | — | n/a — no max exposed | 1 | Y | `4500000.ethernet` |
| `usb1-ohci` | 26.00 | — | n/a — no max exposed | 1 | Y | `4200400.ohci1-controller` |
| `usb1-ehci` | 26.00 | — | n/a — no max exposed | 1 | Y | `4200000.ehci1-controller` |
| `usb1` | 26.00 | — | n/a — no max exposed | 1 | Y | `4200400.ohci1-controller` |
| `usb0-ohci` | 26.00 | — | n/a — gated | 0 | N | `4101400.ohci0-controller` |
| `usb0-ehci` | 26.00 | — | n/a — gated | 0 | N | `4101000.ehci0-controller` |
| `usb0-device` | 26.00 | — | n/a — no max exposed | 1 | Y | `4100000.udc-controller` |
| `usb` | 26.00 | — | n/a — gated | 0 | N | `4101400.ohci0-controller` |
| `dmic-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `owa-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `i2spcm4-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `i2spcm3-bus` | 26.00 | — | n/a — no max exposed | 1 | Y | `i2s3_plat@2535000` |
| `i2spcm2-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `i2spcm1-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `i2spcm0-bus` | 26.00 | — | n/a — no max exposed | 1 | Y | `i2s0_plat@2532000` |
| `lpc-gate` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `sgpio-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `lradc` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `irtx-gate` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `irrx-gate` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `ths0` | 26.00 | — | n/a — no max exposed | 1 | Y | `2522000.ths` |
| `gpadc0` | 26.00 | — | n/a — no max exposed | 1 | Y | `2521000.gpadc` |
| `spi4-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `spi3-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `spif-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `spi2-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `spi1-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `spi0-bus` | 26.00 | — | n/a — no max exposed | 1 | Y | `2540000.spi` |
| `ufs` | 26.00 | — | n/a — no max exposed | 1 | Y | `4520000.ufs` |
| `smhc3-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `smhc2-gate` | 26.00 | — | n/a — gated | 0 | N | `4022000.sdmmc` |
| `smhc1-gate` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `smhc0-gate` | 26.00 | — | n/a — no max exposed | 1 | Y | `4020000.sdmmc` |
| `nand0-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `dram0-gate` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `gpu0-gate` | 26.00 | — | n/a — gated | 0 | N | `gpu@1800000` |
| `npu-gate` | 26.00 | — | n/a — gated | 0 | N | `npu@3600000` |
| `ce-bus` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `ce-sys` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `ve-enc0-bus` | 26.00 | — | n/a — gated | 0 | N | `1c10000.ve2` |
| `ve-dec-gate` | 26.00 | — | n/a — gated | 0 | N | `1c0e000.ve` |
| `eink-gate` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `g2d-gate` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `di-gate` | 26.00 | — | n/a — gated | 0 | N | `deinterlace@5400000` |
| `de0-gate` | 26.00 | — | n/a — no max exposed | 1 | Y | `5000000.de` |
| `timer-bus` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `sysdap` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `dbgsys` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pwm1` | 26.00 | — | n/a — no max exposed | 1 | Y | `2528000.pwm` |
| `pwm0` | 26.00 | — | n/a — no max exposed | 1 | Y | `2527000.pwm` |
| `msgbox0` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `spinlock` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `dma1` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `dma0` | 26.00 | — | n/a — no max exposed | 1 | Y | `4601000.dma-controller` |
| `dma0-mclk` | 26.00 | — | n/a — no max exposed | 1 | Y | `4601000.dma-controller` |
| `ve-mclk` | 26.00 | — | n/a — gated | 1 | N | `1c10000.ve2` |
| `ce-mclk` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `dma1-mclk` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `nand-mclk` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `csi-mclk` | 26.00 | — | n/a — gated | 1 | N | `deviceless` |
| `isp-mclk` | 26.00 | — | n/a — gated | 1 | N | `deviceless` |
| `gmac0-mclk` | 26.00 | — | n/a — no max exposed | 1 | Y | `4500000.ethernet` |
| `gmac1-mclk` | 26.00 | — | n/a — gated | 0 | N | `deviceless` |
| `ve-dec-mclk` | 26.00 | — | n/a — gated | 1 | N | `1c0e000.ve` |
| `iommu0-mbus-gate` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `iommu1-mbus-gate` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `desys-mbus-gate` | 26.00 | — | n/a — no max exposed | 2 | Y | `5000000.de` |
| `ve-mbus-gate` | 26.00 | — | n/a — gated | 1 | N | `1c10000.ve2` |
| `ve-dec-mbus-gate` | 26.00 | — | n/a — gated | 1 | N | `1c0e000.ve` |
| `gpu0-mbus-gate` | 26.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `npu-mbus-gate` | 26.00 | — | n/a — gated | 1 | N | `npu@3600000` |
| `vid-in-mbus-gate` | 26.00 | — | n/a — gated | 1 | N | `deviceless` |
| `serdes-mbus-gate` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `msilite0-mbus-gate` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `store-mbus-gate` | 26.00 | — | n/a — no max exposed | 3 | Y | `4520000.ufs` |
| `msilite2-mbus-gate` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `ve-ahb-gate` | 26.00 | — | n/a — gated | 1 | N | `1c0e000.ve` |
| `ve-enc-ahb-gate` | 26.00 | — | n/a — gated | 1 | N | `1c10000.ve2` |
| `vid-in-ahb-gate` | 26.00 | — | n/a — gated | 1 | N | `deviceless` |
| `vid-out0-ahb-gate` | 26.00 | — | n/a — gated | 1 | N | `5500000.vo0` |
| `vid-out1-ahb-gate` | 26.00 | — | n/a — no max exposed | 2 | Y | `5510000.vo1` |
| `de-ahb-gate` | 26.00 | — | n/a — no max exposed | 2 | Y | `5000000.de` |
| `npu-ahb-gate` | 26.00 | — | n/a — gated | 1 | N | `npu@3600000` |
| `gpu0-ahb-gate` | 26.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `serdes-ahb-gate` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `usb-sys-ahb-gate` | 26.00 | — | n/a — no max exposed | 3 | Y | `4200400.ohci1-controller` |
| `msilite0-ahb-gate` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `store-ahb-gate` | 26.00 | — | n/a — no max exposed | 3 | Y | `4520000.ufs` |
| `cpus-hclk-gate` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `iommu1-sys-mclk` | 26.00 | — | n/a — no max exposed | 1 | Y | `3900000.iommu` |
| `iommu1-sys-pclk` | 26.00 | — | n/a — no max exposed | 1 | Y | `3900000.iommu` |
| `iommu1-sys-hclk` | 26.00 | — | n/a — no max exposed | 1 | Y | `3900000.iommu` |
| `msi-lite2` | 26.00 | — | n/a — no max exposed | 3 | Y | `4200400.ohci1-controller` |
| `msi-lite1` | 26.00 | — | n/a — no max exposed | 2 | Y | `4520000.ufs` |
| `msi-lite0` | 26.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `iommu0-sys-mclk` | 26.00 | — | n/a — no max exposed | 1 | Y | `3900000.iommu` |
| `iommu0-sys-pclk` | 26.00 | — | n/a — no max exposed | 1 | Y | `3900000.iommu` |
| `iommu0-sys-hclk` | 26.00 | — | n/a — no max exposed | 1 | Y | `3900000.iommu` |
| `nsi-cfg` | 26.00 | — | n/a — no max exposed | 1 | Y | `2020000.nsi-controller` |
| `its-pcie0-aclk` | 26.00 | — | n/a — no max exposed | 1 | Y | `6000000.pcie` |
| `pll-ref` | 24.00 | — | n/a — no max exposed | 4 | Y | `deviceless` |
| `r-apbs1` | 24.00 | — | n/a — no max exposed | 3 | Y | `deviceless` |
| `r-twi0` | 24.00 | — | n/a — no max exposed | 1 | Y | `7083000.twi` |
| `r-twi1` | 24.00 | — | n/a — no max exposed | 1 | Y | `7084000.twi` |
| `r-twi2` | 24.00 | — | n/a — no max exposed | 1 | Y | `7085000.twi` |
| `r-uart0` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `r-uart1` | 24.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-de` | 1200.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `pll-de-3x` | 600.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `de0` | 600.00 | — | n/a — no max exposed | 1 | Y | `5000000.de` |
| `pll-de-4x` | 600.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-npu` | 1008.00 | 1008.00 | no (at max) | 0 | N | `npu@3600000` |
| `npu` | 1008.00 | 1008.00 | no (at max) | 0 | N | `3600000.npu` |
| `pll-audio1` | 3072.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-audio1-div5` | 614.40 | — | n/a — no max exposed | 0 | Y | `i2s3_plat@2535000` |
| `i2spcm3` | 24.58 | — | n/a — gated | 0 | N | `i2s3_plat@2535000` |
| `i2spcm0` | 24.58 | — | n/a — gated | 0 | N | `i2s0_plat@2532000` |
| `pll-audio1-div2` | 1536.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-audio0-4x` | 98.29 | — | n/a — gated | 0 | N | `i2s3_plat@2535000` |
| `dmic` | 98.29 | — | n/a — gated | 0 | N | `deviceless` |
| `owa-tx` | 98.29 | — | n/a — gated | 0 | N | `deviceless` |
| `i2spcm4` | 98.29 | — | n/a — gated | 0 | N | `deviceless` |
| `i2spcm2-asrc` | 98.29 | — | n/a — gated | 0 | N | `deviceless` |
| `i2spcm2` | 98.29 | — | n/a — gated | 0 | N | `deviceless` |
| `i2spcm1` | 98.29 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-ve1` | 696.00 | — | n/a — gated | 0 | N | `1c10000.ve2` |
| `ve-enc0` | 696.00 | — | n/a — gated | 0 | N | `1c10000.ve2` |
| `pll-ve0` | 546.00 | — | n/a — gated | 0 | N | `1c0e000.ve` |
| `ve-dec` | 546.00 | — | n/a — gated | 0 | N | `1c0e000.ve` |
| `pll-video2` | 2376.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-video2-3x` | 792.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-video2-4x` | 1188.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `isp` | 297.00 | — | n/a — gated | 0 | N | `deviceless` |
| `csi` | 297.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-video1` | 2376.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-video1-3x` | 792.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-video1-4x` | 1188.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-video0` | 2376.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-video0-3x` | 792.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `lpc` | 792.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-video0-4x` | 1188.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `clk27m_fanout` | 1188.00 | — | n/a — gated | 0 | N | `deviceless` |
| `hdmi-tv` | 1188.00 | — | n/a — gated | 0 | N | `deviceless` |
| `edp-tv` | 1188.00 | — | n/a — gated | 0 | N | `5731000.tcon4` |
| `combphy1` | 1188.00 | — | n/a — gated | 0 | N | `deviceless` |
| `combphy0` | 1188.00 | — | n/a — gated | 0 | N | `deviceless` |
| `vo0-tconlcd2` | 1188.00 | — | n/a — gated | 0 | N | `deviceless` |
| `vo0-tconlcd1` | 1188.00 | — | n/a — gated | 0 | N | `deviceless` |
| `vo0-tconlcd0` | 1188.00 | — | n/a — gated | 0 | N | `deviceless` |
| `eink-panel` | 1188.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-gpu` | 1104.00 | 1104.00 | no (at max) | 0 | N | `gpu@1800000` |
| `gpu0` | 1104.00 | 1104.00 | no (at max) | 0 | N | `gpu@1800000` |
| `pll-peri1` | 2400.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `pll-peri1-480m` | 480.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-peri1-160m` | 160.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-peri1-800m` | 800.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-peri1-2x` | 1200.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `pll-peri1-400m` | 400.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `smhc0` | 400.00 | — | n/a — no max exposed | 1 | Y | `4020000.sdmmc` |
| `pll-peri1-200m` | 200.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-peri1-600m` | 600.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-peri1-300m` | 300.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-peri1-150m` | 150.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-peri0` | 2400.00 | — | n/a — no max exposed | 2 | Y | `deviceless` |
| `pll-peri0-480m` | 480.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `ufs-cfg` | 19.20 | — | n/a — no max exposed | 1 | Y | `4520000.ufs` |
| `eink` | 480.00 | — | n/a — gated | 0 | N | `deviceless` |
| `gic` | 480.00 | — | n/a — no max exposed | 0 | Y | `2020000.nsi-controller` |
| `pll-peri0-160m` | 160.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-peri0-16m` | 16.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-peri0-800m` | 800.00 | — | n/a — no max exposed | 0 | Y | `gpu@1800000` |
| `pll-peri0-2x` | 1200.00 | — | n/a — no max exposed | 2 | Y | `deviceless` |
| `hdmi-cec-clk32k` | 0.03 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-400m` | 400.00 | — | n/a — no max exposed | 2 | Y | `gpu@1800000` |
| `pcie0-axi-slv` | 400.00 | — | n/a — no max exposed | 1 | Y | `6000000.pcie` |
| `ce` | 400.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `cpu-peri` | 100.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-peri0-200m` | 200.00 | — | n/a — no max exposed | 3 | Y | `gpu@1800000` |
| `r-apbs0` | 100.00 | — | n/a — no max exposed | 1 | Y | `7025000.pinctrl` |
| `r-ahb` | 200.00 | — | n/a — no max exposed | 3 | Y | `deviceless` |
| `dcxo-serdes1` | 200.00 | — | n/a — no max exposed | 1 | Y | `6c00000.serdes` |
| `dcxo-serdes0` | 200.00 | — | n/a — no max exposed | 1 | Y | `6c00000.serdes` |
| `rtc-spi` | 200.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `dcxo-ufs-gating` | 200.00 | — | n/a — no max exposed | 1 | Y | `4520000.ufs` |
| `owa-rx` | 200.00 | — | n/a — gated | 0 | N | `deviceless` |
| `ufs-axi` | 200.00 | — | n/a — no max exposed | 1 | Y | `4520000.ufs` |
| `pll-peri0-600m` | 600.00 | — | n/a — no max exposed | 4 | Y | `gpu@1800000` |
| `serdes-phy-cfg` | 100.00 | — | n/a — no max exposed | 2 | Y | `6c00000.serdes` |
| `di` | 600.00 | — | n/a — gated | 0 | N | `deinterlace@5400000` |
| `mbus` | 600.00 | — | n/a — no max exposed | 1 | Y | `2020000.nsi-controller` |
| `nsi` | 600.00 | — | n/a — no max exposed | 1 | Y | `a020000.dmcfreq` |
| `apb0` | 100.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `clk-fanout` | 100.00 | — | n/a — gated | 0 | N | `deviceless` |
| `ahb` | 200.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `bus_debug` | 200.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `pll-peri0-300m` | 300.00 | — | n/a — no max exposed | 3 | Y | `gpu@1800000` |
| `hdcp-esm` | 300.00 | — | n/a — no max exposed | 1 | Y | `5520000.hdmi0` |
| `g2d` | 300.00 | — | n/a — gated | 0 | N | `deviceless` |
| `pll-peri0-150m` | 150.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `pll-peri0-25m` | 25.00 | — | n/a — no max exposed | 0 | Y | `deviceless` |
| `gmac1-phy` | 150.00 | — | n/a — gated | 0 | N | `deviceless` |
| `gmac0-phy` | 25.00 | — | n/a — no max exposed | 1 | Y | `4500000.ethernet` |
| `pll-ddr` | 2400.00 | 2400.00 | no (at max) | 0 | Y | `2020000.nsi-controller` |
| `sdram` | 2400.00 | 2400.00 | no (at max) | 0 | Y | `a020000.dmcfreq` |
| `dram0` | 600.00 | — | n/a — no max exposed | 0 | Y | `2020000.nsi-controller` |
| `pll-cpu-dsu` | 1027.00 | 1352.00 | **YES** (see N1) | 2 | Y | `deviceless` |
| `cpu_dsu` | 1027.00 | 1352.00 | **YES** (see N1) | 1 | Y | `deviceless` |
| `pll-cpu-b` | 2002.00 | 2002.00 | no (at max) | 2 | Y | `cpu6` |
| `cpu_b` | 2002.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `pll-cpu-l` | 1794.00 | 1794.00 | no (at max) | 2 | Y | `cpu0` |
| `cpu_l` | 1794.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `pll-cpu-back` | 1924.00 | — | n/a — no max exposed | 1 | Y | `deviceless` |
| `dcxo24M-div32k` | 0.03 | — | n/a — gated | 0 | N | `deviceless` |
| `dcxo-wakeup` | 26.00 | — | n/a — no max exposed | 1 | Y | `4520000.ufs` |
| `dcxo24M` | 24.00 | — | n/a — no max exposed | 0 | Y | `2000000.pinctrl` |
| `dcxo19_2M` | 19.20 | — | n/a — no max exposed | 0 | Y | `deviceless` |

**N1 — the DSU/L3 below-max case, and why it should be left alone.** The DSU OPP table
(`/proc/device-tree/dsu-opp-table/`) tops out at 1352 MHz but `dsu-clk.dtbo` pins
`/dsufreq@0` `assigned-clock-rates = <1032000000>` at boot (the clock framework lands on 1027 MHz), and
`CONFIG_AW_SUNXI_DSUFREQ is not set`, so **nothing scales it**. This is not an oversight: prior measured
work swept it (780 → 1027 → 1196 MHz) and found **1196 MHz bought nothing further** (all metrics within
noise), so 1027 was kept as the knee. Raising it further is technically possible (see §e/E1) with no
measured upside and unknown voltage headroom.

---

## (e) Disabled / below-max, and what would enable each

### Power domains (`sudo cat /sys/kernel/debug/pm_genpd/pm_genpd_summary`)

| domain | status |
|---|---|
| `pd_vo1` | on |
| `pd_vo` | off-0 |
| `pd_de_sys` | on |
| `pd_usb2` | on |
| `pd_npu` | off-0 |
| `pd_pcie` | on |
| `pd_ve_enc` | off-0 |
| `pd_ve_dec` | off-0 |
| `pd_vi` | off-0 |

* **No domain is OFF while having an active consumer.** `pd_ve_dec`, `pd_ve_enc`, `pd_npu`, `pd_vi`,
  `pd_vo` are off and every device under them is `suspended` — correct.
* **`pd_gpu_core` and `pd_gpu_top` are missing entirely.** Their `pdtest` devices exist but failed to
  probe: `pdtest soc@3000000:pd_gpu_core_test@0: deferred probe timeout, ignoring dependency` →
  `error -110`, same for `pd_gpu_top`. **Consequence: the GPU is never power-gated** — only the
  `gpu0-gate` clock is, so idle GPU power cannot reach zero. This is a DT/driver bring-up gap, not a
  tunable.

### Enableable / worth acting on

| # | item | now | could be | how to enable | risk / verdict |
|---|---|---|---|---|---|
| **E3** | **CPU idle clock** | floor pinned at max 1794/2002 permanently | 416 MHz at idle | `sudo /usr/local/sbin/cpu-mode.sh eco` (or `systemctl disable --now cpu-boost.service`). Revert: `sudo /usr/local/sbin/cpu-mode.sh auto` | **the only change with a clear payoff** — restores idle power/quiet. Cost: prior measurement −29 % on CPU-bound work (schedutil alone ramps to only 1.2–1.4 GHz on bursty GL threads). Use `balanced` to keep the big cores always ready. |
| E1 | DSU / L3 clock | 1027 MHz | 1196 or 1352 MHz | `clkctl.ko` is **not loaded**; `sudo insmod /home/radxa/clkctl/clkctl.ko` then `echo 1196000000 > /sys/kernel/debug/clkctl/dsu`; or change `dsu-clk.dtbo` `assigned-clock-rates` + `sudo u-boot-update` + reboot | unknown DSU voltage; prior sweep says **no gain**; runtime write can hang the fabric. Not recommended. |
| E2 | NPU clock | 1008 MHz (devfreq max) | DT `npu-opp-table` has `opp-1120` = 1120 MHz | devfreq `available_frequencies` omits 1120, so it needs a direct clock write; NPU rail `axp8191-dcdc2` is 800 mV | **do not touch** — NPU is another agent's domain and voltage headroom at 1120 is unproven. |
| E4 | GPU clock | **1104 MHz** = the clock generator's measured ceiling (requests 1152/1200/1296/1392 all clamp to 1104) | cannot go higher | — | already at max; `assigned-clock-rates` 1032 on the GPU node is overridden by `clk_rate = <1104000000>` in `gpu-clk.dtbo` |
| E5 | VE clocks | `ve0`/`pll-ve0` 546 MHz, `ve1`/`pll-ve1` 696 MHz, all gated (`hw=N`, `pd_ve_*` off) | — | — | **out of scope by instruction** (concurrent VE access crashes the kernel) |
| E6 | `vid-out0-ahb-gate` | `en=1` but `hw=N` while `5500000.vo0` is suspended and `pd_vo` is off | — | — | possible leaked clock reference on vo0 (enable_count held with no active consumer). Cosmetic; untested. |

### Key PMIC rails (`sudo cat /sys/kernel/debug/regulator/regulator_summary`)

| rail | consumer | current | notes |
|---|---|---|---|
| `axp8191-dcdc5` | `cpu0-cpu` (little cluster) | **950 mV**, consumer min=max=950 | fixed — no per-OPP voltage observed |
| `axp8191-dcdc3` | `cpu6-cpu` (big cluster) | **1000 mV**, consumer min=max=1000 | fixed |
| `axp8191-dcdc4` | `1800000.gpu-gpu` | **990 mV**, consumer min=990 max=1540 | overlay raised the floor (OPP: 1008 MHz needs 960 mV default / 860 mV best bin) |
| `axp8191-dcdc2` | `3600000.npu-npu` | **800 mV** | also feeds HDMI in the tree |
| `vdd_sys` | `a020000.dmcfreq-vddcore` | **900 mV**, min=max=900 | |

All rails report `0 mA` — the PMIC has no shunt here, so **no watts are measurable**. Full rail list:

| regulator | use | opmode | cur mV | min mV | max mV |
|---|---|---|---|---|---|
| `regulator-dummy` | 31 | unknown | 0 | 0 | 0 |
| `axp8191-dcdc1` | 4 | unknown | 3300 | 1000 | 3800 |
| `axp8191-dc1sw1` | 1 | unknown | 3300 | 0 | 0 |
| `axp8191-dc1sw2` | 5 | unknown | 3300 | 0 | 0 |
| `axp8191-dcdc2` | 3 | unknown | 800 | 500 | 1540 |
| `axp8191-dcdc3` | 2 | unknown | 1000 | 500 | 1540 |
| `axp8191-dcdc4` | 2 | unknown | 990 | 990 | 1540 |
| `axp8191-dcdc5` | 2 | unknown | 950 | 500 | 1540 |
| `axp8191-dcdc6` | 1 | unknown | 560 | 500 | 2760 |
| `axp8191-dcdc7` | 1 | unknown | 1080 | 500 | 1540 |
| `axp8191-dcdc8` | 3 | unknown | 1200 | 500 | 3400 |
| `axp8191-dcdc9` | 1 | unknown | 1240 | 500 | 3400 |
| `axp8191-aldo1` | 1 | unknown | 3300 | 500 | 3400 |
| `axp8191-aldo2` | 0 | unknown | 2800 | 500 | 3400 |
| `axp8191-aldo3` | 0 | unknown | 2800 | 500 | 3400 |
| `axp8191-aldo4` | 0 | unknown | 2800 | 500 | 3400 |
| `axp8191-aldo5` | 1 | unknown | 3300 | 500 | 3400 |
| `axp8191-aldo6` | 0 | unknown | 1800 | 500 | 3400 |
| `axp8191-bldo1` | 1 | unknown | 1800 | 500 | 3400 |
| `axp8191-bldo2` | 0 | unknown | 1800 | 500 | 3400 |
| `axp8191-bldo3` | 0 | unknown | 1200 | 500 | 3400 |
| `axp8191-bldo4` | 0 | unknown | 1800 | 500 | 3400 |
| `axp8191-bldo5` | 0 | unknown | 1800 | 500 | 3400 |
| `axp8191-cldo1` | 1 | unknown | 1800 | 500 | 3400 |
| `axp8191-cldo2` | 1 | unknown | 1800 | 500 | 3400 |
| `axp8191-cldo3` | 2 | unknown | 1800 | 500 | 3400 |
| `axp8191-cldo4` | 0 | unknown | 1800 | 500 | 3400 |
| `axp8191-cldo5` | 4 | unknown | 1800 | 500 | 3400 |
| `axp8191-dldo1` | 1 | unknown | 1800 | 500 | 3400 |
| `axp8191-dldo2` | 0 | unknown | 3300 | 500 | 3400 |
| `axp8191-dldo3` | 0 | unknown | 2800 | 500 | 3400 |
| `axp8191-dldo4` | 0 | unknown | 3300 | 500 | 3400 |
| `axp8191-dldo5` | 0 | unknown | 2800 | 500 | 3400 |
| `axp8191-dldo6` | 2 | unknown | 2500 | 500 | 3400 |
| `axp8191-eldo1` | 1 | unknown | 1075 | 500 | 1500 |
| `axp8191-eldo2` | 1 | unknown | 800 | 500 | 1500 |
| `axp8191-eldo3` | 0 | unknown | 1200 | 500 | 1500 |
| `axp8191-eldo4` | 0 | unknown | 1200 | 500 | 1500 |
| `axp8191-eldo5` | 0 | unknown | 1200 | 500 | 1500 |
| `axp8191-eldo6` | 1 | unknown | 800 | 500 | 1500 |
| `axp8191-rtcldo` | 1 | unknown | 1800 | 1800 | 1800 |
| `vdd_sys` | 1 | unknown | 900 | 900 | 900 |
| `usb0-vbus` | 2 | unknown | 5000 | 5000 | 5000 |
| `usb1-vbus` | 1 | unknown | 5000 | 5000 | 5000 |
| `wifi_power_en` | 1 | unknown | 3300 | 3300 | 3300 |
| `wifi_chip_en` | 1 | unknown | 3300 | 3300 | 3300 |
| `eeprom_wp` | 1 | unknown | 3300 | 3300 | 3300 |

---

## (f) Loss analysis — where the cycles go on the CPU-saturated GL client path

**The named workload cannot be reproduced on this session's stack, and I did not fake it.** The
102 %-of-one-core / 62.5 %-in-kernel figure belongs to the **open** PowerVR driver with
weston + Xwayland. The GPU is currently bound to `pvrsrvkm` (vendor) and the instructions forbid
touching the driver binding, so re-running the open arm is out of scope. What is available was measured.

### Measured now: vendor driver + kwin_x11 (live desktop)

```
# /proc/<pid>/stat fields 14/15 (utime/stime), USER_HZ=100, 15 s window
glmark2-es2 --off-screen --benchmark refract
```

| process | user | sys | total | % of one core |
|---|---|---|---|---|
| `glmark2-es2` client | 0.50 s | 0.46 s | 0.96 s | 6.4 % |
| `kwin_x11` (X server) | 0.09 s | 0.04 s | 0.13 s | 0.9 % |
| **TOTAL** | | | **1.09 s** | **7.3 %** |
| kernel share | | **0.50 s** | | **45.9 % of CPU time (3.3 % of wall)** |

The vendor path is **not** CPU-saturated — the saturation is a property of the open stack, not of "a GL
client" in general. That alone is a result: whatever costs 102 % of a core on the open driver is not
inherent to running GL on this SoC.

### Syscall classes (the client, `strace -c -f -o`)

```
DISPLAY=:0 strace -c -f glmark2-es2 --off-screen --benchmark refract
```

| % time | syscall | calls | note |
|---|---|---|---|
| **47.2 %** | **`futex`** | 11989 | thread/GPU-block synchronisation |
| **31.8 %** | **`ioctl`** | 9061 | the driver's submit/block/wait path |
| **13.5 %** | `ppoll` | 16912 | event waits |
| 3.6 % | `close` | 5168 | |
| 1.0 % | `dup` | 2242 | |

*Counts* are meaningful; the *times* are inflated by ptrace trapping every syscall, so read this as
"futex + ioctl dominate the client's kernel entries", not as an exact split. That still points the same
way as the prior open-stack decomposition: the recoverable cost is **synchronisation and driver-call
overhead**, not pixel work.

### The prior open-path decomposition (recorded, not re-measured)

Summing `/proc/<pid>/stat` over a 40 s, 640×480 glmark2-es2 window on weston + Xwayland:

| process | user | sys | total | share of wall |
|---|---|---|---|---|
| client | 2670 ms | 2760 ms | 5430 ms | 27.1 % |
| **Xwayland** | 5020 ms | **9510 ms** | 14530 ms | **72.6 %** |
| weston | 280 ms | 240 ms | 520 ms | 2.6 % |
| **TOTAL** | | **12510 ms** | 20480 ms | **102.4 % of one core** |
| | | | | **62.5 % of wall in the kernel** |

So on the open stack **76 % of the kernel time is Xwayland** (X11 protocol + dmabuf/DRI3 present copies),
not the GPU driver. Source: `Recovery/GPU-FIRMWARE-RE-2026-10-06.md` §"the 84 % figure DOES hold".

### What cannot be determined, and what would be needed

* **Per-symbol kernel attribution is impossible here.** `perf` is **absent**, `perf_event_paranoid=2`,
  `# CONFIG_SCHEDSTATS is not set`, `# CONFIG_LATENCYTOP is not set`, and `kprobe_events` is reported to
  arm but never fire on this kernel (`CONFIG_KPROBES=y`, `CONFIG_KPROBE_EVENTS=y`).
* To get a real kernel-path breakdown you would need **one** of: a `perf` userspace matching 6.6.98-5
  (and `perf_event_paranoid≤1` for system-wide), a working ftrace/kprobe path on this vendor kernel, or
  instrumentation in Xwayland/the driver. Until then the honest statement is: **the kernel share is
  measured (62.5 % of wall; futex+ioctl dominated on the client, Xwayland-dominated overall); the
  specific kernel functions are not.**

---

## (g) Unknowns

| # | unknown | why | what would resolve it |
|---|---|---|---|
| U1 | L2/L3 cache sizes and the true big/little cache topology | no `cache-size` in the DT, the DTBs, or `/usr/src`; sysfs omits `size`; both clusters report one shared L2 | vendor DTS, or `perf`-free CLIDR dump via a kernel module |
| U2 | DRAM part number and exact type | only `dram_clk:2400` is kernel-visible; the part ID is in the SPL log | bootloader log / board schematic |
| U3 | Power in watts for CPU/GPU/NPU/DDR | the AXP8191 rails report `0 mA` (no shunt) | external meter or PMIC register decoding |
| U4 | Whether CPU voltage steps per OPP | observed rails are fixed (950 mV little / 1000 mV big) but the boost pins the floor at max, so a low-frequency OPP was never sampled | read `regulator_summary` with the floor released at 416 MHz |
| U5 | NPU 1120 MHz feasibility and its voltage | devfreq omits the point; NPU rail is 800 mV | out of scope by instruction |
| U6 | DSU at 1196–1352 MHz: real gain and voltage headroom | no dsufreq driver; prior 1196 sweep showed no gain | direct DSU sweep via `clkctl.ko` (unloaded) with a stability watch |
| U7 | Why `pd_gpu_core`/`pd_gpu_top` fail to probe (-110) | `pdtest` deferred-probe timeout, no further messages | vendor pdtest driver source |
| U8 | Whether `vid-out0-ahb-gate`'s held reference is a leak | `en=1 hw=N` with `vo0` suspended and `pd_vo` off | `clk_disable_unused` / debugfs clk refcount tracing |
| U9 | Big-core preference under >2 runnable tasks | only the 1-task case was sampled (cpu6, 30/30) | EAS trace or a taskset matrix (no schedstat available) |

---

## Reproduce / state

Every command is inline above. Raw dumps used for the tables are in `/tmp/a7a-map/`
(`clk_summary.txt`, `regulator_summary.txt`, `pm_genpd.txt`, `monitor.out`, `stream.c`, `sysbench.csv`,
`ceiling2.csv`, `strace-gl.txt`).

**No setting was changed.** Governor `schedutil` and the max-pinned floors were the pre-existing state
and are unchanged; no clock, voltage or power domain was written; the GPU stayed bound to `pvrsrvkm`
throughout and kwin_x11 was never stopped. Temporary processes (`glxgears`, `glmark2-es2`, `x11perf`,
burner loops, `stream`) were all terminated (verified: none left).
