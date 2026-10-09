# G2D — the 2D accelerator that is present, powered, clocked, and has no driver

**This is the highest-value disabled feature found.** G2D is a **2D graphics accelerator**: bit blits, fills,
stretch-blits (scaling), alpha blending, colour-key, the full ROP set, rotation/flip/mirror, multi-layer mixing,
YUV↔RGB conversion, and an **asynchronous task queue**. **That is precisely the work the compositor and
Xwayland currently do on the CPU** — the measured bottleneck (Xwayland 72.6% of a core, mostly kernel `SYS` from
buffer copies and dmabuf handling).

## Evidence that the hardware is there and idle

| check | result |
|---|---|
| **platform device** | **`/sys/bus/platform/devices/5440000.g2d` EXISTS** |
| **driver bound** | **NONE** |
| **clock** | `g2d` = **300 MHz**, `g2d-gate` = 26 MHz, both marked **`deviceless`, `no_connection_id`** in `clk_summary` |
| **interconnect** | listed as an **NSI master (`g2d`)** with its own **`g2d_iommu`** |
| **udev** | `/run/udev/data/+platform:5440000.g2d` present |
| **kernel config** | **`# CONFIG_AW_G2D is not set`** |
| **UAPI header** | **PRESENT**: `/usr/src/linux-headers-6.6.98-5-aw2511/bsp/include/uapi/linux/sunxi-g2d.h` |
| **driver source** | **ABSENT** — no `g2d*.c` anywhere on disk; `bsp/` is `include`-only (984K) |
| **module build tree** | **PRESENT** — headers pkg ships `Makefile`, `.config`, `Module.symvers` (980 KB), `scripts/` |

**Why the clock table proves it is unclaimed:** the kernel registered and programmed the g2d clock (300 MHz) but no
device consumes it — `deviceless` + `no_connection_id` is exactly what an enabled-but-unbound platform device looks
like.

## What it can do (from the shipped UAPI, quoted)

**Commands:** `G2D_CMD_BITBLT`, `G2D_CMD_BITBLT_H`, `G2D_CMD_FILLRECT`, `G2D_CMD_FILLRECT_H`,
**`G2D_CMD_STRETCHBLT`** (scale), **`G2D_CMD_BLD_H`** (blend), `G2D_CMD_MASK_H`, **`G2D_CMD_MIXER_TASK`**
(multi-layer composite), **`G2D_CMD_LBC_ROT`** (rotate), `G2D_CMD_PALETTE_TBL`, `G2D_CMD_INVERTED_ORDER`,
**`G2D_CMD_QUEUE` / `TASK_APPLY` / `CREATE_TASK` / `TASK_DESTROY`** (async task queue), and memory ops
`MEM_REQUEST` / `MEM_RELEASE` / `MEM_GETADR` / `MEM_FLUSH_CACHE` / `MEM_SELIDX`.

**Blend/ROP:** `G2D_BLT_PIXEL_ALPHA`, `G2D_BLT_PLANE_ALPHA`, `G2D_BLT_MULTI_ALPHA`, `G2D_BLT_DST_PREMULTIPLY`,
`G2D_BLT_SRC_COLORKEY`, `G2D_BLT_DST_COLORKEY`, plus the classic ROP set (`COPYPEN`, `NOTCOPYPEN`, `MASKPEN`,
`MERGENOTPEN`, `NOTXORPEN`, …) and `FLIP_HORIZONTAL/VERTICAL`, `ROTATE90/180/270`, `MIRROR45/135`.

**Formats:** ARGB/ABGR/RGBA/BGRA/XRGB/XBGR/RGBX/BGRX 8888; RGB888/BGR888; RGB565/BGR565; ARGB/ABGR/RGBA/BGRA
4444 and 1555; 10-bit 2101010/1010102; 8-6-8-5 variants; **and YUV** — `IYUV422` variants, `YUV422UVC`,
`YUV422_PLANAR`, **`YUV420UVC`**, `YUV420_PLANAR`, `YUV411UVC`.

## Verdict

**ENABLEABLE, but not by flipping a config alone.** Required:

1. **Obtain the g2d driver source** (`drivers/gpu/drm/sunxi/g2d` or `drivers/media/platform/sunxi/g2d`) **for
   6.6.98-5-aw2511** from the Radxa/Allwinner BSP kernel tree. `apt` carries `linux-image-radxa-a733` and
   `linux-headers-radxa-a733` (candidate 6.6.98-5) but **no matching `linux-source` package**; the a733 repo
   (`https://radxa-repo.github.io/a733-trixie-test`) is configured, and `github.com/radxa/kernel` answered 504 at
   the time of checking, so the source must be fetched when that endpoint is reachable.
2. **Build it out-of-tree** against `/usr/src/linux-headers-6.6.98-5-aw2511` — **proven pattern**: the installed
   `img-bxm-dkms-0.1.0-3` builds exactly this way (`KERNELDIR=/usr/src/linux-headers-${kernelver}`), so no kernel
   rebuild is needed.
3. `insmod`, then verify **`/dev/g2d` appears**, **`5440000.g2d` gains a driver**, and **the `g2d` clock leaves
   `deviceless`**.
4. Benchmark blit/fill/blend/stretch throughput against `memcpy` and against the PowerVR GPU, before wiring it to
   anything.

**Risk:** this changes kernel state on a board whose GPU path is already fragile; the `gpu-fw-guard` and the
switch guards must stay intact and the desktop up. Build and load as a module so it is removable.

## Why this matters more than the VPU/NPU findings

**The measured system bottleneck is not video coding and not inference — it is the compositor's per-frame copies
and blends.** The VPU cannot do them (no 2D capability), the NPU is int8 inference, and the DE can only composite
onto its own planes. **G2D is the block whose operation list matches the bottleneck one-for-one**, and it is
powered, clocked at 300 MHz, and entirely unclaimed.
