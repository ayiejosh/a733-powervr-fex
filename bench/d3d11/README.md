# bench/d3d11 — the Direct3D 9/10/11 harness

Thirty self-contained x86-64 Windows programs that exercise the shipping D3D stack
(wine → DXVK-Sarek arm64ec → PowerVR Vulkan) and the geometry-shader emulation. They are
x86-64 PE on purpose: that is the path an application takes, not a native shortcut.

```sh
./build.sh                  # cross-build into /home/radxa/gpu-experiment/mind3d
./run.sh matrix             # correctness matrix, with expectations
./run.sh timed              # the timed suite behind docs/BENCHMARKS.md
GS_DLL=… ./run.sh gsab      # geometry-shader emulation A/B (swaps a dll, restores it)
```

`bench/full.sh d3d` runs `run.sh all` for you and keeps the logs in one place.
`build.sh` needs the mingw-w64 cross toolchain (`sudo apt-get install -y mingw-w64`);
the pre-built exes live outside this repo because they are binaries.

## The matrix (`run.sh matrix`)

Each row must exit 0 **and** print its marker. Five rows are expected to fail for reasons
that are not ours — they fail identically with the session Vulkan layer and fakes removed,
and on the vendor driver:

| app | proves | expected |
|---|---|---|
| `tri.exe` | 6000 windowed frames, software-llvmpipe window + GPU render | `RENDERED_6000_FRAMES_OK` |
| `cube.exe` | the BC-texture path in a windowed app (also the GS-dll regression canary) | `CUBE_DONE` |
| `tex.exe` `depth.exe` `rtt.exe` `mrt2.exe` | sampling, Z, render-to-texture, two render targets | `*_OK` |
| `compute.exe` | compute dispatch + readback verification | `COMPUTE_OK` |
| `cgs.exe` `cgs2.exe` | compute-amplified indirect draw, and a counter-driven variable-count one | `COMPUTE_GS_ARCH_OK`, `DYN_AMPLIFY_OK` |
| `bctex` `bc2t` `bc4t` `bc5t` `bcbench` `bcdxvk` `bcdxvk3` | BC1–BC5 decode (the shipped patch) incl. a real BC1 texture through DXVK | `*_OK` |
| `bench` `bench2` `bench_flip` `drawbench` | windowed present, instancing, flip, per-draw state | `RESULT` |
| `min_d3d11.exe` | the smallest possible present loop | `PRESENTED_600_FRAMES_OK` |
| `mrt.exe` | **known-open**: `SV_VertexID` with no input layout / no vertex buffer | `MRT_FAIL` |
| `msaa.exe` `msaa2.exe` | **known-open**: the BXM blob has no MSAA | `MSAA_FAIL` |
| `tess.exe` | **known-open**: the blob has no tessellation | rc≠0 |
| `d7test.exe` `d3d7test.exe` | **known-open**: there is no D3DHALDevice for D3D7 | rc≠0 |

## The timed suite (`run.sh timed`)

`drawbench` isolates per-draw CPU state cost (baseline, PSO swap, cbuffer, SRV, vertex
buffer, DrawIndexed, DrawInstanced); every frame ends in `CopyResource`+`Map`, so each
number includes a GPU finish. `realbench r` splits CPU-record from GPU-finish over a
realistic textured/depth-tested frame and sweeps the draw count; `realbench p` prices a
windowed present. `bench`/`bench2`/`bench_flip` are the windowed fps numbers. `bcbench`
prices BC1 decode.

## Reading the numbers

* Only **back-to-back, same-session** comparisons are evidence. Absolute values drift
  ~±10% between sessions with ambient load; a repeat inside one session is stable to
  ~0.2% (`drawbench a` repeated: 4.690 / 4.697 µs/draw).
* The first run after a dll swap is ~20% slow (cold shader cache) — discard it.
* The arm64ec link is not bit-reproducible: two builds of the same source differ. Compare
  behaviour (the gate rows above), not hashes.
* `run.sh gsab` is the only mode that writes to the wine prefix. It copies the deployed dll
  aside first and restores it on exit — check `md5sum` of the deployed dll afterwards if you
  ran it by hand.
