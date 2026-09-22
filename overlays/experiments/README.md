# overlays/experiments/ — kept as evidence, not as recommendations

These were built, installed and measured on the board. **Neither should be installed
unless you are deliberately re-running the experiment** — the two shipping overlays live in
[`../`](../) and are the ones with a measured benefit.

| overlay | what it was for | what it measured |
|---|---|---|
| [`cpu-oc-rejected.dts`](cpu-oc-rejected.dts) | push the CPU past its spec clock: new OPPs at 2100-2400 MHz (big) and 1900-2000 MHz (little) with the vendor's complete `opp-microvolt-vfNNNN` / `26m-vfNNNN` property set, plus raised rail floors | **it does not work on this BSP.** The extra frequencies are never offered — the BSP builds its cpufreq list from the chip's **factory speed grade** (efuse vf bin), not from the DT table — and the reported maximum even moved *down* (big 2002 → 1992 MHz, little 1794 → 1800 MHz) because the added entries perturb the bin matching. Reverted the same day; the board runs the vendor table. |
| [`dsu-clk-1196.dts`](dsu-clk-1196.dts) | ask whether the DSU can go higher than 1027 MHz — identical to `../dsu-clk.dts` but requesting `1196000000` | it can (1196 MHz applied and was stable), but it **bought nothing**: every metric landed within noise of 1027 MHz (`l3read` 11.4-12.9, `l3shared` 13.1-14.1, `dramread` 9.5-13.0, `fex.tcreate` 116988 ns/op, 0 kernel-trouble lines, 62-63 °C). The knee is **~1027 MHz**, so the safer setting ships. |

## The CPU one also produced a real finding about OPP tables

Overlay v1 of `cpu-oc-rejected.dts` omitted the voltage properties. This kernel's OPP core
manages a regulator for the CPU cluster, so it rejects entries with no voltage
information — and it rejects the **whole table**, not just those entries:

```
opp_parse_microvolt: opp-microvolt missing although OPP managing regulators
_of_add_opp_table_v2: Failed to add OPP, -22
OPP table can't be empty
```

Result: `cpufreq` died entirely and the CPU sat pinned at **1014 MHz** — a *slowdown* from a
patch meant to speed it up. That is why the shipped `cpu-oc-rejected.dts` (v2) carries the
full vendor property set, and why any DT OPP work here should be treated as
table-invalidating until proven otherwise.

## Installing one (only to re-measure)

```sh
dtc -@ -I dts -O dtb -o cpu-oc-rejected.dtbo cpu-oc-rejected.dts
sudo cp cpu-oc-rejected.dtbo /boot/dtbo/ && sudo u-boot-update && sudo reboot
# then, to see the wall for yourself:
sudo bench/cpu-oc-sweep.sh
# and to go back:
sudo rm /boot/dtbo/cpu-oc-rejected.dtbo && sudo u-boot-update && sudo reboot
```

`bench/cpu-oc-sweep.sh` deliberately only writes `scaling_max_freq` at runtime, so a crash
mid-sweep reboots into the safe shipped configuration rather than into the experiment.
