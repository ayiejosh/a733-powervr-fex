# box64 on A733

[box64](https://github.com/ptitSeb/box64) runs x86-64 Linux userspace on ARM64.
Nothing here is forked — build upstream and use it as-is. Tested build on this
board: **box64 v0.4.3, dynarec enabled.**

```sh
git clone https://github.com/ptitSeb/box64
cd box64 && mkdir build && cd build
cmake .. -DARM_DYNAREC=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc) && sudo make install
```

## Notes specific to A733 / PowerVR

- For **GPU** workloads under box64, pair it with the native Vulkan ICD
  (`/usr/share/vulkan/icd.d/img_icd.json`) — Vulkan apps reach the real GPU.
  OpenGL apps need the Zink stack (see `../gpu/`).
- **DXVK/Proton is a dead end here** (PowerVR missing DXVK Vulkan extensions),
  same as under FEX — see `docs/FINDINGS.md`. box64+wine is fine for non-3D x86-64
  apps; don't expect DirectX games.
- FEX vs box64: FEX has the custom Vulkan GPU thunk in this repo (`../fex/`); box64
  uses its own wrapped-lib mechanism. Pick per app/compatibility.
