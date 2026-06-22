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
> Single-core emulation load holds 1716 MHz at ~60 C with headroom; sustained all-core
> load climbs past 78 C — the fan is required there.

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

## CPU governor — `performance` (persistent)
A oneshot drop-in sets every cpufreq policy to `performance` at boot
(`/etc/systemd/system/cpu-performance.service`, non-fatal `|| true` so it can't block
boot). Measured throughput gain for sustained jobs is ~0% (ondemand ramps to max under
load); the value is removing ramp-up latency for **bursty** sub-second emulated launches
(the common Wine/desktop case). 1794 MHz is firmware-locked; 1716 MHz is the ceiling.

## Auto-suspend masked
The board's idle auto-suspend was breaking long jobs (it wakes as a fresh boot). The
sleep targets are **masked**. (A `watchdog-pet` + reboot-tracker run alongside to tell a
clean reboot from an abnormal power loss.)

## ⚠️ SD-card / sunxi-mmc software-eject gotcha
Software-ejecting the SD card (or otherwise tearing down the `sunxi-mmc` controller while
mounted/active) triggers a kernel **Oops** in the sunxi-mmc teardown path. Avoid the
software eject; unmount cleanly and prefer a power-off before removing the card.
