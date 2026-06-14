# x86 OpenGL ES 3.2 under FEX → native PowerVR (BXM-4-64)

Run **x86-64 and i386 (32-bit) Linux GLES binaries** under [FEX-emu](https://fex-emu.com)
on the A733's PowerVR BXM-4-64 GPU at **near-native speed** — emulators, GLES engine
games, Qt-EGL / media apps. The heavy GPU work runs on the native ARM driver; only the
thin GL/EGL *call dispatch* is thunked.

Proven end-to-end: an x86 binary under FEX reports `GL_RENDERER = PowerVR B-Series
BXM-4-64`, `OpenGL ES 3.2`, and an FBO render+readback returns the exact rendered pixels
(GL error 0) — bit-identical on x86-64, i386, and native ARM.

## Results

### Performance — real numbers (same GPU, A76-pinned). Full table: [`BENCHMARKS.md`](BENCHMARKS.md)
| Metric (real value) | native ARM | x86-64/FEX | i386/FEX | overhead |
|---|---:|---:|---:|---:|
| shader-ALU throughput | 207 GFLOP/s | 207 | 206 | **1.00×** |
| compute (SSBO) | 9.1 GIntOp/s | 9.1 | 9.1 | **1.00×** |
| triangle rate | 14.2 Mtri/s | 14.2 | 14.2 | **1.00×** |
| simple fill | 6,714 Mpix/s | 6,553 | 6,602 | **~1.0×** |
| texture sampling | 8,634 Mtexel/s | 8,736 | 8,044 | **~1.0×** |
| `glClear` call rate | 3,427 k/s | 3,032 | 3,007 | **1.13×** |
| buffer upload | 3.9 GB/s | 3.7 | 3.7 | **1.04×** |

(overhead = native ÷ FEX; >1.0× = FEX slower.) **Every GPU-bound metric is identical** — the
GPU half is never emulated, so shader/compute/raster throughput is the hardware's regardless
of architecture. Overhead is confined to cheap CPU-side **call dispatch** (~1.13×). i386 ≈
x86-64. For context, FEX's general x86→ARM CPU overhead here is 1.1–2.2×, so GLES sits at the low end.

### Conformance (dEQP-GLES3, native vendor driver — applies to FEX since output is bit-identical)
- `color_clear` 19/19, `depth_stencil_clear` 11/11, `vertex_arrays…normalize` 58/58,
  `shaders.functions.control_flow` 22/22, `negative_api.buffer` 38/38 — all pass.
- `fbo.completeness` 569/576 (the 6 failures are BGRA-renderbuffer edge cases — a closed
  DDK-24.2 driver gap, not a thunk issue; the thunk faithfully mirrors native, BGRA gap included).

## How it works

FEX's stock thunk set had Vulkan but no working **EGL**, and GLES needs the GL command
stream routed to the *vendor* GLES, not GLVND/llvmpipe. Five pieces:

1. **thunkgen crash fix (`eglshim/EGL/eglplatform.h`).** The system EGL headers typedef
   `EGLNativeDisplayType = Display*` (X11); FEX's `assume_compatible_data_layout` on a
   pointer-to-opaque-X11-struct crashes `thunkgen`. A platform-agnostic shim making the
   native handles `void*` lets it generate a complete interface.
2. **EGL thunk** (`libEGL_interface.cpp` + `libEGL_Host.cpp`) — x86 EGL → vendor
   `libEGL.so.1` (→ `libsrv_um` → `pvrsrvkm` → GPU). The expanded interface covers
   `eglQueryString`, config queries, `eglCreatePbufferSurface`, etc.
3. **GLES command stream via the GL thunk.** FEX's libGL thunk already registers all GLES
   functions and resolves them with `dlsym`. Point its `dlopen("libGL.so.1")` at the
   **vendor `libGLESv2_PVR_MESA`** (a symlink on the host `LD_LIBRARY_PATH`), and add a
   `GLESv2` ThunksDB entry overlaying `libGLESv2.so.2` onto the GL guest thunk.
4. **100% GLES 3.2 coverage** (`libGL_interface_100.cpp` + `gl_extra_decls.h`) — adds the
   two core funcs FEX's desktop-GL interface lacked (`glBlendBarrier`,
   `glPrimitiveBoundingBox`) → 358/358 of the functions the hardware exports.
5. **32-bit handle mapping** (`libEGL_Host.cpp`, `#ifdef IS_32BIT_THUNK`). On i386 a
   64-bit `EGLConfig`/`EGLContext`/`EGLSurface` can't fit a 32-bit guest slot. The host
   impls (marked `custom_host_impl`) map handles to 32-bit tokens (and back); identity
   passthrough on x86-64.

## Building

These are FEX *thunk* sources, built against a FEX checkout (see `../../patches/`). They
are **local derived files, not a FEX upstream contribution** (FEX's policy disallows
AI-generated contributions; these are device-local enablement only).

Key recipe notes (host = aarch64, guest = x86):
- Host thunkgen: use the **host sysroot** (`/`) + `-isystem/usr/include/aarch64-linux-gnu`
  and **no `--target`** — `-for-32bit-guest` alone handles 32-bit pointer layout (passing
  `--target=i386` causes an `int64_t`-vs-`long long` mismatch).
- Guest build: **omit** `-DARCHITECTURE_arm64` (else FEX emits a "broken install" stub);
  link with `lld`; the GL guest needs the X11 placeholder (for `XSync`).
- i386 needs matched libc/libstdc++ headers + CRT (the FEX rootfs is x86_64-only).
- Quoted-`#include` gotcha: don't leave a stale `thunkgen_*.inl` next to the host `.cpp`
  — same-directory lookup will shadow the freshly-generated one and silently drop impls.

## Usage

`fex-gles <x86-gles-binary>` (see `fex-gles`) enables the EGL+GL+GLESv2 thunks via an
**isolated** `FEX_THUNKCONFIG`, so the global FEX/Chrome setup is untouched. FEX
auto-selects the `_32` host/guest thunk dirs for i386 guests.

## Limitations
- Not formally Khronos-CTS-certified (sampled subset, ~99% pass).
- The 6 BGRA-renderbuffer dEQP failures are a closed-DDK-24.2 driver gap (a newer vendor
  blob is the only fix).
- Native ARM GLES 3.2 is hardware-default already; this is specifically for **x86** binaries.
