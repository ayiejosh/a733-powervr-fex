# Why the open stack renders slower — measured decomposition (2026-10-06)

`vkrender`'s headline number is one frame = **draw + full-surface image→buffer copy +
fence wait**, so it cannot say *where* a gap lives. `render-gap.sh` turns every knob
`vkrender` already has in one pass. Raw logs: `render-gap-open.txt`,
`render-gap-vendor.txt` (also in /tmp).

Same binary, same workload, one stack bound at a time. 512×512, 60 frames, BATCH=1,
three runs each. Repeatability ±5%.

## Headline

| term (ms/frame @512²) | vendor | open | delta | share of gap |
|---|---|---|---|---|
| CPU `record` | 0.045 | 0.455 | **+0.410** | 39% |
| CPU `submit` | 0.056 | 0.165 | +0.109 | 10% |
| GPU `gpu_wait` | 0.636 | 1.164 | +0.528 | 51% |
| **total** | **0.737** | **1.784** | **+1.047** | |

Splitting the GPU half:

| GPU term | vendor | open | delta | share |
|---|---|---|---|---|
| fixed per-pass overhead | 0.000 | 0.386 | +0.386 | 37% |
| actual render/copy work | 0.636 | 0.778 | +0.142 | 14% |

**The open stack is 2.4× slower, but only 14% of the gap is slower rendering.**
50% is CPU-side driver overhead, 37% is a fixed per-frame GPU cost the vendor does
not have at all.

## Evidence

**1. Clock is not the cause.** `gpu0` = 1104000000 (1.104 GHz) under sustained load on
*both* stacks, same as the vendor's `sunxi_set_device_clk_rate:1104000000`. DVFS ruled out.

**2. The empty render pass costs 0.38 ms on open and exactly 0.000 ms on vendor.**

```
MODE=empty   (no draw, no copy, LOADOP/STOREOP=DONT_CARE)   gpu_wait ms
size        128     256     512    1024
vendor    0.000   0.000   0.000   0.000    record=0.003, submit=0.000
open      0.373   0.378   0.386   0.380    record=0.39-0.52, submit=0.09-0.12
```

Flat from 128² to 1024² → **not proportional to pixels, so not fill**. On the vendor
`submit=0.000` means `vkQueueSubmit` had nothing to do: the DDK elides a pass that
produces no output. Mesa's pvr submits it and the GPU spends ~0.38 ms on it.

**3. That 0.38 ms is real GPU work, not fence latency.** Batching 60 frames into one
submit does not amortize it:

```
MODE=empty, 512², gpu_wait per frame
BATCH=1   0.380     BATCH=5   0.338     BATCH=60  0.312
```

It converges to ~0.31 ms/frame and stays there. Fence-signal latency would have
vanished at BATCH=60.

**4. `record` is 10× the vendor's, and it is userspace, not the kernel.**
Vendor `record` is dead flat 0.019–0.021 ms at every size; open is 0.41–0.82 ms.
`strace -f -c -e trace=ioctl`: 671 ioctls / 20 frames vs 1736 / 60 frames →
**~27 ioctls per frame**, but only ~0.04 ms/frame of kernel time (and that is inflated
by strace's trap). So the cost is Mesa's command building, not syscall overhead.

**5. Shrinking the render area makes the open stack *slower*** — the signature of
full-surface work regardless of pixels:

```
512² surface, 60 frames      vendor      open
AREA=full                    0.774       1.788
AREA=half   (256² drawn)     0.591       1.879
AREA=quarter(128² drawn)     0.589       3.770   <-- 2.1x SLOWER
```

Vendor scales the correct way (less area = faster). Open inverts it, and its `record`
blows up to 1.77 ms. Consistent with the driver doing full-surface tile work
(load/store of the whole 1 MiB surface) regardless of the render area.

**6. Per-pixel slope**, subtracting each stack's own empty-pass baseline:

```
gpu_wait vs pixels (render-only, 512²->1024²)
vendor   0.407 ns/px      open   2.148 ns/px   = 5.3x
```

At 1.104 GHz that is 0.45 cycles/px vs 2.37 cycles/px.

## Conclusion

The render gap is **not a fill-rate problem and cannot be fixed by GPU-side tuning**.
Ranked by size:

1. **CPU command recording, +0.41 ms/frame (39%)** — Mesa pvr builds a 512² frame's
   commands in 0.46 ms where the DDK takes 0.045 ms. Pure userspace; ~27 ioctls/frame
   are not the cost.
2. **Fixed per-pass GPU cost, +0.39 ms/frame (37%)** — an empty pass costs 0.31–0.38 ms
   on open, 0.000 on vendor. Real GPU work, does not amortize under batching, does not
   scale with area. Looks like an unconditional full-surface tile load/store per pass.
3. **Actual rendering, +0.14 ms/frame (14%)** — 1.2× at 512², rising to ~3.2× at 1024².

Items 1 and 2 are worth ~76% of the gap and both are per-frame overhead, so the fix
that matters is *fewer/cheaper per-pass operations*, not faster rasterisation.

## Reproduce

```bash
cd /home/radxa/_REVIEW/emulation/trixie-prep/bench/pvr-vulkan
VK_ICD_FILENAMES=/home/radxa/pvr_gen_icd.json ./render-gap.sh open
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json ./render-gap.sh vendor
```

Stack switch is `ab-open-vs-vendor.sh`'s `bind_open` / `bind_vendor`; both stacks claim
the same platform device so only one can be bound at a time. The desktop
(`display-manager`, `kwin_x11`) must be stopped first — the *closed* driver oopses under
a live KDE desktop (`PhysHeapPagesClean+0x8`, `Comm: QSGRenderThread`).
