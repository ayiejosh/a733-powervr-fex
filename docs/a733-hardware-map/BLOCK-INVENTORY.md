# A733 block inventory — enabled, driven, used, and DISABLED-BUT-ENABLEABLE

**Authoritative source = the NSI interconnect + IOMMU topology + power domains + kallsyms, NOT a shallow
device-tree walk.** (My first DT walk found 81 `status=okay` nodes and MISSED g2d, isp, csi and di entirely,
because they sit deeper or are described only by the interconnect. The topology is the better inventory.)

## The real block list (from the interconnect)

```
NSI masters:  csi  de  di  eink  g2d  isp  npu  usb_pcie  ve0  ve1  ve2
IOMMUs:       csi_iommu  de0_iommu  di_iommu  eink_iommu  g2d_iommu
              isp_iommu  ve_dec0_iommu  ve_dec1_iommu  ve_enc_iommu
```

## Status table

| block | in fabric | DT node | driver | device node | userspace | used today |
|---|---|---|---|---|---|---|
| **GPU** | yes | `img,gpu` | `pvrsrvkm`/`powervr` | `/dev/dri/card1`, `renderD128` | Mesa ICDs | yes |
| **Display engine (de)** | yes | `de@5000000` v352 | `sunxi-display-engine` → `sunxi-drm` | `/dev/dri/card0` | DRM/KMS | **scanout only** |
| **DE planes (7)** | — | via de | — | DRM planes | — | **2 of 7** |
| **DE scalers (6)** | — | `scaler@` props | — | — | — | **ALL DISABLED** |
| **DE writeback** | — | `Writeback-1` | — | DRM connector | — | **unused** |
| **VE decode (ve0+ve1)** | **yes, TWO** | `1c0e000.ve` | `sunxi_ve` | `/dev/cedar_dev` | `libvdecoder` | **PROVEN working** |
| **VE encode (ve2)** | yes | `1c10000.ve2` | `sunxi_ve` | `/dev/cedar_dev_ve2` | `libvencoder` | works |
| **NPU (VIP9000)** | yes | `npu@3600000` | **`vipcore` bound** | `/dev/vipcore` | **NONE** | **idle** (`pd_npu off-0`) |
| **Deinterlacer (di)** | yes | `deinterlace@5400000` | `deinterlace` | `/dev/deinterlace` | **none** | **no** |
| **JPEG/scale scalers (VI)** | — | `vind@5800800/scaler@` ×9 | `sunxi-scaler` | — | **none** | **no** |
| **🔴 G2D (2D blitter)** | **yes + `g2d_iommu`** | **NONE** | **NONE** | none | none | **no — no driver at all** |
| **ISP** | yes + `isp_iommu` | **not in DT** | `CONFIG_SUPPORT_ISP_TDM=y` | — | — | **no** |
| **CSI (camera in)** | yes + `csi_iommu` | **not in DT** | `CONFIG_CSI_VIN=m` | **no `/dev/video*`** | — | **no** |
| **E-ink** | yes + `eink_iommu` | not in DT | — | — | — | no panel |
| **Crypto (ce)** | yes | `ce@4603000` | **not bound** | — | — | **software only** |
| **HDMI + CEC** | yes | `hdmi0@5520000` | `sunxi-hdmi` | `/dev/hdmi`, `/dev/cec0` | DRM | `HDMI-A-1 connected` |
| **Display out (vo/tcon)** | yes | `vo0/vo1`, `tcon3/4` | `tcon-top0/1` | via card0 | DRM | yes |
| **Audio chain** | yes | i2s0/i2s3 + hdmi_codec + ac101b + aw87x_pa | `snd_soc_*` | — | ALSA | yes |
| **Storage** | yes | `sdmmc`×2, `ufs`, `spi-nor` | — | `mmcblk1`, `mtd0` | — | yes |
| **USB** | yes | ehci/ohci/udc/dwc3 | — | — | — | yes |
| **PCIe** | yes | `pcie@6000000` + combo PHYs | — | — | — | yes |
| **Gigabit eth** | yes | `ethernet@4500000` | `sunxi-gmac`/`dwmac` | — | — | yes |
| **WiFi/BT** | yes | (SDIO) | `aic8800_fdrv`, `aic_btusb` | — | — | yes |
| **PMIC** | yes | `axp8191` + `axp2101-pek` | — | — | — | yes |
| **IR receiver** | yes | `irrx@7040000` | — | — | — | ? |
| **CPU (2×A76 + 6×A55)** | — | — | — | — | — | **all pinned at max freq** |

## DISABLED-BUT-ENABLEABLE, ranked by value

1. **🔴 G2D — the 2D blitter.** Present in the SoC fabric with its own IOMMU, and **the kernel's clock tables
   already define `g2d_clk`, `g2d_gate_clk` and `g2d_parents`** — but there is **no device-tree node, no driver and
   no `CONFIG_G2D`**. **This is the block that does blits, fills, rotations, alpha blending and format conversion
   — i.e. exactly the compositor's per-frame copies that currently burn Xwayland's CPU (72.6% of a core, mostly
   kernel `SYS`).** Enabling needs a vendor-BSP g2d driver + its DTS node + userspace.
2. **VA-API driver for the VE** — the VPU works but no standard consumer can reach it. A working driver existed
   and was lost; the source survives in the backup. **Highest-value/lowest-effort item.**
3. **NPU userspace** — `vipcore` bound, `pd_npu` off, no library. Prior work had the ACUITY/VIPLite toolchain
   working; it is absent now but refetchable.
4. **DE planes / 6 scalers / writeback** — present and idle. A compositor could composite and scale in the DE
   instead of the GPU, and could capture via writeback with no CPU copy.
5. **Crypto engine** — `ce@4603000` exists but is **unbound**, and none of the 39 `/proc/crypto` algorithms comes
   from the sunxi driver → **all crypto is software**.
6. **Deinterlacer** — device node exists, no userspace.
7. **CSI/ISP** — configured (`CONFIG_CSI_VIN=m`, `CONFIG_SUPPORT_ISP_TDM=y`) but no DT node and no `/dev/video*`
   → no camera path. May need only a DTS entry plus a sensor.
8. **9 VI scalers** — present, no userspace.

## The correction this inventory forces

**Two of my earlier conclusions were wrong because I inventoried the wrong way:**
* **"the VPU is only useful for encode"** — decode works (proven by CPU cost) and there are **two** decode engines.
* **"the NPU is a dead end"** — the kernel driver IS bound; only userspace is missing, and prior work had it running.
* **and I never saw g2d at all**, which is the block most likely to address the actual measured bottleneck (the
  compositor's copies).
