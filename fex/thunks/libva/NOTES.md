# libva thunk for FEX — status & build/marshalling plan

Purpose: let the x86 CRD process (under FEX) reach the **native ARM** `libva` + our
`sunxi_ve` VE2 H.264 driver, so CRD's encoder runs on the Cedar hardware.

## Design (settled)
- **VADisplay** (`void*`) is opaque to the client → guest holds the *host* pointer as a
  token, passes it back unchanged. No struct marshalling.
- **All VA*ID handles** (Config/Context/Surface/Buffer/Image) are `unsigned int` → marshal
  as integers for free.
- **libdrm is already thunked by FEX**, and VAAPI/`vaGetDisplayDRM` sit on a DRM fd → the
  fd is valid host-side via the existing libdrm thunk. Compose with it.
- Most VA functions are scalar + pointer-to-struct args → auto-marshalled by the generator
  (like libdrm's 60+ functions).

## The one hard part: `vaMapBuffer` / `vaUnmapBuffer`
`vaMapBuffer` returns a pointer into host-allocated buffer memory; the guest then writes
(input frame upload) or reads (coded H.264 output). A host pointer isn't usable in the x86
guest address space. Marked `custom_host_impl` + `custom_guest_entrypoint`:
- **map**: host maps the buffer, allocates guest-visible memory, copies host→guest, returns
  the guest pointer; remember the (guest ptr ↔ host buf) pair.
- **unmap**: copy guest→host (for input/param buffers), then host `vaUnmapBuffer`.
This is copy-on-map (correct, not zero-copy). Zero-copy later via dmabuf + the libdrm thunk.

## Build wiring (to add)
GuestLibs/CMakeLists.txt:  `generate(libva ${CMAKE_CURRENT_SOURCE_DIR}/../libva/libva_interface.cpp)`
                           `add_guest_lib(va "libva.so.2")`  (also libva-drm if split)
HostLibs/CMakeLists.txt:   add the matching host target.
Files: libva/Host.cpp (`EXPORTS(libva)`), libva/Guest.cpp (`LOAD_LIB(libva)`),
       libva/libva_interface.cpp (this dir).

## Remaining work (multi-day)
1. Write Host.cpp/Guest.cpp + custom map/unmap impls; wire CMake.
2. Build FEX guest(x86)+host(ARM) thunk libs (FEX thunk build pipeline).
3. Place guest `libva.so.2`/`libva-drm.so.2` thunks in CRD's x86 rootfs; register in ThunksDB;
   host thunk + real ARM libva + sunxi_ve driver on the host side.
4. Test: run an x86 `vainfo`/encode sample UNDER FEX → must reach the VE2 (mirrors the native
   test_encode.c that already works).
5. Then Component C: Chromium/CRD VEA selection (g_rtc_use_h264, VaapiVideoEncoder, sandbox).

## What already works (native, no thunk)
`~/ve2-vaapi/` — the VAAPI driver encodes valid H.264 on the VE2 (native ARM, validated).
This thunk is the bridge to make that reachable from the x86 CRD process.
