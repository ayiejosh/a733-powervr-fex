# Open Mesa as the system GL/EGL/GBM/Vulkan while the open driver is bound

The open stack (open `powervr` kernel driver + the Mesa in `../../mesa/`) is installed next to the
vendor stack, in its own prefix, and switched on only while the open kernel driver is bound.

Why system configuration and not environment variables: `kwin_wayland` has file capabilities
(`cap_sys_nice`), so it runs in secure mode. The dynamic linker then ignores `LD_LIBRARY_PATH` and the
Vulkan loader ignores `VK_ICD_FILENAMES`, and kwin would load the vendor `libgbm`/`libEGL` from
`/usr/local/lib` (listed by `/etc/ld.so.conf.d/00_xserver-xorg-img-bxm.conf`).

- **`open-stack-libs.sh on|off|status`** (`/usr/local/sbin/`). `on` writes
  `/etc/ld.so.conf.d/00-mesa-open.conf` (the open Mesa, then the system library directories; it sorts
  before the vendor entry), copies the open Vulkan ICD to `/etc/vulkan/icd.d/`, sets
  `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1` in `/etc/environment.d/`, and runs `ldconfig`. `off` removes
  all three. `open-driver-perf/tools/verify/switch-safe.sh` calls `on` after the open driver binds and
  `off` before the vendor stack comes back.
- **`mesa-open-boot-check.service`** runs `open-stack-libs.sh off` at boot, before the display
  manager, unless the open driver is bound. The board boots on the vendor driver, so a switch left
  on (power loss, a reboot from the wedge guard) never reaches the vendor desktop.

With the open driver bound, Plasma Wayland's kwin opens GBM on the display device (`card0`,
`sunxi-drm`). No driver matches it, so Mesa falls back to kmsro: zink renders on the PowerVR and KMS
dumb buffers are scanned out. No Mesa change is needed for that. The vendor X11 session's
`MESA_LOADER_DRIVER_OVERRIDE=zink` must not reach that session (GBM on `card0` fails with it).

## Install

```sh
# Mesa from the PR #12 tree, in its own prefix (glvnd off: it ships its own libEGL/libgbm)
meson setup build-open --prefix=/opt/mesa-open -Dlibdir=lib/aarch64-linux-gnu -Dbuildtype=release \
  -Dgallium-drivers=zink,pvr -Dvulkan-drivers=imagination -Dplatforms=wayland,x11 -Dgbm=enabled \
  -Dglvnd=disabled -Dglx=dri -Dvideo-codecs= -Dvalgrind=disabled -Dteflon=false -Dbuild-tests=false \
  -Dgallium-rusticl=false
ninja -C build-open && sudo ninja -C build-open install
# meson also installs its bundled expat and zlib; they are older than the system ones
sudo rm -f /opt/mesa-open/lib/aarch64-linux-gnu/libexpat.so* /opt/mesa-open/lib/aarch64-linux-gnu/libz.so*
sudo install -m 755 open-stack-libs.sh /usr/local/sbin/
sudo install -m 644 mesa-open-boot-check.service /etc/systemd/system/
sudo systemctl enable mesa-open-boot-check
```

Undo: `sudo systemctl disable mesa-open-boot-check`, remove the two files, `sudo rm -rf /opt/mesa-open`.
