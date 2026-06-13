# pvrsrvkm — DRM PRIME import patch

`pvrsrvkm-drm-prime-import.patch` adds the standard DRM PRIME **import** direction
to the Imagination img-bxm DDK 24.2 kernel module, which ships with
`prime_fd_to_handle` / `gem_prime_import` unimplemented (returns `ENOSYS`). That
gap is what makes `zink` + `wlroots` SIGSEGV at `drmPrimeFDToHandle` — they can't
turn a dma-buf fd into a GEM handle on this device.

The patch wraps an external dma-buf in a PMR-backed GEM object using the **same
EXTERNAL-heap PMR machinery** the services `PhysmemImportDmaBuf` bridge already
uses (proven GPU-renderable), and wires:

- `physmem_dmabuf.c` — `PhysmemGEMPrimeImport()` (self-import short-circuit for our
  own buffers; otherwise `dma_buf_attach` + `PhysmemCreateNewDmaBufBackedPMR` +
  wrap in a GEM object).
- `physmem_dmabuf_internal.h` — declaration.
- `pvr_drm.c` — `pvr_gem_prime_import` wrapper set as `.gem_prime_import`, and
  `.prime_fd_to_handle = drm_gem_prime_fd_to_handle`.

The refcount/lifetime contract is documented inline and was verified against the
v5.15 `drm_prime.c` / `drm_gem.c` (importer returns 1 ref; do **not** set
`obj->dma_buf` or `obj->import_attach`).

## Apply

```sh
# 1. Install the DDK source from the Radxa apt repo (provides the tree this patches):
sudo apt-get install img-bxm-dkms        # a733-bullseye repo

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

## Verify

```c
// drmPrimeFDToHandle on /dev/dri/renderD128 should return a handle (not ENOSYS),
// round-trip handle->fd, and the imported buffer must be GPU-renderable
// (import via vkImportMemoryFdKHR + vkCmdFillBuffer, read back).
```

## Revert

Reinstall the stock `img-bxm-dkms`, or restore the backed-up `.orig` sources +
`pvrsrvkm.ko.orig` and `depmod -a`.

> Note: this patches the **proprietary** vendor module. The patch is shared for
> interoperability; the module source itself comes from the Radxa apt repo.
