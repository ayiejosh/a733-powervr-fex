# Activating the PRIME-import patch — what it took, and how to revert

`kernel/pvrsrvkm-drm-prime-import.patch` is **our** patch (written for the 5.15 BSP, still applies
to the 6.6 DDK). This directory is the activation tooling for it, and the record of the two things
that went wrong on the way — both of them mine, both worth not repeating.

## Result: applied, loaded at boot, verified

| check | result |
|---|---|
| live module build-id | `5d842b087f075c97bbf5225fb08d39ca3c145235` = patched (stock is `50b99ea6…`) |
| `drmPrimeFDToHandle` on a **foreign** dma-buf | `OK -> handle=1` (was `EINVAL` on stock) |
| re-export handle -> fd | `OK` |
| GPU writes into those foreign pages | `readback: 65536/65536 == 0xCAFEF00D` — `PASS` |
| load at boot | `pvrsrvkm-load.service` -> `pvrsrvkm loaded: 5d842b08…` |

## Trap 1 — xz's default check breaks modprobe (the real bug)

`modprobe` failed with **EINVAL** while `insmod` of the same uncompressed `.ko` worked. Cause:

* the DKMS module ships `Check: CRC32`,
* `xz`'s *default* is `Check: CRC64`,
* this kernel's in-kernel XZ decoder supports **CRC32 only**, so it refuses the stream.

Always compress the way DKMS does:

```sh
xz -c --check=crc32 -6 pvrsrvkm.ko > pvrsrvkm.ko.xz
```

Symptom to recognise: `modprobe: ERROR: could not insert 'pvrsrvkm': Invalid argument`, with nothing
in dmesg, while `insmod` on the uncompressed file succeeds.

## Trap 2 — a "safety" guard that caused the failure it was meant to catch

`prime-guard.service` originally swapped the stock module back in if `pvrsrvkm` was not in
`/proc/modules` shortly after boot. It ran *before* the udev autoload, so it replaced the patched
file a fraction of a second before the module was loaded — and the **stock** build came up instead,
which then looked exactly like "the patched module does not load". It is now **report-only** and
simply states which build-id is live.

Lesson: never let a boot-time safety net *write* to the thing it is guarding; have it report, and
load explicitly.

## Load at boot

Relying on the DT-modalias autoload was not dependable (it silently did nothing for this module).
`pvrsrvkm-load.service` loads it explicitly, before `display-manager.service`, and logs the build-id
it got:

```sh
sudo install -m 644 pvrsrvkm-load.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable pvrsrvkm-load.service
```

## Activate / revert

```sh
./activate.sh     # install the patched module (CRC32-compressed) + depmod
sudo reboot
./revert.sh       # put the stock module back
sudo reboot
```

Acceptance test (any time after boot):

```sh
sudo env LD_LIBRARY_PATH=/usr/local/lib VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json \
  /home/radxa/prime-build/staged/dmabuf_foreign_test
```

## What this enables, honestly

Nothing user-visible *yet*: the paths that need foreign dma-buf import (kmsro, wlroots, any
compositor that scans out its own GPU buffers) are blocked elsewhere — Wayland has no surface
extensions in this DDK, and KWin's compositor fails in Mesa's kopper integration. What it does is
make the driver implement the standard DRM contract, so those paths are no longer blocked by *this*
gap once the others clear.

Note the patch lives in a build copy (`/home/radxa/prime-build`), so a kernel or `img-bxm-dkms`
upgrade rebuilds the stock module; re-run `activate.sh` after such an upgrade.
