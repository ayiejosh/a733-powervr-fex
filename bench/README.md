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

### The windowed path (`gles-x11.c`) — and the per-frame-sync wall
`glbench` never presents. `gles-x11.c` creates a real X11 window, a real EGL window surface and
calls `eglSwapBuffers` every frame, timing draw / `glFinish` / swap separately.
```sh
gcc -O2 gles-x11.c -o /tmp/gles-x11 -lX11 -lEGL -lGLESv2
LD_LIBRARY_PATH=/usr/local/lib DISPLAY=:0 SWAP_INTERVAL=0 /tmp/gles-x11 800 600 64 200
LD_LIBRARY_PATH=/usr/local/lib BLIT=1 SWAP_INTERVAL=0 /tmp/gles-x11 800 600 64 200  # offscreen+copy
glrun /tmp/gles-x11 800 600 64 300                                                  # via zink->Vulkan
```
Measured 2026-09-22 (vendor stack, GPU at 1104 MHz): off-screen FBO **579 Mpix/s** at loop 64, but
the *same shader* pipelined vs one `glFinish()` per frame off-screen is 9 125 → 756 fps (loop 1) and
598 → 21 fps (loop 64). A per-frame sync costs **1.3–48 ms** and scales with the shader; the window
buffer, the swap and the X server were each ruled out (see `../docs/GPU-RESEARCH-2026-09-22.md` §2.3).
Windowed throughput is a flat ≈18 Mpix/s at every resolution; at 1920×1080 with a light shader,
vsync'd, it holds **59.8 fps** — so 1080p60 GPU compositing is feasible, heavy per-pixel work is not.

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

## Memory hierarchy — is the L3/DSU fabric starving you? (`membw.c`)
```sh
gcc -O3 -fopenmp -march=native -ffast-math membw.c -o membw -lm
taskset -c 6 ./membw          # pin to a BIG core (6-7); L3 is per-DSU, not per-core
./membw 8                     # multi-thread: coherency/DRAM under load
# If l3read is barely above dramread, the DSU clock is your bottleneck, not DRAM.
# A733 stock (DSU 780 MHz): l3read 10.2 / l3shared 12.4 / dramread 8.2-9.3 / dramcopy 3.2 GB/s
# This is how the stuck-at-780-MHz DSU was found -> overlays/dsu-clk.dts
```

## Clock sweeping without reboots (`clkctl/`)
A ~60-line debugfs module that exposes `clk_set_rate()` for the GPU and DSU clocks, so an
8-point frequency sweep (and the generator's real ceiling) is measured in **one boot**
instead of eight. See [`clkctl/`](clkctl/) — build lines, usage and caveats are there.

## The clock-work harnesses (what produced `overlays/`)
```sh
sudo insmod clkctl/clkctl.ko && sudo ./gpu-clk-sweep.sh   # GPU ceiling in one boot (restores the boot rate)
sudo ./gpu-stress.sh 1104mhz-990mv                        # is that clock actually stable? (6 iterations, fault-watching)
sudo ./gpu-clock-bench.sh 1104mhz-990mv                   # clock state + glmark2 on both GL paths, one file
sudo ./dsu-ab.sh 1027                                     # memory BW + CPU + GPU + thermals for a DSU setting
sudo ./cpu-oc-sweep.sh                                    # try to beat the spec clock; shows the wall (needs the experiment overlay)
```
| harness | the claim it backs |
|---|---|
| `gpu-clk-sweep.sh` | GPU ceiling is **1104 MHz** — 1152/1200/1248/1296/1344/1392 all report 1104 |
| `gpu-stress.sh` | 1104 MHz @ 990 mV: 58-61 °C, zero driver error lines, X survives |
| `dsu-ab.sh` | DSU 780 → 1027 MHz: `l3read` +19%, `dramread` +38%, FEX thread-start −32%, GPU/compute unchanged |
| `cpu-oc-sweep.sh` | the CPU **cannot** be overclocked here — added OPPs are never offered |
| `gpu-clock-bench.sh` | the overlay is live (`sunxi_parse_dts clk_rate:…`) and glmark2 plateaus before the GPU does |

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

## x87 ReducedPrecision win (pattern-dependent)
```sh
x86_64-linux-gnu-gcc -O2 -static x87l.c -o x87l_x86     # fldl/faddl (64-bit on x87 stack)
# toggle via config file (env FEX_ vars are overridden by ~/.fex-emu/Config.json):
for r in 0 1; do printf '{"Config":{"X87ReducedPrecision":"%s"}}' $r > ~/.fex-emu/Config.json
  FEXInterpreter ./x87l_x86 20000000; done
# X87RP=0 ~75 ns/iter  ->  X87RP=1 ~4 ns/iter  = ~18x  (only for 64-bit-on-x87; not true 80-bit long double)
```
