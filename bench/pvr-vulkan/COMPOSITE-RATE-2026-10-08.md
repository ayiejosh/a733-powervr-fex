# weston's composite runs at 30 Mpix/s - 10x slower than a fill on the same driver

## Numbers (1080p, directly measured)

```
client area            2.07 Mpix
client render (model)   7.4 ms      (vkrender model: 0.93 + 3.11*Mpix)
composited frame       76.9 ms      (13 FPS)
=> compositor term     69.5 ms  =  29.8 Mpix/s
```

Same driver, same session:

| workload | rate | vs fill |
|---|---|---|
| fill (vkrender 2048) | 298 Mpix/s | 1.0x |
| sample (vktex 2048) | 200 Mpix/s | 1.5x slower |
| **weston composite** | **30 Mpix/s** | **10x slower** |

## Buffer path exonerated

* **dma-buf works**: pvr advertises `KHR_external_memory_fd` + `EXT_external_memory_dma_buf` and
  implements `pvr_GetMemoryFdKHR` for `DMA_BUF_BIT_EXT`, so `wsi_init_image_dmabuf_fd()` returns a
  real fd and X11's DRI3 path is available - no SHM fallback.
* images are linear (the simple case; sampling linear costs only 1.5x).
* zink submits one geom+PR+frag job set per frame, no extra copy (`PVR_JOB_TRACE`).

## Six hypotheses refuted by measurement

| hypothesis | refuted by |
|---|---|
| vsync caps the client | 113 FPS at 160x120 |
| Xwayland copies the frame | profile: no write/SHM/memcpy |
| tiled buffers need a slow detile | images are linear-only |
| linear sampling is slow | 1.5x fill (`vktex`) |
| zink adds a presentation blit | `PVR_JOB_TRACE`: 3.00 jobs/submit |
| no dma-buf so SHM fallback | `EXT_external_memory_dma_buf` supported |

## Conclusion and next instrument

A composite pass 10x slower than a fill, with the memory path proven fine, points at **how the driver
executes weston's compositing shader** - PCO's compilation of it, or the fixed-function state it uses
(blending, colour management, MRT).

In scope, and instrumentable with tooling already built: dump that shader through PCO
(`PCO_DEBUG_PRINT=passes,fs,internal,vs,nir,binary`) and look for a pathological lowering - the same
approach used for the fp16 and limit work. A 10x gap on a shader the vendor driver runs fast is the
shape of a compiler problem.
