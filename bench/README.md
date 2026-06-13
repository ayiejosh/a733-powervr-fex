# Benchmarks — reproduce & contribute

These are the exact harnesses behind [`../docs/BENCHMARKS.md`](../docs/BENCHMARKS.md).
Run them on your board and **submit your numbers** (PR to [`../RESULTS.md`](../RESULTS.md)
or open a "Benchmark result" issue) so we build a cross-board baseline.

> Needs the vendor GPU stack in place (`../install.sh vendor`) for the GPU/Vulkan ones.

## GPU vs CPU throughput (the headline 150–600× number)
```sh
# GPU — GLES FBO ALU-loop shader, Mpix/s
gcc glbench.c -o glbench -lEGL -lGLESv2 -lgbm -ldl
LD_LIBRARY_PATH=/usr/local/lib ./glbench /dev/dri/renderD128 <loop> <frames>
#   <loop> = ALU iterations per pixel (try 4 16 64 256), <frames> e.g. 300

# CPU — same math, OpenMP 8-core, for the comparison baseline
gcc -O3 -fopenmp -march=native cpubench.c -o cpubench -lm
./cpubench <loop> <frames>
```

## GPU present & usable (Vulkan ICD probe)
```sh
gcc vkprobe.c -o vkprobe -ldl
LD_LIBRARY_PATH=/usr/local/lib XDG_RUNTIME_DIR=/run/user/$(id -u) ./vkprobe
# expect: "PowerVR B-Series BXM-4-64 MC1", INTEGRATED_GPU, Vulkan 1.3, device-local MB
```

## Validate the kernel PRIME-import patch (`../kernel/`)
Proves the patched `pvrsrvkm` actually imports foreign dma-bufs and renders them.
```sh
# build lines are in each file header:
gcc dmabuf_render_test.c  -o dmabuf_render_test  $(pkg-config --cflags --libs gbm)        -lvulkan -ldl
gcc dmabuf_foreign_test.c -o dmabuf_foreign_test $(pkg-config --cflags --libs libdrm gbm) -lvulkan
LD_LIBRARY_PATH=/usr/local/lib VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json ./dmabuf_render_test
LD_LIBRARY_PATH=/usr/local/lib VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json ./dmabuf_foreign_test
# expect: drmPrimeFDToHandle OK (not ENOSYS) + GPU readback matches (e.g. 65536/65536)
# foreign_test allocates from /dev/dma_heap/system to exercise the non-self-import path.
```

## H.264 hardware encode (VE2)
```sh
gcc enctest_h264.c -o enctest_h264 -lva -lva-drm   # adjust libs to your VAAPI setup
./enctest_h264
```

## CPU / RAM / storage (standard tools)
```sh
sysbench --test=cpu  --cpu-max-prime=20000 --num-threads=1 run   # single-core
sysbench --test=cpu  --cpu-max-prime=20000 --num-threads=8 run   # all cores
sysbench --test=memory --memory-block-size=1M --num-threads=8 run
fio --name=r --rw=read    --bs=1M  --direct=1 --size=2G --filename=/tmp/fio.bin
fio --name=w --rw=randwrite --bs=4k --direct=1 --size=1G --filename=/tmp/fio.bin
```

`GPU_BENCHMARK.md` is the original raw GPU run for reference.
**Methodology note:** these are indicative single-board numbers; report your kernel,
DDK version (`strings /usr/lib/libVK_IMG.so* | grep -m1 24.`), board, and ambient temp.

## Unaligned-atomic overhead (the "187× myth")
```sh
x86_64-linux-gnu-gcc -O2 -static uatomic.c -o uatomic_x86      # cross-compile the x86 bench
FEXInterpreter ./uatomic_x86 30000000 0    # aligned   (baseline)
FEXInterpreter ./uatomic_x86 30000000 2    # unaligned (<16B)
FEXInterpreter ./uatomic_x86 30000000 14   # split-lock (crosses 16B)
# STOCK upstream FEX on A733: aligned ~154 Mops, unaligned ~0.70 (≈190x slower, ≈1430 ns/op)
#   — it SIGBUS-traps per op (no FEAT_LSE2/uscat). Config knobs do NOT change this.
# A local FEX codegen patch (Arm64.cpp backpatch fix) cuts unaligned to ~61 Mops (~2.5x).
# So your number depends on which FEX you run — report stock vs patched.
```
