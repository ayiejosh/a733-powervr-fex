# A733 system bring-up helpers

Small board-level helpers for the Cubie A7A/A7S (A733), separate from the GPU/FEX
work but part of getting the board usable.

## Fan control — `fan-curve.sh` + `fan-curve.service`
Gradual PWM fan curve for the A733 (8-bit PWM at the board's `pwmfan` hwmon, no
tachometer). Reads thermal zones; **OFF below 58 °C, linear ramp 58→78 °C → PWM
0→255, 100 % at ≥78 °C.** Tune the `LOW`/`HIGH`/`SPAN` (milli-°C) constants. Has a
fail-safe trap that sets PWM 255 on exit.
```sh
sudo cp fan-curve.sh /usr/local/sbin/ && sudo chmod +x /usr/local/sbin/fan-curve.sh
sudo cp fan-curve.service /etc/systemd/system/ && sudo systemctl enable --now fan-curve
```
> Passive (fan off): idle ~50 °C, but sustained all-core load climbs past 78 °C —
> the fan is required under sustained load.

## FEX binfmt reliability — `binfmt-guard.sh` + `binfmt-guard.service` + `fex-binfmt.service`
`fex-binfmt.service` registers the FEX `binfmt_misc` handlers; `binfmt-guard`
self-heals a corrupted `FEX-x86_64` registration (the magic must end `…02003e00`;
a truncated/wrong magic catches *all* aarch64 ELFs and breaks exec system-wide).
The guard uses only shell builtins (never shell-prints the magic — null-byte
truncation is what corrupts it) to remove + re-register cleanly.
```sh
sudo cp binfmt-guard.sh /usr/local/sbin/ && sudo chmod +x /usr/local/sbin/binfmt-guard.sh
sudo cp binfmt-guard.service fex-binfmt.service /etc/systemd/system/
sudo systemctl enable binfmt-guard fex-binfmt
```
> ⚠️ A bad FEX binfmt magic can break **all** binary execution on the board
> (EACCES, can't even run `echo`). The guard exists to prevent/recover that.
