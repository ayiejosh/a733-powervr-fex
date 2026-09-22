# A733 system bring-up helpers (trixie / 6.6)

Board-level helpers and quirks for the Cubie A7A/A7S (A733) on the **Debian 13 (trixie)
/ kernel 6.6** stack, separate from the GPU/FEX work but part of getting the board usable.

## Desktop on software GL — and the picom XRender compositor

The desktop runs **software-rendered X11** (KDE Plasma). It cannot use the GPU for
compositing: a live GPU compositor on `pvrsrvkm` deadlocks the kernel (see `../kernel/`
and `../docs/FINDINGS.md`). Two pieces keep it usable:

- **`10-software-render.sh`** (`~/.config/plasma-workspace/env/`) sets
  `LIBGL_ALWAYS_SOFTWARE` so the desktop never tries the GPU GL path (which deadlocks
  the board). It is made **X11-only** so it does not poison off-screen/zink/DXVK GPU
  paths that scope their own env. Keep it — it is the board's defense against the hang.
- **picom XRender compositor** — without a GPU compositor the desktop shows hover/repaint
  artifacts. A `picom --backend xrender` (software, **no GPU**) compositor autostarts via
  `~/.config/autostart/picom-xrender.desktop` and fixes the artifacts. No GPU is involved.

## Fan control — `fan-curve.sh` + `fan-curve.service`
Gradual PWM fan curve for the A733 (8-bit PWM at the board's `pwmfan` hwmon, no
tachometer). **OFF below 58 C, linear ramp 58->78 C -> PWM 0->255, 100% at >=78 C.**
Fail-safe trap sets PWM 255 on exit.
```sh
sudo cp fan-curve.sh /usr/local/sbin/ && sudo chmod +x /usr/local/sbin/fan-curve.sh
sudo cp fan-curve.service /etc/systemd/system/ && sudo systemctl enable --now fan-curve
```
> Single-core emulation load holds the big cores at **2002 MHz / ~60 C** with headroom;
> sustained all-core load climbs past 78 C — the fan is required there.
>
> ⚠️ **`release_clamps()` (2026-09-22) is not optional.** Switching a zone to `user_space`
> *freezes* its cooling state, so a non-zero state the kernel applied earlier keeps
> pinning `scaling_max_freq` through the thermal freq-QoS clamp — and that clamp cannot be
> overridden by writing `scaling_max_freq`. Without it the big cores sat at 1716 MHz and
> the little at 1508 MHz (**~14% of the CPU lost**: SHA-256 8-thread 5.78M -> 6.59M).

## FEX binfmt reliability — `binfmt-guard` + `fex-binfmt.service`
`fex-binfmt.service` registers the FEX `binfmt_misc` handlers; `binfmt-guard` self-heals
a corrupted `FEX-x86_64` registration (the magic must end `…02003e00`; a truncated/wrong
magic catches *all* aarch64 ELFs and breaks exec system-wide). The guard uses only shell
builtins (never shell-prints the magic — null-byte truncation is what corrupts it).
```sh
sudo cp binfmt-guard.sh /usr/local/sbin/ && sudo chmod +x /usr/local/sbin/binfmt-guard.sh
sudo cp binfmt-guard.service fex-binfmt.service /etc/systemd/system/
sudo systemctl enable binfmt-guard fex-binfmt
```
> ⚠️ A bad FEX binfmt magic can break **all** binary execution on the board (EACCES,
> can't even run `echo`). The guard exists to prevent/recover that. On trixie FEX is the
> x86 default (`../fex/`), so this guard matters more, not less.

## CPU clocking — `cpu-mode.sh` + `cpu-boost.py` (four profiles, low idle / max on demand)
`schedutil` is the base governor (416 MHz idle) and `cpu-boost.service` raises the
per-cluster frequency **floor** to the hardware maximum while `user.slice` shows real
work — full-speed throughput while work lasts, back to idle ~1 s after it stops. The
demand signal is cgroup v2 CPU accounting on `user.slice`, so background daemons
(syncthing, tailscaled — `system.slice`) can never trigger it; a system-wide guard still
boosts for heavy system jobs (apt/dkms builds). This exists because the kernel cannot
express it otherwise: `CONFIG_UCLAMP_TASK` is not set and neither `schedutil`'s nor
`ondemand`'s tunables are exposed in sysfs, while plain `schedutil` costs **-29%** on
bursty CPU-bound work (a single thread blocked on GPU ioctls only reaches 1.2-1.4 GHz).
```sh
sudo cp cpu-boost.py cpu-mode.sh irq-affinity.sh /usr/local/sbin/ && sudo chmod +x /usr/local/sbin/{cpu-boost.py,cpu-mode.sh,irq-affinity.sh}
sudo cp cpu-boost.service cpu-mode.service irq-affinity.service /etc/systemd/system/
sudo systemctl enable --now cpu-mode cpu-boost irq-affinity
sudo cpu-mode.sh auto|max|balanced|eco     # switch profile at runtime
```
| mode | little / big governor | boost | intent |
|---|---|---|---|
| `auto` *(default)* | schedutil / schedutil | on | 416 MHz idle, max on demand — best of both |
| `max` | performance / performance | off | always max, no dynamics |
| `balanced` | schedutil / performance | off | big cores always ready |
| `eco` | schedutil / schedutil | off | lowest idle power, -29% on bursty work |

> The ceilings are **1794 MHz (little) / 2002 MHz (big)** — reachable, not
> "firmware-locked" as this README used to say. The old `cpu-performance.service`
> (always-`performance`) is superseded and left disabled.

## IRQ affinity — `irq-affinity.sh`
CPU0 was carrying 5.3M interrupts against ~1.1M on its siblings (2.6M `sunxi-gpadc` +
1.5M `pvrsrvkm`), stealing time from whatever ran there. The unit re-applies affinity
each boot (IRQ numbers move): `pvrsrvkm` -> cpu6, `ufshcd` -> cpu7, `sunxi-gpadc` ->
cpu2, `tcon3` -> cpu3.

## Auto-suspend masked
The board's idle auto-suspend was breaking long jobs (it wakes as a fresh boot). The
sleep targets are **masked**. (A `watchdog-pet` + reboot-tracker run alongside to tell a
clean reboot from an abnormal power loss.)

## ⚠️ SD-card / sunxi-mmc software-eject gotcha
Software-ejecting the SD card (or otherwise tearing down the `sunxi-mmc` controller while
mounted/active) triggers a kernel **Oops** in the sunxi-mmc teardown path. Avoid the
software eject; unmount cleanly and prefer a power-off before removing the card.
