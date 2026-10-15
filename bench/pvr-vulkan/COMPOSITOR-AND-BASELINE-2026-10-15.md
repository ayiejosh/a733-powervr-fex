# Compositor compatibility, and the repo benchmark re-run

2026-10-15, Radxa Cubie A7A, kernel 6.6.98-5-aw2511.

## Which compositor is in use, and whether the open driver can host it

**On the vendor driver:**

```
display-manager: active   ·   session: X11   ·   compositor: kwin_x11
processes : sddm · X · ksmserver · kwin_x11 · plasmashell
GPU users : X (root), kded6, ksmserver, kwin_x11  on /dev/dri/renderD128
```

**On the open driver, the same desktop does NOT come up.** `display-manager` reports `active`, but X fails:

```
sddm: Running: /usr/bin/X -nolisten tcp -background none -seat seat0 vt2 -auth ...
sddm: Failed to read display number from pipe
sddm: Display server stopping...
sddm: Attempt 1 starting the Display server on vt 2 failed
```

**Result: `kwin_x11`, `X` and `plasmashell` are all absent while the unit says "active".**

**What does work on the open driver is Wayland.** Weston came up cleanly earlier the same day
(`xserver listening on display :0`, Xwayland attached, `socket: present`).

**So: the open driver is a Wayland-only proposition today.** It cannot host an X11 + `kwin_x11` desktop,
and `kwin_wayland` on the open stack is UNTESTED — that is the obvious next thing to try if a KDE
session on the open driver is wanted.

## The repo benchmark, re-run against `RESULTS.md`

`ab.sh` with its DEFAULT matrix (no arguments): `vkrender:2048:20 vkrender:512:50 vkheavy:2048:5`
`cstp:64:200 cstpf:64:200 cstpi:64:200`.

| probe | `RESULTS.md` recorded | **2026-10-15** | delta |
|---|---|---|---|
| **vkrender 2048 (render+copy)** | **5.358** | **5.362** | **+0.1%** |
| vkrender 512 (render+copy) | 0.637 | 0.688 | +8% |
| **vkheavy 2048** | **171.7** (open) / 179.8 (vendor) | **171.176 / 180.342** | **-0.3% / +0.3%** |
| **cstp 64** | **447.9** (open) / 372.5 (vendor) | **453.8 / 369.9** | **matches** |
| cstpf 64 | 148.6 / 146.2 | 146.9 / 145.4 | matches |
| cstpi 64 | 151.6 / 147.1 | 141.3 / 142.8 | matches |

**The current numbers reproduce the repo's recorded ones.** **The `vkrender` probe's default mode is
*render+copy*, not *render* — comparing against the *render* row (1.766) makes it look like a 204%
regression, which is a row-selection error, not a regression.**

**This run's open/vendor ratios:** `cstp` open +22%, `vkheavy` open +5%, `vkrender 2048` open +7%,
`vkrender 256` open +4%, `vkrender 1024` vendor +9%, `vkrender 4096` vendor +3%, `vkrender 512`
vendor +10%.

## Caveats

* **`vkrender` sizes below 2048 flip direction between runs** (2048 measured open +8.2%, vendor +3.2%,
  open +7% across three runs). **Only `vkheavy` and `cstp` are stable.**
* **Single-sample ratios from `ab.sh`'s table are not measurements** — its table prints one value per
  probe with no spread, and it has produced misleading ratios four separate times in this work.
