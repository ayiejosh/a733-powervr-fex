# overlays/ — device-tree overlays that give this board its clocks back

Two overlays, independent, both applied at boot through U-Boot's `fdtoverlays`
(`/boot/dtbo/` + `sudo u-boot-update`). Neither one touches the bootloader, the kernel
image, or the OS — deleting the `.dtbo` and re-running `u-boot-update` is a complete
rollback.

| overlay | what it fixes | effect |
|---|---|---|
| [`gpu-clk.dts`](gpu-clk.dts) | `pvrsrvkm` reads a plain `clk_rate` off the GPU node and falls back to **600 MHz** when it is absent — Radxa never set it, so the GPU ran 44% below its own OPP table since install | GPU **600 -> 1104 MHz** @ 990 mV (+77..84% on `bench/glbench.c`) |
| [`dsu-clk.dts`](dsu-clk.dts) | the vendor DSU (L3/coherency fabric) scaling driver is **not compiled into this kernel** (`# CONFIG_AW_SUNXI_DSUFREQ is not set`), so the fabric stays at the bootloader's 780 MHz while the CPU clusters run at 1794/2002 MHz | DSU **780 -> 1027 MHz**: `l3read` +19%, `dramread` +38%, FEX thread-start −32% |

Read [`../docs/PERFORMANCE-2026-09-22.md`](../docs/PERFORMANCE-2026-09-22.md) for the
measurements and the comics in [`../docs/comics/`](../docs/comics/) for the plain-language
version.

Two more overlays were built and measured but are **not** recommended — they live in
[`experiments/`](experiments/README.md) as evidence: the CPU overclock attempt (this BSP
ignores added OPPs — it picks frequencies from the chip's factory speed grade) and a
1196 MHz DSU step (stable, but no gain over 1027).

## Install

Compile with the **board's** `dtc` (phandle references in `dsu-clk.dts` come from the
running DT, so a mismatched base tree silently misdirects them):

```sh
dtc -@ -I dts -O dtb -o gpu-clk.dtbo gpu-clk.dts
dtc -@ -I dts -O dtb -o dsu-clk.dtbo dsu-clk.dts
sudo cp gpu-clk.dtbo dsu-clk.dtbo /boot/dtbo/
sudo u-boot-update          # regenerates extlinux.conf's fdtoverlays line
sudo reboot
```

Verify after the reboot:

```sh
sudo dmesg | grep sunxi_parse_dts                  # clk_rate:1104000000
sudo grep -E 'pll-gpu$|pll-cpu-dsu' /sys/kernel/debug/clk/clk_summary
#   pll-gpu            1104000000
#   pll-cpu-dsu        1027000000
```

## Rollback

```sh
sudo rm /boot/dtbo/gpu-clk.dtbo /boot/dtbo/dsu-clk.dtbo
sudo u-boot-update && sudo reboot
```

## Two traps worth knowing (both cost a reboot to find)

1. **`assigned-clock-rates` needs a driver to probe the node.** It is applied by
   `of_clk_set_defaults()` from the driver core, so a node nothing binds to ignores it
   completely. `/dsufreq@0` has no driver -> the first DSU overlay did nothing at all
   (clock stayed 780 MHz). `dsu-clk.dts` therefore also hangs the assignment off the GPU
   node, where `pvrsrvkm` definitely binds. That fragment is the one that works.
2. **A requested rate is not an achieved rate.** `pll-cpu-dsu` settles at 1027 MHz for a
   requested 1032 MHz, and the GPU generator clamps anything above 1104 MHz. Always read
   the rate back (`clk_summary`, or `bench/clkctl/`) instead of trusting the number you
   wrote.

## Safety

Both settings ran sustained mixed-load stability passes with zero kernel-trouble lines
and 58-65 °C (`bench/gpu-stress.sh`-style loops, `fan-curve.service` active). The
bootloader region was verified byte-identical (sha256) before and after every reboot
during this work. The CPU, notably, was **not** overclockable — see
`../docs/PERFORMANCE-2026-09-22.md` §4 — so there is no CPU overlay here on purpose.
