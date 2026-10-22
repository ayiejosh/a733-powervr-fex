# zram: compressed swap, tuned for a 5.8 GiB view of an 8 GiB board

The A733 board shows **5.8 GiB** of its 8 GiB to Linux (~2.2 GiB is reserved for the GPU/firmware,
plus `cma=256M`). Under a build or a browser that is tight, so swap matters — but swapping to the
SATA SSD is both slow and wear. zram keeps swap pages **compressed in RAM** instead.

## Measured effect on this board

```
985.6 MiB of swapped data  ->  253.5 MiB of RAM   (3.9 : 1, lz4)
                            ~732 MiB of usable RAM reclaimed
```

`/proc/vmstat` confirms it is doing real work, not sitting idle: `pgsteal_kswapd 2,932,608`,
`compact_stall 0`. Fallback layout is zram at priority **100** in front of an 8 GiB `/swapfile`
on `/dev/sda3` at priority **−2**, so the SSD is only touched when zram fills.

## How the pieces fit

| file | goes to | role |
|---|---|---|
| `zramswap.default` | `/etc/default/zramswap` | `ALGO=lz4`, `PERCENT=100`, `PRIORITY=100` — 5.78 GiB device, 100% of RAM |
| `zramswap.service.d/10-recomp.conf` | `/etc/systemd/system/zramswap.service.d/` | `ExecStartPre` hook — algorithm writes are only legal while `disksize=0` |
| `zram-multicomp-prepare.sh` | `/usr/local/sbin/` | makes/recreates zram0, sets **lz4 primary + zstd secondary**, attaches writeback |
| `zram-recompress.sh` | `/usr/local/sbin/` | ages cold pages, hands incompressible ones to writeback, recompresses the rest to zstd |
| `zram-recompress.service` + `.timer` | `/etc/systemd/system/` | runs the above every 10 min at `Nice=19`, skipped under load ≥ 6 |
| `zram-writeback-loop.service` | `/etc/systemd/system/` | loop-attaches `/home/radxa/zram-writeback.img` (2 GiB) before `zramswap` starts |

Three design points that are not obvious:

* **lz4 primary, zstd secondary.** lz4 is fast enough for the write path; zstd only ever sees pages
  that have gone cold, so its cost competes for idle cycles rather than latency.
* **`hot_remove`/`hot_add`, never `zramctl --reset`.** On util-linux 2.41 `--reset` hot-*removes* the
  device and the board came back without zram swap. The prepare script also `swapoff`s first, because
  `hot_remove` refuses while the block device is open.
* **Writeback for incompressible pages.** A page that won't compress wastes a full 4 KiB; handing
  those to the 2 GiB backing image is cheaper than storing them in RAM.

## Install

```sh
sudo cp zramswap.default          /etc/default/zramswap
sudo mkdir -p /etc/systemd/system/zramswap.service.d
sudo cp zramswap.service.d/10-recomp.conf /etc/systemd/system/zramswap.service.d/
sudo cp zram-multicomp-prepare.sh zram-recompress.sh /usr/local/sbin/
sudo cp zram-recompress.service zram-recompress.timer zram-writeback-loop.service /etc/systemd/system/
sudo chmod +x /usr/local/sbin/zram-multicomp-prepare.sh /usr/local/sbin/zram-recompress.sh
truncate -s 2G /home/radxa/zram-writeback.img
sudo systemctl daemon-reload
sudo systemctl enable --now zram-writeback-loop zramswap zram-recompress.timer
```

## Kernel side, and one trap

Running kernel: **`6.6.98-5-aw2511`** (`#5 SMP Sun Sep 20 04:35:45 UTC 2026`), arm64.
Cmdline carries `cma=256M coherent_pool=4M`.

**Do not trust `/proc/config.gz` for zram features on this kernel.** It reports:

```
# CONFIG_ZRAM_WRITEBACK is not set
# CONFIG_ZRAM_MULTI_COMP is not set
# CONFIG_ZRAM_TRACK_ENTRY_ACTIME is not set
```

…yet the shipped module **has all three**. Verified two independent ways:

```sh
xzcat /lib/modules/6.6.98-5-aw2511/kernel/drivers/block/zram/zram.ko.xz | strings |
  grep -E '^(backing_dev|recomp_algorithm|idle|recompress)$'      # all four present
ls /sys/block/zram0/{recompress,backing_dev,idle,recomp_algorithm} # all exist
```

The module (`vermagic 6.6.98-5-aw2511`, so it *is* the matching one) was built with options the
exported config does not reflect. **Check sysfs or the module, not the config**, before concluding a
feature is unavailable. Note the module is xz-compressed — plain `strings` on it returns nothing and
looks like a missing feature.

## What is *not* here

`zswap` is not set in the config and is not used; zram is the whole strategy. THP is unavailable
(`CONFIG_TRANSPARENT_HUGEPAGE` not set), so there is no hugepage interaction to tune.
