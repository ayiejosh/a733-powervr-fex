# pvrsrvkm — DRM PRIME import patch (+ the live-compositor kernel deadlock)

> **Kernel `6.6.x-aw2511` (trixie BSP).** The PRIME-import patch below originated on the
> 5.15 BSP and **still applies** to the 6.6 `img-bxm` DDK 24.2 module. The new, critical
> finding on 6.6 is the **live-compositor kernel deadlock** documented at the bottom — read it.

## The PRIME-import patch

`pvrsrvkm-drm-prime-import.patch` adds the standard DRM PRIME **import** direction to the
Imagination img-bxm DDK 24.2 kernel module, which ships with `prime_fd_to_handle` /
`gem_prime_import` unimplemented (returns `ENOSYS`). That gap is what makes `zink` +
kmsro/`wlroots` fail at `drmPrimeFDToHandle` — they can't turn a dma-buf fd into a GEM
handle on this device.

The patch wraps an external dma-buf in a PMR-backed GEM object using the **same
EXTERNAL-heap PMR machinery** the services `PhysmemImportDmaBuf` bridge already uses
(proven GPU-renderable), and wires:

- `physmem_dmabuf.c` — `PhysmemGEMPrimeImport()` (self-import short-circuit; otherwise
  `dma_buf_attach` + `PhysmemCreateNewDmaBufBackedPMR` + wrap in a GEM object).
- `physmem_dmabuf_internal.h` — declaration.
- `pvr_drm.c` — `pvr_gem_prime_import` wrapper as `.gem_prime_import`, and
  `.prime_fd_to_handle = drm_gem_prime_fd_to_handle`.

The refcount/lifetime contract is documented inline (importer returns 1 ref; do **not**
set `obj->dma_buf` or `obj->import_attach`). On 6.6 verify the contract against the 6.6
`drm_prime.c` / `drm_gem.c` if you adapt it.

### Apply

```sh
# 1. Install the DDK source (provides the tree this patches):
sudo apt-get install img-bxm-dkms        # Radxa A733 repo

# 2. Apply, from the DDK source root:
cd /usr/src/img-bxm-dkms-<ver>/img-bxm/linux/rogue_km/services/server/env/linux/
sudo patch -p1 < /path/to/pvrsrvkm-drm-prime-import.patch

# 3. Rebuild + load (from the DDK build dir):
cd /usr/src/img-bxm-dkms-<ver>/img-bxm/build/linux/sunxi_linux
sudo make BUILD=release KERNEL_CC=gcc KERNELDIR=/usr/src/linux-headers-$(uname -r)
# reload only when refcnt is 0 (cat /sys/module/pvrsrvkm/refcnt):
sudo rmmod pvrsrvkm && sudo insmod <path>/pvrsrvkm.ko
# permanent: xz -c pvrsrvkm.ko > /lib/modules/$(uname -r)/updates/dkms/pvrsrvkm.ko.xz; sudo depmod -a
```

### Verify
The patched `pvrsrvkm` must import a foreign dma-buf and render it: `drmPrimeFDToHandle`
on `/dev/dri/renderD128` returns a handle (not `ENOSYS`), round-trips handle->fd, and the
imported buffer is GPU-renderable (`vkImportMemoryFdKHR` + `vkCmdFillBuffer`, read back).
The off-screen kmsro bridge on `card0` (a `SCANOUT|RENDER` gbm buffer + zink/PowerVR) is
proven with this patch in place.

### Revert
Reinstall the stock `img-bxm-dkms`, or restore the backed-up `.orig` sources +
`pvrsrvkm.ko.orig` and `depmod -a`.

> Note: this patches the **proprietary** vendor module. The patch is shared for
> interoperability; the module source itself comes from the Radxa apt repo.

## ⚠️ CRITICAL 6.6 finding: a live GPU compositor through `pvrsrvkm` deadlocks the kernel

The off-screen GPU path is solid. **Driving the display from a live compositor on the
PowerVR GPU is not.** When a real compositor (e.g. KWin Wayland) engages `pvrsrvkm` to
scan out the HDMI display, the **kernel deadlocks**: `pvrsrvkm` `mutex_spin_on_owner` in
IRQ -> hard hang -> the board must be power-cycled (confirmed in `journalctl -b -1` after
the attempt). The buffer-sharing bridge under it is *proven* (a `SCANOUT|RENDER` gbm
buffer on `card0` + zink/PowerVR render all succeed off-screen) — the deadlock is the
**closed `pvrsrvkm` driver**, not Mesa/zink, and it is **not fixable from userspace**.

Same failure class to avoid on this board:
- KDE/KWin **GL compositing** on the GPU,
- `DXVK_HUD` (never set it),
- `scrot` while a GPU app runs (use `kmsgrab`/`grim` instead).

Consequence: the desktop stays on **software-rendered X11**. The
`LIBGL_ALWAYS_SOFTWARE` workaround in the session env is precisely the board's defense
against this hang. Only a **fixed `pvrsrvkm`** (or mainline `drm/imagination`, absent on
this kernel — see `../docs/FINDINGS.md`) can lift it. Off-screen GPU compute/render/D3D/
zink are all unaffected.
