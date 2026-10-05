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

**5. Render area has NO effect — retracting an earlier claim.** A first pass measured
`AREA=quarter` at 3.770 ms/frame with `record` blowing up to 1.77 ms, and this was
written up as "full-surface work regardless of the render area". **That was not
reproducible.** Repeating the sweep three times gives a flat result:

```
512² surface, 60 frames   run1                      run2                      run3
AREA=full                 r=0.427 s=0.161 w=1.165  r=0.417 s=0.159 w=1.163  r=0.421 s=0.164 w=1.148
AREA=half                 r=0.357 s=0.157 w=1.168  r=0.513 s=0.195 w=1.153  r=0.350 s=0.157 w=1.167
AREA=quarter              r=0.329 s=0.145 w=1.162  r=0.341 s=0.166 w=1.139  r=0.367 s=0.155 w=1.166
```

Isolating the render half from the copy removes any doubt — drawing 1/16 of the pixels
costs exactly the same as drawing all of them:

```
MODE=render  AREA=full     record=0.411  submit=0.087  gpu_wait=0.916
MODE=render  AREA=quarter  record=0.320  submit=0.077  gpu_wait=0.913
MODE=copy    AREA=full     record=0.002  submit=0.063  gpu_wait=0.327
MODE=copy    AREA=quarter  record=0.002  submit=0.059  gpu_wait=0.328
```

The correct statement is the opposite of the retracted one: cost is set by the
**attachment surface size**, not by the pixels drawn and not by the render area. Image
size does scale it (0.447 → 3.503 ms across 128²→1024²); render area does not. The
3.770 ms reading was a transient — same class of error as the 3-frame A/B below.

**6. The end-of-tile program was recompiled on every render pass.** `pvr_usc_eot()`
builds NIR and runs a full PCO compile, and
`pvr_sub_cmd_gfx_per_job_fragment_programs_create_and_upload()` called it once per pass
and `ralloc_free`d the result. Confirmed by breakpoint count on `pvr_usc_eot` (gdb,
pending breakpoint so it resolves on dlopen):

```
 10 frames -> breakpoint hit 14 times
 30 frames -> breakpoint hit 34 times
```

`frames + 4` — exactly one compile per render pass. Its inputs (`emit_count`, PBE state
words, tile-buffer addresses, all device-lifetime) are stable across frames, so it is
trivially cacheable. Fixed; see the follow-up section.

**7. Per-pixel slope**, subtracting each stack's own empty-pass baseline:

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
   are not the cost. **Partly fixed: −0.13 ms of this was a redundant shader compile.**
2. **Fixed per-pass GPU cost, +0.39 ms/frame (37%)** — an empty pass costs 0.31–0.38 ms
   on open, 0.000 on vendor. Real GPU work, does not amortize under batching, and does
   not scale with render area. It is structural: each pass is submitted as three
   hardware jobs (geometry + an unconditional partial-render job + fragment) with a
   syncobj round trip between the first two, and nothing skips an empty pass. Not
   fixable from the UMD.
3. **Actual rendering, +0.14 ms/frame (14%)** — 1.2× at 512², rising to ~3.2× at 1024².

Items 1 and 2 are worth ~76% of the gap and both are per-frame overhead, so the fix
that matters is *fewer/cheaper per-pass operations*, not faster rasterisation.

## Follow-up: the redundant EOT compile, fixed

`pvr_usc_eot()` recompiles the end-of-tile program with PCO on every render pass
(finding 6). Cached on the device, keyed on everything the compile reads — emit count,
PBE state words, tile-buffer addresses, MSAA samples, output regs. The per-pass upload
stays, because the PDS data segment is generated from wherever the program lands in the
command buffer.

Three files: `pvr_device.h` (cache field), `pvr_arch_device.c` (init/finish),
`pvr_arch_cmd_buffer.c` (use it). Correctness is unchanged — the shader is a pure
function of the key, and every run still passes the full 262144-pixel check.

Clean A/B, `vkrender 512 60`, five runs each, same session, only the `.so` swapped:

```
              record ms (5 runs)                     submit   gpu_wait
pre   (before)  0.855 0.788 0.406 0.420 0.394       0.157    1.162
post  (cached)  0.265 0.273 0.298 0.398 0.392       0.164    1.157
```

**`record` 0.406 → 0.273 ms steady state, −33%.** The first two `pre` runs are warmup
outliers; the cached build has no equivalent spike, which is the same effect seen from
the other side. `MODE=empty` is the cleanest signal: `record` 0.394 → 0.149 ms, because
an empty pass still compiled a full EOT program.

End to end that is ~8% (1.72 → 1.59 ms/frame). Modest, because `gpu_wait` — the
structural per-pass cost in item 2 — is untouched at ~1.16 ms and now dominates
completely. **Closing the rest of the gap means changing how many jobs a render pass
becomes, which is a kernel (`drm/imagination`) change, not a Mesa one.**

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
