# Mesa pvr on the Radxa Cubie A7A (Allwinner A733, PowerVR BXM-4-64)

Mesa's `pvr` Vulkan driver is the only userspace that can drive the **mainline**
`powervr` DRM driver. Getting it to run on this board needed two things beyond a
stock build, because this GPU (BVNC 36.56.104.183) sits in a gap between Mesa
releases. Both are in this directory.

## Which Mesa version

| version | knows BVNC 36.56.104.183 | device enumeration | builds without LLVM | verdict |
|---|---|---|---|---|
| 25.0.7 | **no** — ships only `axe-1-16m`, `bxs-4-64`, `gx6250` | DT `compatible` table | yes | driver also stubs its shader compiler (`pco_nir.c` has 4 `finishme`s, plus `pvr_hardcode.c`) |
| 25.1 / 25.2 | no | DT `compatible` table | yes | same gap |
| **25.3.0** | **yes** — `device_info/bxm-4-64.h` | DT `compatible` table | no: pvr needs CLC → LLVM + LLVMSPIRVLib + libclc + SPIRV-Tools | works; see build below |
| main | yes | capability-based (DRM name + dumb-buffer/PRIME caps), no table | no (same CLC chain) | no patches needed |

`imagination-uscgen-devices` does not list `bxm-4-64`, so no USC programs are
pre-built for this GPU; 25.3.0 compiles them at runtime, which is why 25.0.7's
hard-coded-program path had to go.

## Patches

- `0001-pvr-add-A733-img-gpu-platform.patch` — adds
  `DEF_CONFIG("img,gpu", "allwinner,sunxi-drm")` to `pvr_drm_configs[]`. Without
  it the driver enumerates **zero** devices on this board, because the table is a
  DT-`compatible` whitelist and this device tree names the GPU `img,gpu` and the
  display engine `allwinner,sunxi-drm`. Needed by 24.3–25.3; main has no table.
- `0002-pvr-backport-BXM-4-64-device-info.patch` — only if you must stay on
  25.0–25.2: backports the 25.3.0 device-info blob for this BVNC. `struct
  pvr_device_info` is byte-identical between 25.0.7 and 25.3.0, so the copy
  compiles; entries that 25.0.7 cannot represent are removed and listed in the
  file. On 25.3.0 this patch is unnecessary.

## Building 25.3.0 here

The CLC chain is the awkward part; on this board it is satisfiable from what
Debian already has except for two pieces:

```sh
# 1. LLVMSPIRVLib 19.1.x (Mesa wants >=19.1, <19.2; this board has LLVM 19.1.7)
git clone --depth 1 -b v19.1.15 https://github.com/KhronosGroup/SPIRV-LLVM-Translator
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_DIR=/usr/lib/llvm-19/lib/cmake/llvm -DLLVM_SPIRV_INCLUDE_TESTS=OFF \
      -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j2 && sudo cmake --install build && sudo ldconfig   # -j2: see note

# 2. libclc bitcode + the pkg-config file Mesa looks up
apt-get download libclc-19 && sudo dpkg-deb -x libclc-19_*.deb /
printf 'prefix=/usr\nlibexecdir=/usr/lib/clc\n\nName: libclc\nDescription: OpenCL C library bitcode\nVersion: 19.1.7\n' \
  | sudo tee /usr/local/lib/pkgconfig/libclc.pc

# 3. Mesa itself (llvm-config must be on PATH for Meson's config-tool lookup)
ln -sf /usr/bin/llvm-config-19 ~/bin/llvm-config
PATH=~/bin:$PATH PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig \
  meson setup build -Dbuildtype=release -Dvulkan-drivers=imagination -Dimagination-srv=true \
    -Dgallium-drivers= -Dopengl=false -Dglx=disabled -Dplatforms= -Dgbm=disabled \
    -Dglvnd=disabled -Dvulkan-layers= -Dvideo-codecs= -Dvalgrind=disabled -Dtools= -Dbuild-tests=false
PATH=~/bin:$PATH PKG_CONFIG_PATH=... ninja -C build -j3
```

Two things that cost time here: the build needs `export PYTHONPATH` pointing at a
site-packages with `mako` if it was pip-installed rather than packaged, and
`-j8` got a compiler killed by memory pressure on this 5.8 GB board — `-j2`/`-j3`
is the reliable setting.

Build outputs used by the tests: `build/src/imagination/vulkan/libvulkan_powervr_mesa.so`
and `powervr_mesa_devenv_icd.aarch64.json` (the **devenv** manifest points at the
build directory; the other manifest points at the install prefix).

## Diagnostic instrumentation

The gates above were found with temporary `fprintf` traces in
`pvr_device.c` (device enumeration, `pvr_winsys_create`, `device_info_init`) and
in `winsys/powervr/pvr_drm.c` (the BVNC the winsys read), enabled by
`PVR_TRACE=1`. They are not part of either patch — Mesa's own
`mesa_loge`/`vk_errorf` messages are the durable signal.

## What must be true around it

The vendor `pvrsrvkm` module and the mainline `powervr` module cannot both own
`1800000.gpu`. See `../kernel/open-driver-spike/stage4-mainline-vulkan.sh` for the
swap, which stops `display-manager.service`, drops the vendor module's 188
references in under a second, runs the test, and restores — desktop included,
without a reboot.
