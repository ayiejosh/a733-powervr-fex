# Acknowledgments

## Built on
This work stands on these projects and vendor sources:

- **The Linux kernel** — the DRM PRIME / GEM subsystem; the `pvrsrvkm` patch's
  refcount/lifetime contract was verified against `drm_prime.c` / `drm_gem.c`.
- **Imagination Technologies** — the PowerVR Rogue DDK (the proprietary GPU driver
  these patches apply *on top of*; not redistributed here).
- **Radxa** — the Cubie A7A/A7S boards, the Debian BSP, the `a733-bullseye` apt repo,
  and the `img-bxm-dkms` / `xserver-xorg-img-bxm` packaging.
- **Allwinner** — the A733 (`sun60iw2`) SoC and the AIoT BSP.
- **FEX-Emu** (<https://github.com/FEX-Emu/FEX>) — x86→ARM64 emulation and the
  thunk framework the Vulkan GPU thunk extends.
- **box64** by ptitSeb (<https://github.com/ptitSeb/box64>).
- **Mesa / Zink** — GL-over-Vulkan, the route to GPU OpenGL on this stack.
- **sway + wlroots** and **wayvnc** — the GPU-composited Wayland desktop + its VNC.

## Related A733 work (parallel efforts worth following)
These were not dependencies of this repo, but they're the rest of the A733/Cubie
community working the same SoC — credit to them, and they're good to track:

- **NickAlilovic** — Armbian A733/A7A community build (`build` repo, branch
  `Radxa-A7A`); an A733 bring-up reference.
- **GameOctane / OctaneOS** (<https://github.com/GameOctane/OctaneOS>) — a Batocera
  fork for the Cubie A7S; first community build on the 6.6 BSP with a compiled
  `pvrsrvkm.ko`.
- **crescenzo77** — mainline A733 / Cubie A7S DTS upstreaming (the path that would
  eventually lift the hard GPU walls).
- **Orange Pi** — `linux-orangepi` branch `orange-pi-5.15-sun60iw2` (fuller A733
  CCU / display / USB-C DP / combo-PHY support).
- **dok2d** — `cubie-a7z-debian` (reproducible Debian Trixie on Linux 6.6.98).

---
*If you contributed or your work belongs here, open a PR adding yourself.*
