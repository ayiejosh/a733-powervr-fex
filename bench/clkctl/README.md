# clkctl/ — sweep clock rates live, without a reboot per frequency

A minimal debugfs module for A733 bring-up testing. It exposes three files:

```
/sys/kernel/debug/clkctl/gpu_clk        GPU clock     (gpu@1800000 "clk")
/sys/kernel/debug/clkctl/gpu_parent     its parent PLL
/sys/kernel/debug/clkctl/dsu            DSU clock     (/dsufreq@0 index 0)
```

Reading returns the **actual** current rate; writing calls `clk_set_rate()`. That is the
whole trick: "one reboot per candidate frequency" becomes a single boot that walks a
table, and the *achieved* rate is read straight back — which is how the GPU generator's
1104 MHz ceiling and the DSU's 1032 -> 1027 MHz rounding were both found.

```sh
make                                   # uses /lib/modules/$(uname -r)/build
sudo insmod clkctl.ko
sudo cat /sys/kernel/debug/clkctl/gpu_clk            # e.g. 600000000
for r in 1008000000 1104000000 1152000000 1200000000 1296000000; do
  echo $r | sudo tee /sys/kernel/debug/clkctl/gpu_clk >/dev/null
  echo "requested $r -> actual $(sudo cat /sys/kernel/debug/clkctl/gpu_clk)"
done
sudo rmmod clkctl
```

Measured on a Cubie A7A (2026-09-22), DSU 1027 MHz, rail 990 mV:

| requested | actual | `glbench` loop4 |
|---|---|---|
| 1008000000 | 1104000000 | 7428 Mpix |
| 1104000000 | **1104000000** | 7428 |
| 1152000000 / 1200000000 / 1248000000 / 1296000000 / 1344000000 / 1392000000 | **all 1104000000** | 7624-7641 |

Zero driver error lines at every point, 58-61 °C.

## Caveats

* **Runtime only.** Writes do not survive a reboot, and the module is not installed by
  anything — that is deliberate. Apply a winning value permanently through
  [`../../overlays/`](../../overlays/) instead.
* **Writes are live.** A bad rate can hang or crash the board. That is why this is
  *safer* than the alternative: a crash during a sweep reboots back into the saved,
  known-good configuration — whereas the same experiment baked into a DT overlay would
  boot straight into the bad state.
* **Not every node accepts every rate.** With both clock overlays applied, a request of
  exactly `1008000000` on `gpu_clk` was observed landing on a lower parent (552 MHz)
  instead — the clock framework picks the parent PLL it likes, not the one you meant.
  Always read the value back.
* `dsu` needs the DSU clock exposed by the SoC's CCU driver; if the read says `-ENODEV`
  the node path or index differs on your BSP (`entries[]` at the top of `clkctl.c`).
