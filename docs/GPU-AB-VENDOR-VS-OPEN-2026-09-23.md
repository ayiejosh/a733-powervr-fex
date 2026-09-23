# A/B: vendor driver against the open driver at HEAD

One variable: the driver. Same binaries, same board, same clock overlay, both runs inside the same
session (2026-09-23T15:57Z), vendor first because it needs no module swap.

* **A - vendor**: `pvrsrvkm` + `libVK_IMG.so.24.2.6603887` (DDK 24.2), `/usr/share/vulkan/icd.d/img_icd.json`
* **B - open**: mainline `powervr` + Mesa `pvr` at HEAD (94 commits), `build-x11` devenv ICD

Raw: `bench/pvr-vulkan/results-2026-09-23-ab-{vendor,open}.txt`, side by side in
`results-2026-09-23-ab-diff.txt`. Produced with `stackbench.sh`, the same instrument the three-stack
comparison used, extended here with the compatibility cases this round added.

## The headline: functional parity on everything tested

| case | vendor | open | before this round |
|---|---|---|---|
| `bda` buffer device addresses | PASS | PASS | open PASS (previous round) |
| `pctest` push constants | PASS | PASS | open PASS (previous round) |
| `vkbits` 8/16-bit storage | PASS | **PASS** | open **FAIL** - push-constant features missing |
| `io16` 16-bit varying | PASS | **PASS** | open **FAIL** - `vkCreateDevice` returned `VK_ERROR_FEATURE_NOT_PRESENT` |
| `depthclamp` (clamped) | PASS | **PASS** | open **FAIL** - same |
| `depthclamp_clipped` | PASS | **PASS** | open **FAIL** - same |
| `vsstore` vertex-stage SSBO | PASS | **PASS** | open **FAIL** - same |
| `vktest` compute | PASS | PASS | |
| `vkrender` 512 / 1024 / 2048 / 4096 / 8192 | PASS | PASS | |

**Seven of seven compatibility cases pass on both drivers.** The four marked "open FAIL" are the ones
this round closed, and each failure mode was observed directly during the work rather than inferred:
`vkCreateDevice` returning `-8` (`VK_ERROR_FEATURE_NOT_PRESENT`) against a feature that was still `false`
in the table.

`regress.sh` on the open driver is **28 passed, 0 failed, 0 known-open**.

## Performance: unchanged, and still the real gap

| metric | vendor | open | open/vendor |
|---|---|---|---|
| `render512.ms_frame` | 0.690 | 1.755 | **2.54x slower** |
| `render1024.ms_frame` | 1.742 | 4.365 | **2.51x slower** |
| `render2048.ms_frame` | 6.024 | 14.424 | **2.39x slower** |
| `render512.Mpix_s` | 379.7 | 149.4 | 0.39x |
| `compute.ms_dispatch` | 0.787 | 0.878 | 1.12x slower |
| `compute.GBps` | 2.67 | 2.39 | 0.90x |

This is the same shape the earlier benchmarks found and it is unchanged by any of this round's work -
which is the point. **None of the nine storage features, `depthClamp` or
`vertexPipelineStoresAndAtomics` moved a performance number, because none of them is on the path these
tests exercise.** The draw gap is the per-tile cost measured in §19 (`0.37 ms + 0.556 us/tile` against
the vendor's `0.26 + 0.101`), and closing it is a different project from closing the feature table.

## Advertised surface: net four features behind, and four ahead

| metric | vendor | open |
|---|---|---|
| `icd.apiVersion` | 1.3.277 | **1.3.363** (newer) |
| `icd.deviceExtCount` | 114 | 106 |
| `icd.featuresOn` | 55 | 51 |
| `icd.maxImageDimension2D` | 8192 | 8192 |
| `icd.bufferDeviceAddress` | 1 | 1 |
| `icd.colorSampleCounts` | 0x7 | 0x7 |

`featuresOn` counts the `=1` lines `vkaudit` prints; the net difference of 4 decomposes exactly as
§21.20.1 measured it: **8 lines the vendor has and the open driver does not, 4 the open driver has and
the vendor does not.** The four the open driver is ahead on include `VK_EXT_non_seamless_cube_map` and
`VK_EXT_robustness2`, which are two of the six things DXVK degrades on.

## One asymmetry worth stating rather than hiding

`readback.total_ms`, `readback.cpu_ms` and `alloc.ms` are **blank** on the open side and populated on the
vendor side. That is not a parser accident: `stackbench.sh` reads the second host-visible memory type,
and the open driver offers one.

```
open:   PowerVR B-Series BXM-4-64 MC1 (api 1.3.363) - 1 heap(s), 1 memory type(s)
          type 0: heap 0  DEVICE_LOCAL HOST_VISIBLE HOST_COHERENT
vendor: ... 2 memory types, the second cached (6.6 GB/s reads against 330 MB/s)
```

The cached type exists and is now correct in the open driver - `vkFlush`/`vkInvalidateMappedMemoryRanges`
are real dma-buf sync, and GL through zink with it on is 262144/262144 correct at 2613 MB/s - but it is
**opt-in behind `PVR_ENABLE_CACHED_MEMORY_TYPE=1` and off by default** (§21.16). So there is no readback
number to compare until that policy question is answered, and the honest thing is to leave the row empty
and say why.

## What the A/B says about what we were chasing

Two conclusions, and the second is the uncomfortable one.

1. **The compatibility work did what it was supposed to.** The open driver went from "cannot create a
   device for four of these tests" to passing all seven, and it is now *ahead* of the vendor on API
   version and on two extensions DXVK specifically degrades on. That is real, and it is the part worth
   upstreaming.

2. **None of it is what makes games run better on this board.** The vendor already passed all seven
   cases before this round started; what the round bought is the ability to *not need the vendor*, not
   the ability to run anything the vendor could not. The remaining work that would change what a user
   experiences is performance (§19) and the module-ownership problem - the desktop needs `pvrsrvkm`, and
   only one driver can own the GPU - not the seven remaining features.
