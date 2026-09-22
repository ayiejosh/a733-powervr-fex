# Prepared upstream contributions (NOT sent)

Two patches that would make the open PowerVR stack usable on this board and on every other
Allwinner A733. They are **written and apply-tested, and deliberately unsent** — sending them is an
outward-facing act and needs an explicit go-ahead.

| patch | goes to | state |
|---|---|---|
| [`linux-firmware/0001-powervr-add-firmware-for-BXM-4-64-revision-36.56.104.183.patch`](linux-firmware/) | `linux-firmware@kernel.org` | ready to send |
| [`drm-imagination/0001-drm-imagination-add-BXM-4-64-revision-36.56.104.183-as-experimental.patch`](drm-imagination/) | `dri-devel@lists.freedesktop.org` (IMG maintainers), CC `linux-sunxi` | ready to send |
| [`drm-imagination/0002-drm-imagination-promote-BXM-4-64-revision-36.56.104.183-to-supported.patch`](drm-imagination/) | same | **do not send yet** — its commit message carries a placeholder for a Vulkan CTS result that has not been produced |

## Why these two

The A733's GPU is a PowerVR **BXM-4-64 MC1 at BVNC 36.56.104.183**. Everything needed for the
mainline driver to bind now exists except two lines of paperwork:

* **firmware** — Imagination publishes the open-ABI blob for exactly this revision in their own
  firmware tree (`gitlab.freedesktop.org/imagination/linux-firmware`, branch `powervr`, commit
  `8a58f818`, version `1.1.OS@6976702`), but upstream linux-firmware only carries 33.15.11.3,
  36.52.104.182 and 36.53.104.796. The board already has the file
  (`/lib/firmware/powervr/rogue_36.56.104.183_v1.fw`, `sha256 1db1c399…`, `info_version 3`,
  `PVR_FW_FLAGS_OPEN_SOURCE`, packed BVNC matching the silicon).
* **kernel whitelist** — `pvr_gpu_support_level()` in `drivers/gpu/drm/imagination/pvr_device.c`
  knows only those same three parts, so probe fails with `-ENODEV` unless `exp_hw_support=1`.
  Mesa's `src/imagination/common/device_info/bxm-4-64.h` already carries the GPU ID (`0x36104183`),
  so the kernel is the only component that refuses the part.

The precedent is exact: 36.52.104.182 (TH1520, same BXM-4-64) was added as experimental and later
promoted to supported (`e55fead2`, 2026-07-24) after a CTS run — `0001` follows the first half,
`0002` the second.

## Provenance and verification

* `pvr_device.c` was taken from `torvalds/linux` master (7.3.0-rc4) on 2026-09-22;
  `WHENCE` (486 096 B) from `firmware/linux-firmware.git` the same day. The patches are generated
  from those exact bytes, so their context matches the real trees.
* Both were checked with `git apply --check` against a pristine copy of the upstream file, and
  `0002` additionally against `0001` already applied. `0001` for linux-firmware is a `--binary`
  patch (it adds the firmware blob and one `File:` line to `WHENCE`).
* The firmware blob in the linux-firmware patch is byte-identical to
  `/lib/firmware/powervr/rogue_36.56.104.183_v1.fw` on this board.

## Sending (when approved)

```sh
# linux-firmware
git clone --depth 1 https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git
cd linux-firmware && git am --3way /path/to/upstream/linux-firmware/0001-*.patch
git send-email --to=linux-firmware@kernel.org --cc=dri-devel@lists.freedesktop.org \
               --cc=linux-sunxi@lists.linux.dev 0001-*.patch

# kernel
git clone --depth 1 --branch master https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
cd linux && git am --3way /path/to/upstream/drm-imagination/0001-*.patch
git send-email --to=dri-devel@lists.freedesktop.org --cc=linux-sunxi@lists.linux.dev \
               --cc=linux-kernel@vger.kernel.org 0001-*.patch
```

Maintainers of record (`MAINTAINERS`, "IMGTEC POWERVR DRM DRIVER"): Frank Binns, Matt Coster,
Alessio Belle — the same people who authored the TH1520/BXM-4-64 work.

## What this does *not* do

Adding the whitelist entry does not by itself bring the driver up on this board: the Radxa 6.6 BSP
tree (`radxa/kernel`, `allwinner-aiot-linux-6.6`) contains **neither `drivers/gpu/drm/imagination`
nor `drm_gpuvm`**, and the A733 has no mainline platform support yet. These patches are what makes
that path a configuration exercise for whoever does the backport or waits for a newer kernel.
See [`../docs/GPU-RESEARCH-2026-09-22.md`](../docs/GPU-RESEARCH-2026-09-22.md) §4.
