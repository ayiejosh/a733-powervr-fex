#!/bin/bash
# Install the PRIME-import-patched pvrsrvkm and make it live at the NEXT boot.
# Revert: /home/radxa/prime-build/staged/revert.sh (then reboot).
set -e
K=$(uname -r); D=/lib/modules/$K/updates/dkms
[ -f $D/pvrsrvkm.ko.xz ] && cp -n $D/pvrsrvkm.ko.xz /home/radxa/prime-build/stock/pvrsrvkm.ko.xz.live
install -m 644 /home/radxa/prime-build/staged/pvrsrvkm.ko.xz $D/pvrsrvkm.ko.xz
depmod -a "$K"
echo "installed; now: sudo reboot   (acceptance test after boot:"
echo "  LD_LIBRARY_PATH=/usr/local/lib VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json \\"
echo "    /tmp/dmabuf_foreign_test   # drmPrimeFDToHandle must return a handle, not ENOSYS)"
