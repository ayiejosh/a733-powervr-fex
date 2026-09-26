#!/bin/bash
# Put the stock module back and reboot.
set -e
K=$(uname -r); D=/lib/modules/$K/updates/dkms
install -m 644 /home/radxa/prime-build/stock/pvrsrvkm.ko.xz $D/pvrsrvkm.ko.xz
depmod -a "$K"
echo "stock module restored; now: sudo reboot"
