# Full A733 hardware map — every block, what drives it, what uses it

**Written because I judged the VPU (and earlier the NPU) on a narrow slice and concluded too fast.** The device
tree lists **81 enabled blocks**. Mapping all of them changed the answer: **the block a workload needs is often
not the one being examined, and the useful path is frequently two blocks chained.**

## 1. The video/display chain — and what is idle

| block | DT node | driver | node | userspace | in use |
|---|---|---|---|---|---|
| **VPU decode** | `cedar_dev` | in-kernel + `sunxi_ve` | `/dev/cedar_dev` | `libvdecoder` + demo | **yes, by me only** |
| **VPU encode** | `cedar_dev_ve2` | `sunxi_ve` | `/dev/cedar_dev_ve2` | `libvencoder` + demo | **yes, by me only** |
| **Display engine (DE)** | `de@5000000` `display-engine-v352` | `sunxi-display-engine` → `sunxi-drm` | `/dev/dri/card0` | DRM/KMS | **scanout only** |
| **DE planes** | 7 planes: `plane-0-vch0`, `-1-vch1`, `-0-vch2`, `-2-uch0`, `-3-uch2`, `-1-uch1`, `-2-uch3` | same | DRM planes | DRM/KMS | **2 active (1 buffer + cursor)** |
| **DE scalers** | `scaler@104000` … `224000` ×6 | same | DRM plane props | none | **ALL DISABLED** |
| **DE alpha blend** | `afbd@`, `tfbd@` ×4 | same | DRM plane props | none | **ALL DISABLED** |
| **DE writeback** | `connector[144] Writeback-1` | same | DRM connector | none | **unused** |
| **Deinterlace** | `deinterlace@5400000` | `deinterlace` | `/dev/deinterlace` | **none** | **no** |
| **VI scalers** | `vind@5800800/scaler@` ×9 | `sunxi-scaler` | — | **none** | **no** |
| **Display out** | `vo0@5500000`, `vo1@5510000`, `tcon3`, `tcon4` | `tcon-top0/1` | via card0 | DRM | yes |
| **HDMI + CEC** | `hdmi0@5520000`, `cec` | `sunxi-hdmi` | `/dev/hdmi`, `/dev/cec0` | DRM | `card0-HDMI-A-1 connected` |

### The linkage that matters

```
VPU decode ─► YUV frame ─► DE video plane ─► CSC + scaler (hardware) ─► scanout
  (0.4 cores)               (7 available)     scaler@ / csc@          zero CPU copy
```

**The DE's 7 planes carry `color-encoding=ITU-R BT.601 YCbCr`, so they accept YUV directly.** A video player
today does: software decode → **swscale (measured 6.396 s CPU for 90 frames of YUV→BGRA + upscale)** → RGB → GPU
composite → DE gets one finished buffer. **Every stage of that could be hardware.**

**So the VPU IS usable for the display path — not alone (it cannot composite), but with the DE as the middle
step.** My earlier "no" was right about the VPU and wrong about the system.

## 2. Compute / accelerator blocks

| block | DT node | driver | node | userspace | notes |
|---|---|---|---|---|---|
| **GPU** | `img,gpu` | `pvrsrvkm` (vendor) / `powervr` (open) | `/dev/dri/card1`, `renderD128` | Mesa ICDs | **2.4× behind vendor** |
| **NPU** | `npu@3600000` `allwinner,npu` | **`vipcore` bound** | `/dev/vipcore` | **NONE** | thermal zone present; **no lib, tool or package anywhere** |
| **Crypto engine** | `ce@4603000` `allwinner,sunxi-ce` | **not bound** | — | — | 39 `/proc/crypto` algos but **no sunxi driver among them → software fallback** |
| CPU | 2×A76@2002 + 6×A55@1794 | — | — | — | all pinned at max freq |

## 3. Everything else enabled (81 blocks total)

**Storage:** `sdmmc@4020000`, `sdmmc@4022000` (`mmcblk1`), `ufs@04520000`, `spi@2540000` + `spidev0` (`spi-nor`,
`mtd0`), `mtdblock0`
**USB:** `ehci0/1`, `ohci0/1`, `udc-controller`, `usbc0/1/2`, `dwc3 xhci2`, `usbc@10 otg-manager`
**PCIe:** `pcie@6000000`, `serdes@6c00000` + combo PHYs (usb/pcie/dp/aux-hpd)
**Network:** `ethernet@4500000` (`sunxi-gmac-210`, `dwmac-5.20`), `aic8800_fdrv` WiFi, `aic_btusb` BT
**Audio:** `i2s0/i2s3` plat+ mach, `hdmi_codec`, `ac101b@3e`, `aw87x_pa`, `sunxi-codec-hdmi`
**Sensors/misc:** `gpadc@2521000`, `lradc@2524000`, `irrx@7040000`, `pwm@2527/2528/7023`, `s_pwm0`
**PMIC:** `axp8191` with `axp2101-pek` powerkey
**I2C:** `twi@2510000` (+`eeprom@50` atmel,24c16), `twi@7083000/7084000/7085000`
**UART:** `uart@2500000`
**Power domains:** `pd_de_sys`, `pd_gpu_core`, `pd_gpu_top`, `pd_npu`, `pd_pcie`, `pd_usb2`, `pd_ve_dec`,
`pd_ve_enc`, `pd_vi`, `pd_vo`, `pd_vo1`
**Other:** `addr_mgt`, `auto_print`, `sunxi-drm`

## 4. What this changes about my earlier conclusions

| earlier claim | status now |
|---|---|
| "the VPU is only useful for encode" | **incomplete** — it chains with the DE for zero-copy video playback |
| "the compositor cannot be offloaded" | **right about the VPU, untested against the DE** — the DE has **hardware blending, 7 planes, alpha, and 6 scalers, all disabled** |
| "the NPU is a dead end" | **held** — device + `vipcore` bound + thermal zone, **no userspace in any package** |
| "CPU is not a limiter" | **held** — max frequency throughout |
| "nothing standard can reach the VPU" | **held** — no VA-API, V4L2 M2M, hwaccel or GStreamer element |

**The mistake was scope, not method: I measured the VPU thoroughly and the system not at all.**

## 5. The untested questions this map exposes

1. **Can the DE planes composite the desktop** instead of the GPU (KMS overlay), removing Xwayland's copies?
   **6 scalers + 4 alpha units + 7 planes are idle.**
2. **Can DE writeback capture the screen in hardware**, removing the `x11grab` CPU cost (2.817 s / 3 s)?
3. **Does the VPU → DE YUV plane chain work?** That is a hardware video-playback path with no CPU conversion.
4. **Why is the crypto engine not bound?** 39 algorithms, none from sunxi.
5. **What are the 9 VI scalers for**, and is any of them reachable?
