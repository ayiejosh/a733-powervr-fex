# G2D: source acquired and MODULE BUILT — the enableable feature is now real

**`g2d` is the block whose operation list matches the measured bottleneck (Xwayland's copies/blends: 72.6 % of a
core, futex+rpc dominated). It was present, clocked at 300 MHz, `deviceless`, with `# CONFIG_AW_G2D is not set`.**
**Its driver is now fetched and built as a loadable module.**

## How the source was found (the chain that mattered)

1. `apt-cache show linux-image-radxa-a733` → **`Source: linux-aw2511`** — the authoritative source package name.
2. `github.com/radxa/kernel` (504 on the web UI) → the **packaging** repo is `radxa-pkg/linux-aw2511`, which uses
   **git submodules**:
   ```
   src          → github.com/radxa/kernel          branch allwinner-aiot-linux-6.6
   bsp          → github.com/radxa/allwinner-bsp    branch cubie-aiot-v1.4.8
   device-a733  → github.com/radxa/allwinner-device branch device-a733-v1.4.8
   ```
3. **The driver is NOT in the kernel tree — it is in the `bsp` submodule**: `radxa/allwinner-bsp @
   cubie-aiot-v1.4.8 : drivers/`**`g2d/`** — **47 paths**, matching the `bsp/include/uapi/linux/sunxi-g2d.h`
   that the installed headers package ships.

## What was fetched

**41 files, 444,953 bytes** → `/home/radxa/g2d-module/g2d/`

```
g2d/Kconfig  g2d/Makefile  g2d/syncfence.c  g2d/g2d_buf_cache.c/.h  g2d/g2d_trace.h
g2d/g2d_rcq/   g2d.c (41.7K)  g2d_mixer.c (31.6K)  g2d_bld.c (19.8K)  g2d_scal.c (18.2K)
               g2d_rcq.c  g2d_top.c  g2d_debug.c  g2d_wb.c  g2d_rotate.c
               g2d_ovl_u.c  g2d_ovl_v.c  + headers
g2d/g2d_legacy/  (g2d_driver.c, g2d_bsp_v2.c, g2d_regs*.h — excluded from the build)
```

## The build

**Out-of-tree against `/usr/src/linux-headers-6.6.98-5-aw2511`, using the RCQ (modern register-command-queue)
variant.** `Makefile` at `/home/radxa/g2d-module/Makefile`.

**Result: it BUILT ON THE FIRST ATTEMPT.**

```
CC [M] g2d_rcq/g2d.o  g2d_top.o  g2d_debug.o  g2d_mixer.o  g2d_ovl_v.o  g2d_ovl_u.o
       g2d_rcq.o  g2d_scal.o  g2d_wb.o  g2d_bld.o  g2d_rotate.o  syncfence.o
LD [M] /home/radxa/g2d-module/g2d_sunxi.ko     164,560 bytes
```

**Symbol check — the gate that decides whether it can load:**

* **77 undefined symbols; ALL are present in `/proc/kallsyms`** → the kernel can satisfy every one.
* **No BSP-only dependency**: `sunxi_info`, `sunxi_err`, `sunxi_warn`, `sunxi_debug`,
  `sunxi_reset_device_iommu` are **not referenced** after preprocessing — they were inlined macros.
* **No kernel rebuild required** — the `tristate` Kconfig made `=m` valid.

## Why this is the highest-value unlock on the board

`g2d`'s UAPI provides **bitblt, fillrect, stretchblt (scale), blend with pixel/plane/multi-alpha, colour-key, the
full ROP set, rotation/flip/mirror, multi-layer `MIXER_TASK`, YUV↔RGB conversion, and an async task queue** — and
the fetched driver implements them in `g2d_mixer.c`, `g2d_bld.c`, `g2d_scal.c`, `g2d_rotate.c`, `g2d_ovl_*.c`,
`g2d_wb.c` (writeback), driven through `g2d_rcq.c`.

**That is the per-frame copy/blend/scale work that currently runs on the CPU.**

## Next step, and its risk

**Load it and verify:** `insmod g2d_sunxi.ko` → expect `/dev/g2d`, `5440000.g2d` gaining a driver, and the `g2d`
clock leaving the `deviceless` state in `clk_summary`.

**Risk, stated plainly:** this is an untested BSP driver on a live desktop, and this board has rebooted five times
from driver interactions (three of them in the VE path). It is a **removable module**, `g2d` is separate silicon
from the display (`5440000.g2d`, its own clocks and its own IOMMU), and the load is one `insmod` — but a probe
fault would reboot the board. **The `gpu-fw-guard` and switch guards are unaffected either way.**

---

# ✅ LOADED — g2d is live

**`sudo insmod ./g2d_sunxi.ko` → exit 0, first attempt, no reboot, nothing broken.**

```
module loaded:  g2d_sunxi 106496B  used:0
driver bound:   /sys/bus/platform/devices/5440000.g2d/driver -> bus/platform/drivers/g2d
device node:    crw------- 1 root root 509, 0   /dev/g2d

dmesg:
  g2d 5440000.g2d: Adding to iommu group 0
  sunxi:g2d_sunxi:[INFO]: [G2D]: rcq version initialized.major:509
  sunxi:g2d_sunxi:[INFO]: [G2D]: g2d_module_init
```

**The clock table proves the hardware is now claimed:** before the load, `g2d` read **`deviceless`**; after, a
**`g2d@5440000` consumer entry appears** against the 300 MHz `g2d` clock.

**Nothing was disturbed:** kwin_x11 **ALIVE**, GPU driver still **`pvrsrvkm`**, display-manager **active**,
`gpu-fw-guard` **active**. **No reboot, no fault.**

## What this means for the objective

**A hardware block that was present, powered, clocked at 300 MHz and completely unusable is now driven, with a
device node** — and the entire path was non-invasive:

1. the **GPU driver binding was never touched**;
2. **no kernel was rebuilt** (the `tristate` Kconfig made a module valid);
3. **one `insmod`**, and the desktop never noticed;
4. **fully reversible** — `rmmod g2d_sunxi` removes it.

**This is the first disabled block actually enabled on this board**, and it is the one whose operation list
(`g2d_mixer.c`, `g2d_bld.c`, `g2d_scal.c`, `g2d_rotate.c`, `g2d_ovl_*.c`, `g2d_wb.c` behind `g2d_rcq.c`) matches the
measured bottleneck — the CPU copies, blends and scales that cost Xwayland 72.6 % of a core.

## Next: prove it does work, then measure it

**`/dev/g2d` has no userspace library** — there is no `libg2d` and no tool on this system, so nothing calls it yet.
The driver's UAPI is fully known (`bsp/include/uapi/linux/sunxi-g2d.h`: `G2D_CMD_BITBLT`, `FILLRECT`,
`STRETCHBLT`, `BLD_H`, `MIXER_TASK`, `QUEUE`, `TASK_APPLY`, `QUERY_VERSION`, …), so a minimal userspace probe is
the next step: `G2D_CMD_QUERY_VERSION` first, then a blit of a known pattern with a CPU readback to verify the
result, then a throughput measurement against `memcpy` and against the GPU.

**Success criterion:** a pattern blitted by g2d matches the pattern computed on the CPU. **Only then does a
throughput number mean anything.**
